// foo_true_random — main-menu and context-menu commands.
// SPDX-License-Identifier: MIT
#include "stdafx.h"

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "commands.h"
#include "guids.h"
#include "fb2k_backend.h"
#include "ftr/hardware_rng.h"
#include "ftr/playlist_logic.h"

namespace ftr_fb2k {


namespace {
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

}  // namespace

ftr::operation_result run_new_playlist(const std::vector<handle>& items, const char* kind,
                                       const char* what) {
    ftr::hardware_rng rng(ftr::cpu_instruction_provider(), {}, &ftr::os_system_rng_provider());
    playlist_manager_backend pm;
    const std::string name = std::string("True Random - ") + kind;
    const ftr::operation_result r = ftr::create_shuffled_playlist(pm, rng, items, name);
    report(r, what);
    return r;
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
    to_vector(all, items);
    if (items.empty()) {
        report_failure("Entire Media Library", "The Media Library is empty. Nothing was changed.");
        return;
    }
    run_new_playlist(items, "Library", "Entire Media Library");
}

void cmd_selected_tracks(metadb_handle_list_cref data) {
    std::vector<handle> items;
    to_vector(data, items);  // preserves order and multiplicity as given by the caller
    if (items.empty()) {
        report_failure("Selected Tracks", "No tracks are selected. Nothing was changed.");
        return;
    }
    run_new_playlist(items, "Selection", "Selected Tracks");
}

void cmd_current_playlist() {
    ftr::hardware_rng rng(ftr::cpu_instruction_provider(), {}, &ftr::os_system_rng_provider());
    playlist_manager_backend pm;
    const ftr::operation_result r = ftr::shuffle_playlist_in_place(pm, rng, pm.active_playlist());
    report(r, "Current Playlist");
}

void cmd_status() {
    const ftr::cpuid_snapshot snap = ftr::read_cpuid_snapshot();
    const ftr::cpu_features f = ftr::decode_cpu_features(snap);
    ftr::hardware_rng rng(ftr::cpu_instruction_provider(), {}, &ftr::os_system_rng_provider());
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
        msg += "Random number generator unavailable: " + rng.failure_reason() + "\n";
    }
    if (rng.using_system_rng()) {
        msg +=
            "\nThis CPU has neither RDSEED nor RDRAND, so randomness comes from the Windows system random "
            "number generator (BCryptGenRandom) instead. Shuffling is still exactly unbiased; only the "
            "source of the random values differs. On CPUs with RDSEED or RDRAND the component never uses it.";
    } else {
        msg +=
            "\nRandomness comes only from the CPU's RDSEED/RDRAND instructions; a hardware failure is refused, "
            "never replaced by software. The Windows system random number generator is used only on CPUs "
            "that have neither instruction. This component does not and cannot certify the quality of the "
            "CPU's internal entropy source.";
    }
    console::print(("foo_true_random: status: " + msg).c_str());
    popup_message::g_show(msg.c_str(), "True Random: Hardware RNG Status");
}

namespace {

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
                p_out = "Creates a new playlist containing the entire Media Library in random order (RDSEED/RDRAND; Windows RNG only on CPUs without them).";
                return true;
            case cmd_current:
                p_out = "Reorders the active playlist into random order (RDSEED/RDRAND; Windows RNG only on CPUs without them). Undoable.";
                return true;
            case cmd_status_idx:
                p_out = "Shows which random-number source is available and in use.";
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
        guarded("Selected Tracks", [&] { cmd_selected_tracks(p_data); });
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
        p_out = "Creates a new playlist containing the selected tracks in random order (RDSEED/RDRAND; Windows RNG only on CPUs without them).";
        return true;
    }

    GUID get_parent() override { return contextmenu_groups::root; }
};

contextmenu_item_factory_t<true_random_contextmenu> g_contextmenu_factory;

}  // namespace
}  // namespace ftr_fb2k
