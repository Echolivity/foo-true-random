// foo_true_random_itest — separate integration-test component (never shipped).
//
// It is installed next to the SHIPPED foo_true_random.dll (taken from the
// .fb2k-component package). When foobar2000 starts with FTR_ITEST_REPORT set,
// it locates the shipped component's commands through foobar2000's own menu
// services (by GUID), executes them against the real playlist_manager and
// library_manager, verifies the results, writes a report and exits foobar2000.
// The rollback and no-hardware checks drive the same portable transaction code
// (compiled into this DLL) against the real playlist_manager.
// SPDX-License-Identifier: MIT
#define _CRT_SECURE_NO_WARNINGS
#include "stdafx.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "fb2k_backend.h"
#include "guids.h"
#include "ftr/hardware_rng.h"
#include "ftr/playlist_logic.h"

namespace ftr_fb2k {
namespace {

// Writes each line immediately (flushed), so a hang still leaves evidence.
struct reporter {
    std::ofstream out;
    int failures = 0;
    int checks = 0;
    explicit reporter(const char* path) : out(path, std::ios::binary | std::ios::app) {}
    void line(const std::string& s) { out << s << "\n" << std::flush; }
    void info(const std::string& s) { line("  " + s); }
    void check(bool ok, const std::string& what) {
        ++checks;
        if (!ok) ++failures;
        line(std::string(ok ? "[PASS] " : "[FAIL] ") + what);
    }
};

std::vector<const void*> ptrs(const std::vector<handle>& v) {
    std::vector<const void*> p;
    for (auto& h : v) p.push_back(h.get_ptr());
    return p;
}
bool same_multiset(const std::vector<handle>& a, const std::vector<handle>& b) {
    auto x = ptrs(a), y = ptrs(b);
    std::sort(x.begin(), x.end());
    std::sort(y.begin(), y.end());
    return x == y;
}
bool same_order(const std::vector<handle>& a, const std::vector<handle>& b) { return ptrs(a) == ptrs(b); }

std::size_t add_playlist(const char* name, const std::vector<handle>& items) {
    auto pm = playlist_manager::get();
    const t_size idx = pm->create_playlist(name, SIZE_MAX, SIZE_MAX);
    metadb_handle_list l;
    for (auto& h : items) l.add_item(h);
    pm->playlist_insert_items(idx, 0, l, pfc::bit_array_false());
    return idx;
}

std::vector<handle> items_of(std::size_t idx) {
    playlist_manager_backend b;
    std::vector<handle> v;
    b.get_items(idx, v);
    return v;
}

// Returns the registered name of a main-menu command, or "" if not registered.
std::string mainmenu_command_name(const GUID& g) {
    service_enum_t<mainmenu_commands> e;
    service_ptr_t<mainmenu_commands> p;
    while (e.next(p)) {
        const t_uint32 n = p->get_command_count();
        for (t_uint32 i = 0; i < n; ++i)
            if (p->get_command(i) == g) {
                pfc::string8 name;
                p->get_name(i, name);
                return name.c_str();
            }
    }
    return {};
}

// Finds the registered context-menu item; returns false if not registered.
bool find_context_item(const GUID& g, service_ptr_t<contextmenu_item>& out, unsigned& index, std::string& name) {
    service_enum_t<contextmenu_item> e;
    service_ptr_t<contextmenu_item> p;
    while (e.next(p)) {
        const unsigned n = p->get_num_items();
        for (unsigned i = 0; i < n; ++i)
            if (p->get_item_guid(i) == g) {
                pfc::string8 s;
                p->get_item_name(i, s);
                out = p;
                index = i;
                name = s.c_str();
                return true;
            }
    }
    return false;
}

// Backend wrapper used to inject failures on top of the REAL playlist_manager.
class failing_backend : public ftr::playlist_backend<handle> {
public:
    enum class mode { fail_append, partial_append, throw_after_partial };
    explicit failing_backend(mode m) : m_mode(m) {}
    std::size_t playlist_count() override { return m_real.playlist_count(); }
    std::size_t active_playlist() override { return m_real.active_playlist(); }
    void set_active_playlist(std::size_t i) override { m_real.set_active_playlist(i); }
    std::string playlist_name(std::size_t i) override { return m_real.playlist_name(i); }
    std::size_t create_playlist(const std::string& n) override { return m_real.create_playlist(n); }
    bool remove_playlist(std::size_t i) override { return m_real.remove_playlist(i); }
    void get_items(std::size_t p, std::vector<handle>& o) override { m_real.get_items(p, o); }
    bool append_items(std::size_t p, const std::vector<handle>& items) override {
        if (m_mode == mode::fail_append) return false;
        if (m_mode == mode::throw_after_partial) {
            std::vector<handle> one(items.begin(), items.begin() + 1);
            m_real.append_items(p, one);  // really inserts into the real playlist...
            throw std::runtime_error("simulated failure after partial insert");  // ...then fails
        }
        std::vector<handle> half(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(items.size() / 2));
        return m_real.append_items(p, half);  // really inserts half, reports success
    }
    bool reorder_blocked(std::size_t p) override { return m_real.reorder_blocked(p); }
    void undo_backup(std::size_t p) override { m_real.undo_backup(p); }
    bool reorder(std::size_t p, const std::vector<std::size_t>& o) override { return m_real.reorder(p, o); }

private:
    mode m_mode;
    playlist_manager_backend m_real;
};

class no_hardware_provider : public ftr::instruction_provider {
public:
    int executed = 0;
    ftr::cpu_features features() noexcept override { return {}; }
    bool rdseed64_once(std::uint64_t&) noexcept override { ++executed; return false; }
    bool rdrand64_once(std::uint64_t&) noexcept override { ++executed; return false; }
};

void run_tests(reporter& r, const char* media_dir) {
    auto pm = playlist_manager::get();

    // ---- environment / hardware
    const auto snap = ftr::read_cpuid_snapshot();
    const auto feats = ftr::decode_cpu_features(snap);
    r.info("CPU features (CPUID): " + ftr::describe_features(feats));
    {
        ftr::hardware_rng rng(ftr::cpu_instruction_provider());
        std::uint64_t w = 0;
        const bool ok = rng.usable() && rng.next_u64(w);
        r.info(std::string("Hardware RNG source in foobar2000 process: ") +
               (ok ? ftr::to_string(rng.last_source()) : rng.failure_reason().c_str()));
        r.check(ok == (feats.rdseed || feats.rdrand), "hardware RNG usable iff CPUID reports RDSEED or RDRAND");
    }

    // ---- registration
    r.check(mainmenu_command_name(guid_cmd_library) == "True Random: Entire Media Library",
            "main menu registers 'True Random: Entire Media Library'");
    r.check(mainmenu_command_name(guid_cmd_current) == "True Random: Current Playlist",
            "main menu registers 'True Random: Current Playlist'");
    r.check(mainmenu_command_name(guid_cmd_status) == "True Random: Hardware RNG Status",
            "main menu registers 'True Random: Hardware RNG Status'");
    service_ptr_t<contextmenu_item> ctx;
    unsigned ctx_index = 0;
    std::string ctx_name;
    const bool ctx_found = find_context_item(guid_ctx_selected, ctx, ctx_index, ctx_name);
    r.check(ctx_found && ctx_name == "True Random: Selected Tracks",
            "context menu registers 'True Random: Selected Tracks'");

    // ---- test tracks: 40 distinct handles + 2 extra copies of one (duplicates)
    std::vector<handle> tracks;
    auto mdb = metadb::get();
    for (int k = 0; k < 40; ++k) {
        char path[1024];
        std::snprintf(path, sizeof path, "file://%s\\t%02d.wav", media_dir, k);
        tracks.push_back(mdb->handle_create(path, 0));
    }
    tracks.push_back(tracks[5]);
    tracks.push_back(tracks[5]);
    r.info("Test input: 42 entries (40 distinct tracks, one track present 3 times)");

    // ---- reorder semantics of playlist_reorder_items: new[k] == old[order[k]]
    {
        const std::size_t idx = add_playlist("itest reorder semantics", tracks);
        std::vector<std::size_t> order(tracks.size());
        for (std::size_t k = 0; k < order.size(); ++k) order[k] = (k + 7) % order.size();
        playlist_manager_backend b;
        const bool ok = b.reorder(idx, order);
        const auto after = items_of(idx);
        bool sem = ok && after.size() == tracks.size();
        for (std::size_t k = 0; sem && k < after.size(); ++k) sem = after[k] == tracks[order[k]];
        r.check(sem, "playlist_reorder_items semantics: new[k] == old[order[k]]");
        pfc::bit_array_one m(idx);
        pm->remove_playlists(m);
    }

    // ---- 5.3 Selected Tracks through the registered context-menu item
    if (ctx_found) {
        const std::size_t src = add_playlist("itest source", tracks);
        const auto src_before = items_of(src);
        const std::size_t count_before = pm->get_playlist_count();
        metadb_handle_list sel;
        for (auto& h : tracks) sel.add_item(h);
        ctx->item_execute_simple(ctx_index, pfc::guid_null, sel, contextmenu_item::caller_active_playlist_selection);
        const bool hw = feats.rdseed || feats.rdrand;
        if (hw) {
            r.check(pm->get_playlist_count() == count_before + 1, "Selected Tracks: exactly one new playlist");
            const std::size_t np = pm->get_playlist_count() - 1;
            pfc::string8 name;
            pm->playlist_get_name(np, name);
            r.check(std::string(name.c_str()) == "True Random - Selection", "Selected Tracks: playlist name");
            const auto out = items_of(np);
            r.check(out.size() == tracks.size(), "Selected Tracks: track count 42");
            r.check(same_multiset(out, tracks), "Selected Tracks: identical track identities incl. 3x duplicate");
            r.check(!same_order(out, tracks), "Selected Tracks: order differs from input");
            r.check(pm->get_active_playlist() == np, "Selected Tracks: new playlist activated");
            // Running it again must not create a second playlist with an identical name.
            ctx->item_execute_simple(ctx_index, pfc::guid_null, sel, contextmenu_item::caller_active_playlist_selection);
            pfc::string8 name2;
            pm->playlist_get_name(pm->get_playlist_count() - 1, name2);
            r.check(pm->get_playlist_count() == count_before + 2 &&
                        std::string(name2.c_str()) == "True Random - Selection (2)",
                    "Selected Tracks again: second playlist named 'True Random - Selection (2)'");
            pfc::bit_array_range m(np, 2);
            pm->remove_playlists(m);
        } else {
            r.check(pm->get_playlist_count() == count_before, "Selected Tracks without hardware RNG: no playlist created");
        }
        r.check(same_order(items_of(src), src_before), "Selected Tracks: source playlist unchanged");
        pfc::bit_array_one m(src);
        pm->remove_playlists(m);
    }

    // ---- 5.2 Current Playlist through the registered main-menu command + undo
    {
        const std::size_t cur = add_playlist("itest current", tracks);
        pm->set_active_playlist(cur);
        const auto before = items_of(cur);
        const std::size_t count_before = pm->get_playlist_count();
        const bool executed = mainmenu_commands::g_execute(guid_cmd_current);
        r.check(executed, "Current Playlist: executed via mainmenu_commands::g_execute");
        const auto after = items_of(cur);
        r.check(pm->get_playlist_count() == count_before, "Current Playlist: no playlist created or removed");
        r.check(after.size() == before.size(), "Current Playlist: track count unchanged");
        r.check(same_multiset(after, before), "Current Playlist: identical identities incl. duplicates");
        if (feats.rdseed || feats.rdrand) {
            r.check(!same_order(after, before), "Current Playlist: order changed");
            r.check(pm->playlist_is_undo_available(cur), "Current Playlist: undo restore point available");
            const bool undone = pm->playlist_undo_restore(cur);
            r.check(undone && same_order(items_of(cur), before), "Current Playlist: native undo restores exact previous order");
        } else {
            r.check(same_order(after, before), "Current Playlist without hardware RNG: playlist untouched");
        }
        pfc::bit_array_one m(cur);
        pm->remove_playlists(m);
    }

    // ---- 5.1 Entire Media Library through the registered main-menu command
    {
        auto lm = library_manager::get();
        const bool enabled = lm->is_library_enabled();
        metadb_handle_list all;
        lm->get_all_items(all);
        r.info("Media Library enabled: " + std::string(enabled ? "yes" : "no") + ", items: " +
               std::to_string(all.get_count()));
        const std::size_t count_before = pm->get_playlist_count();
        const bool executed = mainmenu_commands::g_execute(guid_cmd_library);
        r.check(executed, "Entire Media Library: executed via mainmenu_commands::g_execute");
        if (enabled && all.get_count() > 0 && (feats.rdseed || feats.rdrand)) {
            r.check(pm->get_playlist_count() == count_before + 1, "Entire Media Library: one new playlist");
            std::vector<handle> lib;
            to_vector(all, lib);
            const auto out = items_of(pm->get_playlist_count() - 1);
            r.check(same_multiset(out, lib), "Entire Media Library: every library track exactly once");
        } else {
            r.check(pm->get_playlist_count() == count_before,
                    "Entire Media Library (library not configured/empty): nothing created, user informed");
        }
    }

    // ---- rollback on the real playlist_manager
    for (auto mode : {failing_backend::mode::fail_append, failing_backend::mode::partial_append,
                      failing_backend::mode::throw_after_partial}) {
        const char* label = mode == failing_backend::mode::fail_append      ? "insert refused"
                            : mode == failing_backend::mode::partial_append ? "partial insert"
                                                                            : "exception after partial insert";
        const std::size_t anchor = add_playlist("itest anchor", {tracks[0]});
        pm->set_active_playlist(anchor);
        const std::size_t count_before = pm->get_playlist_count();
        failing_backend fb(mode);
        ftr::hardware_rng rng(ftr::cpu_instruction_provider());
        bool failed = false;
        try {
            failed = !ftr::create_shuffled_playlist(fb, rng, tracks, "itest rollback").ok;
        } catch (const std::exception&) {
            failed = true;  // the command layer reports this to the user
        }
        r.check(failed, std::string("Rollback (") + label + "): operation reports failure");
        r.check(pm->get_playlist_count() == count_before, std::string("Rollback (") + label + "): no partial playlist left");
        r.check(pm->get_active_playlist() == anchor, std::string("Rollback (") + label + "): active playlist restored");
        pfc::bit_array_one m(anchor);
        pm->remove_playlists(m);
    }

    // ---- fail-closed path with a CPU that has no hardware RNG (Sandy Bridge model)
    {
        const std::size_t count_before = pm->get_playlist_count();
        no_hardware_provider nh;
        ftr::hardware_rng rng(nh);
        playlist_manager_backend b;
        const auto res = ftr::create_shuffled_playlist(b, rng, tracks, "itest no hardware");
        r.check(!res.ok && pm->get_playlist_count() == count_before && nh.executed == 0,
                "No-hardware CPU model: refused, nothing created, no instruction executed");
        r.info("No-hardware message: " + res.message);
    }
}

void run_and_exit() {
    const char* report_path = std::getenv("FTR_ITEST_REPORT");
    const char* media = std::getenv("FTR_ITEST_MEDIA");
    {
        reporter r(report_path);
        r.line("driver running on main thread");
        try {
            run_tests(r, media ? media : "C:\\ftr-itest-media");
        } catch (const std::exception& e) {
            r.check(false, std::string("exception: ") + e.what());
        }
        r.line(std::string("RESULT: ") + (r.failures == 0 ? "PASS" : "FAIL") + " (" + std::to_string(r.checks) +
               " checks, " + std::to_string(r.failures) + " failed)");
    }
    standard_commands::main_exit();
}

class itest_initquit : public initquit {
public:
    void on_init() override {
        const char* path = std::getenv("FTR_ITEST_REPORT");
        if (path == nullptr) return;  // inert unless explicitly requested
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            f << "driver on_init reached\n";  // progress marker for diagnostics
        }
        fb2k::inMainThread([] { run_and_exit(); });
    }
};

initquit_factory_t<itest_initquit> g_itest_factory;

}  // namespace
}  // namespace ftr_fb2k

DECLARE_COMPONENT_VERSION("True Random integration test", "1.0.0",
                          "Test harness for foo_true_random. Inert unless FTR_ITEST_REPORT is set. Not for end users.");
VALIDATE_COMPONENT_FILENAME("foo_true_random_itest.dll");
