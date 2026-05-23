/* Launcher Manager view — left-rail VM list + right-pane details.
 *
 * Phase 2: layout + selection only. The "Run" button sets L->want_run; the
 * actual in-process boot is wired in Phase 3. */

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"

#include "launcher_state.h"

#include <stdio.h>
#include <string.h>

void launcher_refresh_vms(launcher_state_t *L)
{
    vm_bundle_list_free(&L->vms);
    if (vm_bundle_list(&L->vms) != 0) {
        L->vms.items = NULL;
        L->vms.count = 0;
    }
    if (L->selected_vm >= (int)L->vms.count) L->selected_vm = -1;
    if (L->selected_vm < 0 && L->vms.count > 0) L->selected_vm = 0;
}

static const ImVec2 ZERO = {0.0f, 0.0f};

static void draw_details_pane(launcher_state_t *L)
{
    if (L->selected_vm < 0 || L->selected_vm >= (int)L->vms.count) {
        igTextDisabled("Select a VM on the left, or click \"New VM\" to create one.");
        return;
    }
    const vm_bundle_t *vm = &L->vms.items[L->selected_vm];

    igPushFont(NULL, 18.0f);
    igText("%s", vm->name);
    igPopFont();

    igTextDisabled("%s", vm->path);
    igSeparator();

    igText("NAND:        %s", vm->cfg.nand_path ? vm->cfg.nand_path : "(none)");
    igText("CF slots:    %u", vm->cfg.cf_count);
    for (unsigned i = 0; i < vm->cfg.cf_count; i++) {
        igText("  slot %u:    %s", i, vm->cfg.cf_paths[i] ? vm->cfg.cf_paths[i] : "(empty)");
    }
    igText("SDRAM:       %u MB", vm->cfg.sdram_size / (1024u * 1024u));
    igText("Target MHz:  %u", vm->cfg.target_mhz);
    igText("Framebuffer: %ux%u",
        vm->cfg.fb_width ? vm->cfg.fb_width : 240u,
        vm->cfg.fb_height ? vm->cfg.fb_height : 320u);
    igText("Scale:       %.2fx", vm->cfg.scale);
    igText("Frame bezel: %s", vm->cfg.frame_visible ? "yes" : "no");
    igText("Audio:       %s", vm->cfg.enable_audio ? "on" : "off");
    igText("RTC mode:    %s", vm->cfg.enable_rtc_host_time ? "host time" : "battery-cold");
    igText("NE2000:      %s", vm->cfg.enable_ne2000 ? "on" : "off");
    igText("PPSH:        %s", vm->cfg.enable_ppsh ? "on" : "off");
    igText("Stowaway:    %s", vm->cfg.enable_stowaway_keyboard ? "on" : "off");
    if (vm->cfg.pcconnect_bridge)
        igText("PCConnect:   %s", vm->cfg.pcconnect_bridge);
    if (vm->cfg.serial0_bridge)
        igText("Serial0:     %s", vm->cfg.serial0_bridge);
    if (vm->cfg.serial1_bridge)
        igText("Serial1:     %s", vm->cfg.serial1_bridge);

    igSpacing();
    igSeparator();
    igSpacing();

    const ImVec2 btn = {110.0f, 0.0f};
    if (igButton("Run", btn)) L->want_run = true;
    igSameLine(0.0f, 6.0f);
    if (igButton("Edit", btn)) L->view = LAUNCHER_VIEW_SETTINGS;
    igSameLine(0.0f, 6.0f);
    if (igButton("Delete", btn)) {
        igOpenPopup_Str("Confirm Delete", 0);
    }

    if (igBeginPopupModal("Confirm Delete", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        igText("Permanently delete \"%s\"?", vm->name);
        igTextDisabled("%s", vm->path);
        igSpacing();
        if (igButton("Delete", (ImVec2){100, 0})) {
            vm_bundle_delete(vm);
            igCloseCurrentPopup();
            launcher_refresh_vms(L);
        }
        igSameLine(0.0f, 8.0f);
        if (igButton("Cancel", (ImVec2){100, 0})) igCloseCurrentPopup();
        igEndPopup();
    }
}

static void draw_left_rail(launcher_state_t *L)
{
    if (igButton("+ New VM", (ImVec2){-1, 0})) {
        L->view = LAUNCHER_VIEW_WIZARD;
    }
    igSameLine(0.0f, 4.0f);

    igSpacing();
    igSeparator();
    igSpacing();

    if (L->vms.count == 0) {
        igTextDisabled("No VMs yet.");
        return;
    }

    igBeginChild_Str("vm_list", ZERO, ImGuiChildFlags_Borders, 0);
    for (size_t i = 0; i < L->vms.count; i++) {
        bool selected = ((int)i == L->selected_vm);
        char label[256];
        snprintf(label, sizeof label, "%s##vm%zu", L->vms.items[i].name, i);
        if (igSelectable_Bool(label, selected, 0, ZERO)) {
            L->selected_vm = (int)i;
        }
    }
    igEndChild();
}

void launcher_manager_draw(launcher_state_t *L)
{
    ImGuiViewport *vp = igGetMainViewport();
    igSetNextWindowPos(vp->WorkPos, ImGuiCond_Always, ZERO);
    igSetNextWindowSize(vp->WorkSize, ImGuiCond_Always);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    if (!igBegin("BE-300 VM Manager", NULL, flags)) { igEnd(); return; }

    /* Two-column layout: left rail (250px) + details. */
    igColumns(2, "main_cols", true);
    igSetColumnWidth(0, 250.0f);

    draw_left_rail(L);
    igNextColumn();
    draw_details_pane(L);

    igColumns(1, NULL, false);
    igEnd();
}
