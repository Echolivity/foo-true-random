// Playlist transaction logic tests against an in-memory fake playlist manager
// with failure injection. The fake models the relevant foobar2000 semantics:
// indexed playlists, active playlist, append, remove, reorder
// (new[k] = old[order[k]]), locks and an undo stack.
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "ftr/playlist_logic.h"
#include "mock_provider.h"
#include "test_harness.h"

using namespace ftr;
using namespace ftr_test;

namespace {

using handle = std::string;

struct fake_pm : playlist_backend<handle> {
    struct pl {
        std::string name;
        std::vector<handle> items;
        std::vector<std::vector<handle>> undo;
        bool reorder_locked = false;
    };
    std::vector<pl> lists;
    std::size_t active = npos;

    // failure injection
    bool fail_create = false;
    bool fail_append = false;
    bool partial_append = false;   // inserts half and still reports success
    bool fail_remove = false;
    bool fail_reorder = false;
    bool create_changes_active = false;  // simulate a core that activates new playlists
    bool throw_in_append = false;        // SDK call throws after the playlist exists
    bool throw_in_get_items = false;     // verification read throws
    bool foreign_insert_during_append = false;  // another component adds a playlist at index 0
    bool ignore_activation = false;      // set_active_playlist has no effect
    int creates = 0, removes = 0, reorders = 0;

    std::size_t playlist_count() override { return lists.size(); }
    std::size_t active_playlist() override { return active; }
    void set_active_playlist(std::size_t i) override {
        if (!ignore_activation) active = i;
    }
    std::string playlist_name(std::size_t i) override { return i < lists.size() ? lists[i].name : std::string(); }
    std::size_t create_playlist(const std::string& name) override {
        ++creates;
        if (fail_create) return npos;
        lists.push_back({name, {}, {}, false});
        if (create_changes_active) active = lists.size() - 1;
        return lists.size() - 1;
    }
    bool remove_playlist(std::size_t i) override {
        ++removes;
        if (fail_remove || i >= lists.size()) return false;
        lists.erase(lists.begin() + static_cast<std::ptrdiff_t>(i));
        if (active == i) active = npos;
        else if (active != npos && active > i) --active;
        return true;
    }
    void get_items(std::size_t i, std::vector<handle>& out) override {
        if (throw_in_get_items && i == lists.size() - 1) throw std::runtime_error("simulated read failure");
        out = lists.at(i).items;
    }
    bool append_items(std::size_t i, const std::vector<handle>& items) override {
        if (foreign_insert_during_append) {
            lists.insert(lists.begin(), pl{"Foreign", {"f"}, {}, false});
            if (active != npos) ++active;
            ++i;
            return false;  // and the insert into ours fails
        }
        if (throw_in_append) {
            lists.at(i).items.push_back(items.front());  // partially populated, then throws
            throw std::runtime_error("simulated out of memory");
        }
        if (fail_append) return false;
        auto& v = lists.at(i).items;
        if (partial_append) {
            v.insert(v.end(), items.begin(), items.begin() + static_cast<std::ptrdiff_t>(items.size() / 2));
            return true;
        }
        v.insert(v.end(), items.begin(), items.end());
        return true;
    }
    bool reorder_blocked(std::size_t i) override { return lists.at(i).reorder_locked; }
    void undo_backup(std::size_t i) override { lists.at(i).undo.push_back(lists.at(i).items); }
    bool reorder(std::size_t i, const std::vector<std::size_t>& order) override {
        ++reorders;
        if (fail_reorder) return false;
        auto& v = lists.at(i).items;
        if (order.size() != v.size()) return false;
        std::vector<handle> n;
        for (auto k : order) n.push_back(v.at(k));
        v.swap(n);
        return true;
    }
    bool undo(std::size_t i) {
        auto& p = lists.at(i);
        if (p.undo.empty()) return false;
        p.items = p.undo.back();
        p.undo.pop_back();
        return true;
    }
};

struct rng_fixture {
    mock_provider m;
    rng_fixture(bool seed, bool rand) {
        m.feats = {seed, rand};
        m.rdseed = always_ok(101);
        m.rdrand = always_ok(202);
    }
};

std::vector<handle> make_items(std::size_t n) {
    std::vector<handle> v;
    for (std::size_t k = 0; k < n; ++k) v.push_back("track" + std::to_string(k));
    return v;
}

std::vector<handle> sorted(std::vector<handle> v) {
    std::sort(v.begin(), v.end());
    return v;
}

fake_pm two_playlists() {
    fake_pm pm;
    pm.lists.push_back({"Default", make_items(5), {}, false});
    pm.lists.push_back({"Other", {"a", "b", "b", "c"}, {}, false});
    pm.active = 0;
    return pm;
}

}  // namespace

// --------------------------------------------------- new playlist (5.1/5.3)

TEST(new_playlist_success_counts_identity_duplicates) {
    rng_fixture f(true, true);
    hardware_rng rng(f.m);
    auto pm = two_playlists();
    std::vector<handle> items = make_items(200);
    items.push_back("track7");  // duplicate entries must survive
    items.push_back("track7");
    const auto r = create_shuffled_playlist(pm, rng, items, "True Random");
    CHECK(r.ok);
    CHECK_EQ(r.track_count, items.size());
    CHECK_EQ(pm.lists.size(), std::size_t(3));
    CHECK_EQ(r.playlist_index, std::size_t(2));
    CHECK_EQ(pm.active, std::size_t(2));
    const auto& out = pm.lists[2].items;
    CHECK_EQ(out.size(), items.size());
    CHECK(sorted(out) == sorted(items));
    CHECK_EQ(std::count(out.begin(), out.end(), handle("track7")), std::ptrdiff_t(3));
    CHECK(out != items);  // 202! orders; identity would indicate the shuffle was skipped
    CHECK(r.source == rng_source::rdseed);
    CHECK(r.message.find("RDSEED") != std::string::npos);
    // Source playlists untouched.
    CHECK(pm.lists[0].items == make_items(5));
    CHECK((pm.lists[1].items == std::vector<handle>{"a", "b", "b", "c"}));
}

TEST(new_playlist_rng_unavailable_creates_nothing) {
    rng_fixture f(false, false);  // Sandy Bridge
    hardware_rng rng(f.m);
    auto pm = two_playlists();
    const auto r = create_shuffled_playlist(pm, rng, make_items(10), "True Random");
    CHECK(!r.ok);
    CHECK_EQ(pm.creates, 0);
    CHECK_EQ(pm.lists.size(), std::size_t(2));
    CHECK_EQ(pm.active, std::size_t(0));
    CHECK(r.message.find("hardware RNG unavailable") != std::string::npos);
    CHECK(r.message.find("No playlist was created") != std::string::npos);
    CHECK_EQ(f.m.rdseed_calls + f.m.rdrand_calls, std::uint64_t(0));
}

TEST(new_playlist_rng_fails_midway_creates_nothing) {
    mock_provider m;
    m.feats = {false, true};
    rng_policy pol;
    m.rdrand = ok_then_fail(pol.self_test_words + 3, 5);  // far fewer words than needed
    hardware_rng rng(m, pol);
    auto pm = two_playlists();
    const auto r = create_shuffled_playlist(pm, rng, make_items(100), "True Random");
    CHECK(!r.ok);
    CHECK_EQ(pm.creates, 0);
    CHECK_EQ(pm.lists.size(), std::size_t(2));
}

TEST(new_playlist_empty_input) {
    rng_fixture f(true, true);
    hardware_rng rng(f.m);
    auto pm = two_playlists();
    const auto r = create_shuffled_playlist(pm, rng, {}, "True Random");
    CHECK(!r.ok);
    CHECK_EQ(pm.creates, 0);
}

TEST(new_playlist_create_failure_reports_and_leaves_state) {
    rng_fixture f(true, true);
    hardware_rng rng(f.m);
    auto pm = two_playlists();
    pm.fail_create = true;
    const auto r = create_shuffled_playlist(pm, rng, make_items(10), "True Random");
    CHECK(!r.ok);
    CHECK_EQ(pm.lists.size(), std::size_t(2));
    CHECK_EQ(pm.active, std::size_t(0));
}

TEST(new_playlist_insert_failure_rolls_back) {
    rng_fixture f(true, true);
    hardware_rng rng(f.m);
    auto pm = two_playlists();
    pm.active = 1;
    pm.fail_append = true;
    pm.create_changes_active = true;
    const auto r = create_shuffled_playlist(pm, rng, make_items(10), "True Random");
    CHECK(!r.ok);
    CHECK_EQ(pm.removes, 1);
    CHECK_EQ(pm.lists.size(), std::size_t(2));  // no partial playlist left
    CHECK_EQ(pm.active, std::size_t(1));        // active playlist restored
    CHECK(r.message.find("removed") != std::string::npos);
}

TEST(new_playlist_partial_insert_detected_and_rolled_back) {
    rng_fixture f(true, true);
    hardware_rng rng(f.m);
    auto pm = two_playlists();
    pm.partial_append = true;
    const auto r = create_shuffled_playlist(pm, rng, make_items(10), "True Random");
    CHECK(!r.ok);
    CHECK_EQ(pm.lists.size(), std::size_t(2));
    CHECK_EQ(pm.active, std::size_t(0));
}

TEST(new_playlist_rollback_remove_failure_is_reported) {
    rng_fixture f(true, true);
    hardware_rng rng(f.m);
    auto pm = two_playlists();
    pm.fail_append = true;
    pm.fail_remove = true;
    const auto r = create_shuffled_playlist(pm, rng, make_items(10), "True Random");
    CHECK(!r.ok);
    CHECK(r.message.find("could not be removed") != std::string::npos);
    CHECK_EQ(pm.active, std::size_t(0));
}

TEST(new_playlist_with_no_active_playlist) {
    rng_fixture f(false, true);
    hardware_rng rng(f.m);
    fake_pm pm;
    const auto r = create_shuffled_playlist(pm, rng, make_items(3), "True Random");
    CHECK(r.ok);
    CHECK(r.source == rng_source::rdrand);
    CHECK_EQ(pm.lists.size(), std::size_t(1));
}

// ----------------------------------------------------------- in place (5.2)

TEST(in_place_success_preserves_multiplicity_and_undo_restores) {
    rng_fixture f(true, true);
    hardware_rng rng(f.m);
    fake_pm pm;
    std::vector<handle> original = make_items(60);
    original.push_back("track3");
    original.push_back("track3");
    pm.lists.push_back({"Current", original, {}, false});
    pm.active = 0;
    const auto r = shuffle_playlist_in_place(pm, rng, 0);
    CHECK(r.ok);
    CHECK_EQ(r.track_count, original.size());
    CHECK_EQ(pm.lists.size(), std::size_t(1));      // no new playlist
    CHECK_EQ(pm.lists[0].undo.size(), std::size_t(1));  // undo point created
    const auto shuffled = pm.lists[0].items;
    CHECK(sorted(shuffled) == sorted(original));
    CHECK(shuffled != original);
    CHECK(pm.undo(0));
    CHECK(pm.lists[0].items == original);
}

TEST(in_place_rng_unavailable_does_not_touch_playlist) {
    rng_fixture f(false, false);
    hardware_rng rng(f.m);
    fake_pm pm;
    pm.lists.push_back({"Current", make_items(20), {}, false});
    const auto r = shuffle_playlist_in_place(pm, rng, 0);
    CHECK(!r.ok);
    CHECK(pm.lists[0].items == make_items(20));
    CHECK_EQ(pm.lists[0].undo.size(), std::size_t(0));
    CHECK_EQ(pm.reorders, 0);
    CHECK(r.message.find("not modified") != std::string::npos);
}

TEST(in_place_rng_fails_midway_does_not_touch_playlist) {
    mock_provider m;
    m.feats = {true, false};
    rng_policy pol;
    pol.rdseed_attempts = 8;
    m.rdseed = ok_then_fail(pol.self_test_words + 2, 3);
    hardware_rng rng(m, pol);
    fake_pm pm;
    pm.lists.push_back({"Current", make_items(50), {}, false});
    const auto r = shuffle_playlist_in_place(pm, rng, 0);
    CHECK(!r.ok);
    CHECK(pm.lists[0].items == make_items(50));
    CHECK_EQ(pm.lists[0].undo.size(), std::size_t(0));
    CHECK_EQ(pm.reorders, 0);
}

TEST(in_place_locked_playlist_refused) {
    rng_fixture f(true, true);
    hardware_rng rng(f.m);
    fake_pm pm;
    pm.lists.push_back({"Locked", make_items(5), {}, true});
    const auto r = shuffle_playlist_in_place(pm, rng, 0);
    CHECK(!r.ok);
    CHECK_EQ(pm.reorders, 0);
    CHECK(pm.lists[0].items == make_items(5));
}

TEST(in_place_reorder_refused_reports) {
    rng_fixture f(true, true);
    hardware_rng rng(f.m);
    fake_pm pm;
    pm.lists.push_back({"Current", make_items(5), {}, false});
    pm.fail_reorder = true;
    const auto r = shuffle_playlist_in_place(pm, rng, 0);
    CHECK(!r.ok);
    CHECK(pm.lists[0].items == make_items(5));
}

TEST(in_place_no_active_playlist_and_trivial_sizes) {
    rng_fixture f(true, true);
    hardware_rng rng(f.m);
    fake_pm pm;
    CHECK(!shuffle_playlist_in_place(pm, rng, npos).ok);
    pm.lists.push_back({"Empty", {}, {}, false});
    CHECK(!shuffle_playlist_in_place(pm, rng, 0).ok);
    pm.lists.push_back({"One", {"x"}, {}, false});
    const auto r = shuffle_playlist_in_place(pm, rng, 1);
    CHECK(r.ok);
    CHECK_EQ(pm.reorders, 0);
}

TEST(new_playlist_single_track_message_is_clear) {
    rng_fixture f(true, true);
    hardware_rng rng(f.m);
    auto pm = two_playlists();
    const auto r = create_shuffled_playlist(pm, rng, {"only"}, "True Random");
    CHECK(r.ok);
    CHECK((pm.lists.back().items == std::vector<handle>{"only"}));
    CHECK(r.message.find("Copied 1 track") != std::string::npos);
    CHECK(r.message.find("no hardware random values drawn") == std::string::npos);
}

TEST(new_playlist_message_reports_source_for_two_or_more) {
    rng_fixture f(true, true);
    hardware_rng rng(f.m);
    auto pm = two_playlists();
    const auto r = create_shuffled_playlist(pm, rng, make_items(2), "True Random");
    CHECK(r.ok);
    CHECK(r.message.find("Randomized 2 tracks") != std::string::npos);
    CHECK(r.message.find("RDSEED (1 x 64-bit values)") != std::string::npos);
}

// ------------------------------------------------- audit hardening (1.0.2)

TEST(new_playlist_exception_after_create_rolls_back) {
    rng_fixture f(true, true);
    hardware_rng rng(f.m);
    auto pm = two_playlists();
    pm.active = 1;
    pm.throw_in_append = true;
    bool threw = false;
    try {
        (void)create_shuffled_playlist(pm, rng, make_items(10), "True Random");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);                                // the error still reaches the command layer
    CHECK_EQ(pm.lists.size(), std::size_t(2));   // partially populated playlist removed
    CHECK_EQ(pm.active, std::size_t(1));
    CHECK(pm.lists[0].items == make_items(5));
}

TEST(new_playlist_exception_in_verification_rolls_back) {
    rng_fixture f(true, true);
    hardware_rng rng(f.m);
    auto pm = two_playlists();
    pm.throw_in_get_items = true;
    bool threw = false;
    try {
        (void)create_shuffled_playlist(pm, rng, make_items(10), "True Random");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK_EQ(pm.lists.size(), std::size_t(2));
    CHECK_EQ(pm.active, std::size_t(0));
}

TEST(rollback_never_removes_a_foreign_playlist) {
    rng_fixture f(true, true);
    hardware_rng rng(f.m);
    auto pm = two_playlists();
    pm.foreign_insert_during_append = true;
    const auto r = create_shuffled_playlist(pm, rng, make_items(10), "True Random");
    CHECK(!r.ok);
    // Indices shifted under us: nothing may be removed blindly.
    CHECK_EQ(pm.removes, 0);
    CHECK_EQ(pm.lists.size(), std::size_t(4));
    CHECK(pm.lists[0].name == "Foreign");
    CHECK(pm.lists[1].items == make_items(5));
    CHECK(r.message.find("could not be removed safely") != std::string::npos);
}

TEST(new_playlist_name_collision_gets_number) {
    rng_fixture f(true, true);
    hardware_rng rng(f.m);
    auto pm = two_playlists();
    auto r1 = create_shuffled_playlist(pm, rng, make_items(3), "True Random - Library");
    auto r2 = create_shuffled_playlist(pm, rng, make_items(3), "True Random - Library");
    pm.lists.push_back({"true random - library (3)", {}, {}, false});  // case-insensitive clash
    auto r3 = create_shuffled_playlist(pm, rng, make_items(3), "True Random - Library");
    CHECK(r1.ok && r2.ok && r3.ok);
    CHECK(pm.lists[2].name == "True Random - Library");
    CHECK(pm.lists[3].name == "True Random - Library (2)");
    CHECK(pm.lists[5].name == "True Random - Library (4)");
    CHECK(r2.message.find("\"True Random - Library (2)\"") != std::string::npos);
    // Existing playlists were never reused or modified.
    CHECK_EQ(pm.lists[2].items.size(), std::size_t(3));
}

TEST(names_equal_is_ascii_case_insensitive) {
    CHECK(names_equal("Abc", "aBC"));
    CHECK(!names_equal("Abc", "Abd"));
    CHECK(!names_equal("Abc", "Abcd"));
}

TEST(activation_failure_is_reported_not_hidden) {
    rng_fixture f(true, true);
    hardware_rng rng(f.m);
    auto pm = two_playlists();
    pm.ignore_activation = true;
    const auto r = create_shuffled_playlist(pm, rng, make_items(4), "True Random");
    CHECK(r.ok);  // the playlist is complete and correct
    CHECK_EQ(pm.lists.size(), std::size_t(3));
    CHECK(r.message.find("could not be activated") != std::string::npos);
}

TEST(single_track_message_is_source_neutral) {
    rng_fixture f(true, true);
    hardware_rng rng(f.m);
    auto pm = two_playlists();
    const auto r = create_shuffled_playlist(pm, rng, {"only"}, "True Random - Library");
    CHECK(r.ok);
    CHECK(r.message.find("selected") == std::string::npos);  // was wrong for the library command
    CHECK(r.message.find("Copied 1 track") != std::string::npos);
}

TEST(large_input_new_playlist_and_in_place) {
    rng_fixture f(true, true);
    hardware_rng rng(f.m);
    fake_pm pm;
    pm.lists.push_back({"Big", make_items(50000), {}, false});
    pm.active = 0;
    const auto r1 = shuffle_playlist_in_place(pm, rng, 0);
    CHECK(r1.ok);
    CHECK(sorted(pm.lists[0].items) == sorted(make_items(50000)));
    const auto r2 = create_shuffled_playlist(pm, rng, make_items(50000), "True Random - Library");
    CHECK(r2.ok);
    CHECK(sorted(pm.lists[1].items) == sorted(make_items(50000)));
    CHECK(pm.lists[1].items != make_items(50000));
}

TEST_MAIN("playlist_logic")
