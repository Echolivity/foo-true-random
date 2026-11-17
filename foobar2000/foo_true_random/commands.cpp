// foo_true_random — main-menu and context-menu commands.
// SPDX-License-Identifier: MIT
#include "stdafx.h"

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "fb2k_backend.h"
#include "ftr/hardware_rng.h"
#include "ftr/playlist_logic.h"

namespace {

using ftr_fb2k::handle;

// {2CB9B2A6-E9AB-4D5E-9198-BD321A279FA9}
const GUID guid_mainmenu_group = {0x2cb9b2a6, 0xe9ab, 0x4d5e, {0x91, 0x98, 0xbd, 0x32, 0x1a, 0x27, 0x9f, 0xa9}};
// {95D9B1AA-13D8-4BCD-B7AA-C8DD684281E1}
const GUID guid_cmd_library = {0x95d9b1aa, 0x13d8, 0x4bcd, {0xb7, 0xaa, 0xc8, 0xdd, 0x68, 0x42, 0x81, 0xe1}};
// {09D264D5-96D2-4E88-BF96-0E6CD7102C28}
const GUID guid_cmd_current = {0x09d264d5, 0x96d2, 0x4e88, {0xbf, 0x96, 0x0e, 0x6c, 0xd7, 0x10, 0x2c, 0x28}};
// {AB3271A6-B494-48BF-AAB1-F463CC35F85E}
const GUID guid_cmd_status = {0xab3271a6, 0xb494, 0x48bf, {0xaa, 0xb1, 0xf4, 0x63, 0xcc, 0x35, 0xf8, 0x5e}};
// {243C4433-D970-43CC-B3F3-CDB53AEE43A0}
const GUID guid_ctx_selected = {0x243c4433, 0xd970, 0x43cc, {0xb3, 0xf3, 0xcd, 0xb5, 0x3a, 0xee, 0x43, 0xa0}};

const char* const k_title = "True Random";

void report(const ftr::operation_result& r, const char* what) {
    std::string line = std::string("foo_true_random: ") + what + ": " + r.message;
    console::print(line.c_str());
    if (!r.ok) popup_message::g_show(r.message.c_str(), k_title, popup_message::icon_error);
}

void report_failure(const char* what, const char* msg) {
    std::string line = std::string("foo_true_random: ") + what + ": " + msg;
    console::print(line.c_str());
    popup_message::g_show(msg, k_title, popup_message::icon_error);
}

void run_new_playlist(const std::vector<handle>& items, const char* kind, const char* what) {
    ftr::hardware_rng rng(ftr::cpu_instruction_provider());
    ftr_fb2k::playlist_manager_backend pm;
    const std::string name = std::string("True Random - ") + kind;
    const ftr::operation_result r = ftr::create_shuffled_playlist(pm, rng, items, name);
    report(r, what);
}

void cmd_entire_library() {
    auto lm = library_manager::get();
    if (!lm->is_library_enabled()) {
        report_failure("Entire Media Library",
                       "The Media Library is not configured (no Media Library folders). Nothing was changed.");
        return;
    }
    metadb_handle_list all;
    lm->get_all_items(all);
    std::vector<handle> items;
    ftr_fb2k::to_vector(all, items);
    if (items.empty()) {
        report_failure("Entire Media Library", "The Media Library is empty. Nothing was changed.");
        return;
    }
    run_new_playlist(items, "Library", "Entire Media Library");
}

void cmd_current_playlist() {
    ftr::hardware_rng rng(ftr::cpu_instruction_provider());
    ftr_fb2k::playlist_manager_backend pm;
    const ftr::operation_result r = ftr::shuffle_playlist_in_place(pm, rng, pm.active_playlist());
    report(r, "Current Playlist");
}

void cmd_status() {
    const ftr::cpuid_snapshot snap = ftr::read_cpuid_snapshot();
    const ftr::cpu_features f = ftr::decode_cpu_features(snap);
    ftr::hardware_rng rng(ftr::cpu_instruction_provider());
    std::string msg;
    msg += "CPU capability (CPUID): " + ftr::describe_features(f) + "\n";
    char raw[160];
    std::snprintf(raw, sizeof raw, "CPUID max leaf 0x%X, leaf1.ECX bit30=%u, leaf7.EBX bit18=%u\n",
                  snap.max_basic_leaf, (snap.leaf1_ecx >> 30) & 1u, (snap.leaf7_0_ebx >> 18) & 1u);
    msg += raw;
    if (rng.usable()) {
        std::uint64_t probe = 0;
        if (rng.next_u64(probe)) {
            msg += std::string("Active RNG source: ") + ftr::to_string(rng.last_source()) +
                   " (verified by drawing a value now)\n";
        } else {
            msg += "Hardware RNG failed while drawing a value: " + rng.failure_reason() + "\n";
        }
    } else {
        msg += "Hardware RNG unavailable: " + rng.failure_reason() + "\n";
    }
    msg +=
        "\nRandomness comes only from the CPU's RDSEED/RDRAND instructions. There is no software or "
        "operating-system fallback. This component does not and cannot certify the quality of the "
        "CPU's internal entropy source.";
    console::print(("foo_true_random: status: " + msg).c_str());
    popup_message::g_show(msg.c_str(), "True Random: Hardware RNG Status");
}

template <class F>
void guarded(const char* what, F&& f) {
    try {
        f();
    } catch (const std::exception& e) {
        std::string m = std::string("Operation failed: ") + e.what();
        report_failure(what, m.c_str());
    }
}

// ------------------------------------------------------------------- main menu

mainmenu_group_factory g_mainmenu_group(guid_mainmenu_group, mainmenu_groups::edit,
                                        mainmenu_commands::sort_priority_dontcare);

class true_random_mainmenu : public mainmenu_commands {
public:
    enum { cmd_library = 0, cmd_current, cmd_status_idx, cmd_total };

    t_uint32 get_command_count() override { return cmd_total; }

    GUID get_command(t_uint32 p_index) override {
        switch (p_index) {
            case cmd_library: return guid_cmd_library;
            case cmd_current: return guid_cmd_current;
            case cmd_status_idx: return guid_cmd_status;
            default: return pfc::guid_null;
        }
    }

    void get_name(t_uint32 p_index, pfc::string_base& p_out) override {
        switch (p_index) {
            case cmd_library: p_out = "True Random: Entire Media Library"; break;
            case cmd_current: p_out = "True Random: Current Playlist"; break;
            case cmd_status_idx: p_out = "True Random: Hardware RNG Status"; break;
            default: p_out = ""; break;
        }
    }

    bool get_description(t_uint32 p_index, pfc::string_base& p_out) override {
        switch (p_index) {
            case cmd_library:
                p_out = "Creates a new playlist containing the entire Media Library in hardware-random order (RDSEED/RDRAND only).";
                return true;
            case cmd_current:
                p_out = "Reorders the active playlist into hardware-random order (RDSEED/RDRAND only). Undoable.";
                return true;
            case cmd_status_idx:
                p_out = "Shows which hardware random-number instruction is available and in use.";
                return true;
            default: return false;
        }
    }

    GUID get_parent() override { return guid_mainmenu_group; }

    void execute(t_uint32 p_index, service_ptr_t<service_base> p_callback) override {
        (void)p_callback;
        switch (p_index) {
            case cmd_library: guarded("Entire Media Library", cmd_entire_library); break;
            case cmd_current: guarded("Current Playlist", cmd_current_playlist); break;
            case cmd_status_idx: guarded("Hardware RNG Status", cmd_status); break;
            default: break;
        }
    }
};

mainmenu_commands_factory_t<true_random_mainmenu> g_mainmenu_factory;

// ---------------------------------------------------------------- context menu

class true_random_contextmenu : public contextmenu_item_simple {
public:
    unsigned get_num_items() override { return 1; }

    void get_item_name(unsigned p_index, pfc::string_base& p_out) override {
        (void)p_index;
        p_out = "True Random: Selected Tracks";
    }

    void context_command(unsigned p_index, metadb_handle_list_cref p_data, const GUID& p_caller) override {
        (void)p_index;
        (void)p_caller;
        guarded("Selected Tracks", [&] {
            std::vector<handle> items;
            ftr_fb2k::to_vector(p_data, items);  // preserves order and multiplicity as given
            if (items.empty()) {
                report_failure("Selected Tracks", "No tracks are selected. Nothing was changed.");
                return;
            }
            run_new_playlist(items, "Selection", "Selected Tracks");
        });
    }

    bool context_get_display(unsigned p_index, metadb_handle_list_cref p_data, pfc::string_base& p_out,
                             unsigned& p_displayflags, const GUID& p_caller) override {
        (void)p_caller;
        get_item_name(p_index, p_out);
        p_displayflags = (p_data.get_count() == 0) ? FLAG_DISABLED_GRAYED : 0;
        return true;
    }

    GUID get_item_guid(unsigned p_index) override {
        (void)p_index;
        return guid_ctx_selected;
    }

    bool get_item_description(unsigned p_index, pfc::string_base& p_out) override {
        (void)p_index;
        p_out = "Creates a new playlist containing the selected tracks in hardware-random order (RDSEED/RDRAND only).";
        return true;
    }

    GUID get_parent() override { return contextmenu_groups::root; }
};

contextmenu_item_factory_t<true_random_contextmenu> g_contextmenu_factory;

}  // namespace
