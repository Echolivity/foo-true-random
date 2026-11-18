// foo_true_random — playlist_backend on top of playlist_manager (SDK 2026-10-01).
// SPDX-License-Identifier: MIT
#include "fb2k_backend.h"

namespace ftr_fb2k {

void to_vector(metadb_handle_list_cref in, std::vector<handle>& out) {
    out.clear();
    const t_size n = in.get_count();
    out.reserve(n);
    for (t_size k = 0; k < n; ++k) out.push_back(in[k]);
}

playlist_manager_backend::playlist_manager_backend() : m_pm(playlist_manager::get()) {}

std::size_t playlist_manager_backend::playlist_count() { return m_pm->get_playlist_count(); }

std::size_t playlist_manager_backend::active_playlist() {
    // SDK: "infinite if no playlist is active" — SIZE_MAX, identical to ftr::npos.
    return m_pm->get_active_playlist();
}

void playlist_manager_backend::set_active_playlist(std::size_t index) {
    m_pm->set_active_playlist(index);  // infinite => no active playlist
}

std::string playlist_manager_backend::playlist_name(std::size_t index) {
    pfc::string8 name;
    if (!m_pm->playlist_get_name(index, name)) return {};
    return name.c_str();
}

std::size_t playlist_manager_backend::create_playlist(const std::string& name) {
    // p_name_length = SIZE_MAX: null-terminated. p_index = SIZE_MAX: append at end.
    // Returns the new index, or infinite on failure.
    return m_pm->create_playlist(name.c_str(), SIZE_MAX, SIZE_MAX);
}

bool playlist_manager_backend::remove_playlist(std::size_t index) {
    const t_size count = m_pm->get_playlist_count();
    if (index >= count) return false;
    // remove_playlists() takes a mask; size it to the current playlist count
    // and select exactly the one playlist to remove.
    pfc::bit_array_bittable mask(count);
    mask.set(index, true);
    return m_pm->remove_playlists(mask);
}

void playlist_manager_backend::get_items(std::size_t playlist, std::vector<handle>& out) {
    metadb_handle_list list;
    m_pm->playlist_get_all_items(playlist, list);
    to_vector(list, out);
}

bool playlist_manager_backend::append_items(std::size_t playlist, const std::vector<handle>& items) {
    metadb_handle_list list;
    for (const auto& h : items) list.add_item(h);
    const t_size base = m_pm->playlist_get_item_count(playlist);
    const t_size r = m_pm->playlist_insert_items(playlist, base, list, pfc::bit_array_false());
    // The caller additionally verifies the playlist contents element by element.
    return r != SIZE_MAX;
}

bool playlist_manager_backend::reorder_blocked(std::size_t playlist) {
    return (m_pm->playlist_lock_get_filter_mask(playlist) & playlist_lock::filter_reorder) != 0;
}

void playlist_manager_backend::undo_backup(std::size_t playlist) { m_pm->playlist_undo_backup(playlist); }

bool playlist_manager_backend::reorder(std::size_t playlist, const std::vector<std::size_t>& order) {
    static_assert(sizeof(t_size) == sizeof(std::size_t));
    return m_pm->playlist_reorder_items(playlist, order.data(), order.size());
}

}  // namespace ftr_fb2k
