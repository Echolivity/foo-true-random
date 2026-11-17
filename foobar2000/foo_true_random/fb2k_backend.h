// foo_true_random — playlist_backend implemented on foobar2000's playlist_manager.
// All methods must be called from the main thread (playlist_manager contract).
// SPDX-License-Identifier: MIT
#pragma once

#include "stdafx.h"

#include <string>
#include <vector>

#include "ftr/playlist_logic.h"

namespace ftr_fb2k {

using handle = metadb_handle_ptr;

void to_vector(metadb_handle_list_cref in, std::vector<handle>& out);

class playlist_manager_backend final : public ftr::playlist_backend<handle> {
public:
    playlist_manager_backend();

    std::size_t playlist_count() override;
    std::size_t active_playlist() override;
    void set_active_playlist(std::size_t index) override;
    std::size_t create_playlist(const std::string& name) override;
    bool remove_playlist(std::size_t index) override;
    void get_items(std::size_t playlist, std::vector<handle>& out) override;
    bool append_items(std::size_t playlist, const std::vector<handle>& items) override;
    bool reorder_blocked(std::size_t playlist) override;
    void undo_backup(std::size_t playlist) override;
    bool reorder(std::size_t playlist, const std::vector<std::size_t>& order) override;

private:
    service_ptr_t<playlist_manager> m_pm;
};

}  // namespace ftr_fb2k
