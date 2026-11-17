// foo_true_random — command entry points.
// All functions must be called on foobar2000's main thread.
// SPDX-License-Identifier: MIT
#pragma once

#include "stdafx.h"

#include <vector>

#include "fb2k_backend.h"
#include "ftr/playlist_logic.h"

namespace ftr_fb2k {

void cmd_entire_library();
void cmd_current_playlist();
void cmd_status();
void cmd_selected_tracks(metadb_handle_list_cref data);
ftr::operation_result run_new_playlist(const std::vector<handle>& items, const char* kind, const char* what);

}  // namespace ftr_fb2k
