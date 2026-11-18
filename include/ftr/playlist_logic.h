// foo-true-random — SDK-independent playlist transaction logic.
//
// The foobar2000 component implements `playlist_backend` on top of the real
// playlist_manager; the tests implement it with an in-memory fake that can
// inject failures. All decisions about ordering, validation, rollback, and
// reporting live here so they can be tested without foobar2000.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ftr/hardware_rng.h"
#include "ftr/random_core.h"

namespace ftr {

inline constexpr std::size_t npos = static_cast<std::size_t>(-1);

template <class Handle>
class playlist_backend {
public:
    virtual ~playlist_backend() = default;
    virtual std::size_t playlist_count() = 0;
    virtual std::size_t active_playlist() = 0;                 // npos if none
    virtual void set_active_playlist(std::size_t index) = 0;   // npos for none
    // Name of a playlist (empty on failure).
    virtual std::string playlist_name(std::size_t index) = 0;
    // Create a playlist at the end; returns its index or npos on failure.
    virtual std::size_t create_playlist(const std::string& name) = 0;
    // Remove exactly one playlist; returns success.
    virtual bool remove_playlist(std::size_t index) = 0;
    virtual void get_items(std::size_t playlist, std::vector<Handle>& out) = 0;
    // Append items; returns false on failure (e.g. playlist lock).
    virtual bool append_items(std::size_t playlist, const std::vector<Handle>& items) = 0;
    // Whether reordering the playlist is blocked by a playlist lock.
    virtual bool reorder_blocked(std::size_t playlist) = 0;
    virtual void undo_backup(std::size_t playlist) = 0;
    // Reorder so that new position k holds old item order[k].
    virtual bool reorder(std::size_t playlist, const std::vector<std::size_t>& order) = 0;
};

struct operation_result {
    bool ok = false;
    std::size_t track_count = 0;
    std::size_t playlist_index = npos;
    rng_source source = rng_source::none;  // source that supplied the last value
    std::string rng_usage;                 // e.g. "RDSEED (41 x 64-bit values)"
    std::string message;                   // user-facing summary or failure reason
};

// Compute a full shuffled copy of `items` with the hardware RNG. Nothing
// outside `out` is touched. On failure `out` is left empty and `why` explains.
template <class Handle>
bool shuffle_items(hardware_rng& rng, const std::vector<Handle>& items, std::vector<Handle>& out,
                   std::string& why) {
    out.clear();
    if (!rng.usable()) {
        why = rng.failure_reason();
        return false;
    }
    std::vector<std::size_t> order;
    const core_status st = make_permutation<std::uint32_t>(rng, items.size(), order);
    if (st != core_status::ok) {
        why = std::string("Randomization aborted: ") + to_string(st) + ".";
        if (!rng.usable()) why += " " + rng.failure_reason();
        return false;
    }
    if (!apply_permutation(items, order, out)) {
        why = "Internal error: invalid permutation.";
        out.clear();
        return false;
    }
    return true;
}

// ASCII case-insensitive equality (foobar2000 playlist names are compared
// case-insensitively by its own find_playlist helper).
inline bool names_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t k = 0; k < a.size(); ++k) {
        char x = a[k], y = b[k];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// Returns `base` if no playlist has that name, otherwise "base (2)", "base (3)", ...
// Existing playlists are never reused or overwritten.
template <class Handle>
std::string unique_playlist_name(playlist_backend<Handle>& pm, const std::string& base) {
    std::vector<std::string> names;
    const std::size_t count = pm.playlist_count();
    names.reserve(count);
    for (std::size_t k = 0; k < count; ++k) names.push_back(pm.playlist_name(k));
    auto taken = [&](const std::string& n) {
        for (const auto& e : names)
            if (names_equal(e, n)) return true;
        return false;
    };
    if (!taken(base)) return base;
    // count + 2 candidates cannot all be taken by count playlists.
    for (std::size_t k = 2; k <= count + 2; ++k) {
        std::string n = base + " (" + std::to_string(k) + ")";
        if (!taken(n)) return n;
    }
    return base;  // unreachable
}

// Owns a playlist created by create_shuffled_playlist until commit().
// rollback() runs explicitly on ordinary failures and from the destructor if an
// exception escapes, so no partially populated playlist is left behind.
// It only removes the playlist after confirming it is still the one we created
// (same playlist count, same index, same name): if another component changed
// the playlist list in between, it removes nothing rather than risk deleting a
// user's playlist.
template <class Handle>
class created_playlist_guard {
public:
    created_playlist_guard(playlist_backend<Handle>& pm, std::size_t prev_active)
        : m_pm(pm), m_prev_active(prev_active) {}
    created_playlist_guard(const created_playlist_guard&) = delete;
    created_playlist_guard& operator=(const created_playlist_guard&) = delete;
    ~created_playlist_guard() {
        if (m_armed) {
            try {
                rollback();
            } catch (...) {
                // Destructor must not throw; the caller reports the original failure.
            }
        }
    }
    // Call immediately after the playlist was created. Armed first, so that if
    // a query below throws, the destructor still attempts (safe) cleanup.
    void arm(std::size_t index, const std::string& requested_name) {
        m_armed = true;
        m_index = index;
        m_name = requested_name;
        m_count_after_create = m_pm.playlist_count();
        m_name = m_pm.playlist_name(index);  // the name as actually stored by the host
    }
    void commit() noexcept { m_armed = false; }
    // Returns true if the created playlist was removed (or none was created).
    bool rollback() {
        if (!m_armed) return true;
        m_armed = false;
        bool removed = false;
        if (m_pm.playlist_count() == m_count_after_create && m_index < m_count_after_create &&
            m_pm.playlist_name(m_index) == m_name) {
            removed = m_pm.remove_playlist(m_index);
        }
        // We created at the end, so prev_active (< index) is unaffected by the removal.
        if (m_pm.active_playlist() != m_prev_active) m_pm.set_active_playlist(m_prev_active);
        return removed;
    }

private:
    playlist_backend<Handle>& m_pm;
    std::size_t m_prev_active;
    std::size_t m_index = npos;
    std::size_t m_count_after_create = 0;
    std::string m_name;
    bool m_armed = false;
};

// Operations 5.1 (library) and 5.3 (selection): shuffle `items` into a new
// playlist named `name` (or "name (2)" etc. if that name is taken).
//
// Failure safety: the complete shuffled sequence is computed first; if the
// hardware RNG fails, foobar2000 is never touched. After creating the playlist,
// insertion is verified element by element. If insertion or verification fails,
// or an exception escapes, the new playlist is removed (see
// created_playlist_guard) and the previously active playlist is restored.
// This is compensating rollback, not an atomic transaction: other components
// can observe the new playlist briefly before it is removed.
template <class Handle>
operation_result create_shuffled_playlist(playlist_backend<Handle>& pm, hardware_rng& rng,
                                          const std::vector<Handle>& items,
                                          const std::string& name) {
    operation_result r;
    r.track_count = items.size();
    if (items.empty()) {
        r.message = "No tracks to randomize.";
        return r;
    }
    std::vector<Handle> shuffled;
    std::string why;
    if (!shuffle_items(rng, items, shuffled, why)) {
        r.message = why + " No playlist was created.";
        return r;
    }
    r.source = rng.last_source();
    r.rng_usage = rng.usage_summary();

    const std::size_t prev_active = pm.active_playlist();
    const std::string final_name = unique_playlist_name(pm, name);
    created_playlist_guard<Handle> guard(pm, prev_active);
    const std::size_t idx = pm.create_playlist(final_name);
    if (idx == npos) {
        if (pm.active_playlist() != prev_active) pm.set_active_playlist(prev_active);
        r.message = "Could not create a new playlist. Nothing was changed.";
        return r;
    }
    guard.arm(idx, final_name);
    bool inserted = pm.append_items(idx, shuffled);
    if (inserted) {
        std::vector<Handle> check;
        pm.get_items(idx, check);
        inserted = (check == shuffled);
    }
    if (!inserted) {
        const bool removed = guard.rollback();
        r.message = removed ? "Inserting tracks failed; the incomplete playlist was removed. Nothing else was changed."
                            : "Inserting tracks failed and the incomplete playlist \"" + final_name +
                                  "\" could not be removed safely. Please delete it manually.";
        return r;
    }
    guard.commit();
    r.ok = true;
    r.playlist_index = idx;
    if (items.size() == 1) {
        // A single track has only one possible order: no random values are needed.
        r.message = "Copied 1 track into new playlist \"" + final_name +
                    "\" (a single track has only one possible order, so no random values were needed).";
    } else {
        r.message = "Randomized " + std::to_string(items.size()) + " tracks into new playlist \"" + final_name +
                    "\" using " + r.rng_usage + ".";
    }
    pm.set_active_playlist(idx);
    if (pm.active_playlist() != idx) r.message += " (The new playlist could not be activated; select it manually.)";
    return r;
}

// Operation 5.2: reorder an existing playlist in place.
//
// The permutation is computed before anything is touched. An undo restore
// point is created immediately before the reorder, so the native Edit > Undo
// reverts it. If the RNG fails the playlist is never modified.
template <class Handle>
operation_result shuffle_playlist_in_place(playlist_backend<Handle>& pm, hardware_rng& rng,
                                           std::size_t playlist) {
    operation_result r;
    if (playlist == npos || playlist >= pm.playlist_count()) {
        r.message = "There is no active playlist.";
        return r;
    }
    r.playlist_index = playlist;
    std::vector<Handle> before;
    pm.get_items(playlist, before);
    r.track_count = before.size();
    if (before.size() < 2) {
        r.message = before.empty() ? "The current playlist is empty."
                                   : "The current playlist has only one track; nothing to reorder.";
        r.ok = !before.empty();
        return r;
    }
    if (pm.reorder_blocked(playlist)) {
        r.message = "The current playlist is locked against reordering. Nothing was changed.";
        return r;
    }
    if (!rng.usable()) {
        r.message = rng.failure_reason() + " The playlist was not modified.";
        return r;
    }
    std::vector<std::size_t> order;
    const core_status st = make_permutation<std::uint32_t>(rng, before.size(), order);
    if (st != core_status::ok) {
        r.message = std::string("Randomization aborted: ") + to_string(st) + ". " +
                    rng.failure_reason() + " The playlist was not modified.";
        return r;
    }
    std::vector<Handle> expected;
    if (!apply_permutation(before, order, expected)) {
        r.message = "Internal error: invalid permutation. The playlist was not modified.";
        return r;
    }
    r.source = rng.last_source();
    r.rng_usage = rng.usage_summary();

    pm.undo_backup(playlist);
    if (!pm.reorder(playlist, order)) {
        r.message = "foobar2000 refused the reorder. The playlist was not modified.";
        return r;
    }
    std::vector<Handle> after;
    pm.get_items(playlist, after);
    if (after != expected) {
        r.message = "Playlist contents after reordering do not match the computed order. "
                    "Use Edit > Undo to restore the previous order.";
        return r;
    }
    r.ok = true;
    r.message = "Randomized " + std::to_string(before.size()) + " tracks in the current playlist using " +
                r.rng_usage + ". Edit > Undo restores the previous order.";
    return r;
}

}  // namespace ftr
