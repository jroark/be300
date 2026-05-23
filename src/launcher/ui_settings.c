/* Settings — edit an existing VM. Mirrors the wizard but operates on a
 * working copy of the selected VM's cfg, and writes back via vm_bundle_save
 * on Save. NAND/CF "Replace..." buttons re-copy into the bundle. */

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"

#include "launcher_state.h"
#include "ui_filepick.h"
#include "vm_bundle.h"
#include "vm_config_json.h"

#include <stdio.h>
#include <string.h>

static bool         s_loaded = false;
static int          s_loaded_index = -1;
static vm_bundle_t  s_vm;
static char         s_status[256];
static char         s_pcconnect_bridge[BE300_VM_BRIDGE_MAX];
static char         s_pcconnect_tee[BE300_VM_PATH_MAX];
static char         s_serial0_bridge[BE300_VM_BRIDGE_MAX];
static char         s_serial0_tee[BE300_VM_PATH_MAX];
static char         s_serial1_bridge[BE300_VM_BRIDGE_MAX];
static char         s_serial1_tee[BE300_VM_PATH_MAX];

static void copy_buf_safe(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (src) snprintf(dst, cap, "%s", src);
    else dst[0] = '\0';
}

static void load_from(const vm_bundle_t *src)
{
    memcpy(&s_vm, src, sizeof s_vm);
    vm_bundle_rebind_strings(&s_vm);
    copy_buf_safe(s_pcconnect_bridge, sizeof s_pcconnect_bridge, s_vm.cfg.pcconnect_bridge);
    copy_buf_safe(s_pcconnect_tee,    sizeof s_pcconnect_tee,    s_vm.cfg.pcconnect_tee);
    copy_buf_safe(s_serial0_bridge,   sizeof s_serial0_bridge,   s_vm.cfg.serial0_bridge);
    copy_buf_safe(s_serial0_tee,      sizeof s_serial0_tee,      s_vm.cfg.serial0_tee);
    copy_buf_safe(s_serial1_bridge,   sizeof s_serial1_bridge,   s_vm.cfg.serial1_bridge);
    copy_buf_safe(s_serial1_tee,      sizeof s_serial1_tee,      s_vm.cfg.serial1_tee);
    s_status[0] = '\0';
}

static void commit_buffers_back(void)
{
    snprintf(s_vm.pcconnect_bridge_buf, sizeof s_vm.pcconnect_bridge_buf, "%s", s_pcconnect_bridge);
    snprintf(s_vm.pcconnect_tee_buf,    sizeof s_vm.pcconnect_tee_buf,    "%s", s_pcconnect_tee);
    snprintf(s_vm.serial0_bridge_buf,   sizeof s_vm.serial0_bridge_buf,   "%s", s_serial0_bridge);
    snprintf(s_vm.serial0_tee_buf,      sizeof s_vm.serial0_tee_buf,      "%s", s_serial0_tee);
    snprintf(s_vm.serial1_bridge_buf,   sizeof s_vm.serial1_bridge_buf,   "%s", s_serial1_bridge);
    snprintf(s_vm.serial1_tee_buf,      sizeof s_vm.serial1_tee_buf,      "%s", s_serial1_tee);
    vm_bundle_rebind_strings(&s_vm);
}

void launcher_settings_reset(void)
{
    s_loaded = false;
    s_loaded_index = -1;
}

void launcher_settings_draw(launcher_state_t *L)
{
    if (L->selected_vm < 0 || L->selected_vm >= (int)L->vms.count) {
        L->view = LAUNCHER_VIEW_MANAGER;
        return;
    }
    if (!s_loaded || s_loaded_index != L->selected_vm) {
        load_from(&L->vms.items[L->selected_vm]);
        s_loaded = true;
        s_loaded_index = L->selected_vm;
    }

    ImGuiViewport *vp = igGetMainViewport();
    igSetNextWindowPos(vp->WorkPos, ImGuiCond_Always, (ImVec2){0,0});
    igSetNextWindowSize(vp->WorkSize, ImGuiCond_Always);
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    if (!igBegin("Edit VM", NULL, flags)) { igEnd(); return; }

    igPushFont(NULL, 18.0f);
    igText("%s", s_vm.name);
    igPopFont();
    igTextDisabled("%s", s_vm.path);
    igSeparator();
    igSpacing();

    machine_config_t *c = &s_vm.cfg;

    if (igCollapsingHeader_TreeNodeFlags("Basics", ImGuiTreeNodeFlags_DefaultOpen)) {
        igText("NAND: %s", c->nand_path ? c->nand_path : "(none)");
        igSameLine(0.0f, 8.0f);
        if (igButton("Replace...##nand", (ImVec2){0,0})) {
            char picked[BE300_VM_PATH_MAX];
            if (ui_filepick_open("Select NAND image", "bin,img", NULL,
                    picked, sizeof picked)) {
                if (vm_bundle_import_nand(&s_vm, picked) == 0) {
                    snprintf(s_status, sizeof s_status, "NAND replaced.");
                } else {
                    snprintf(s_status, sizeof s_status, "NAND replace failed.");
                }
            }
        }
        int sdram_mb = (int)(c->sdram_size / (1024u * 1024u));
        if (igSliderInt("SDRAM (MB)", &sdram_mb, 8, 64, "%d", 0)) {
            c->sdram_size = (uint32_t)sdram_mb * 1024u * 1024u;
        }
        int fb_choice = (c->fb_width == 480) ? 1 : 0;
        const char *fb_items[] = { "240x320", "480x640 (experimental)" };
        if (igCombo_Str_arr("Framebuffer", &fb_choice, fb_items, 2, -1)) {
            if (fb_choice == 1) {
                c->fb_width = 480; c->fb_height = 640; c->fb_stride = 512;
            } else {
                c->fb_width = 0; c->fb_height = 0; c->fb_stride = 0;
            }
        }
        igCheckbox("Show BE-300 bezel", &c->frame_visible);
        float scale = (float)c->scale;
        if (igSliderFloat("Render scale", &scale, 1.0f, 4.0f, "%.2fx", 0)) {
            c->scale = (double)scale;
        }
    }

    if (igCollapsingHeader_TreeNodeFlags("Boot media / CF", 0)) {
        for (unsigned slot = 0; slot < BE300_MAX_CF_SLOTS; slot++) {
            igText("CF slot %u: %s", slot,
                c->cf_paths[slot] ? c->cf_paths[slot] : "(empty)");
            igSameLine(0.0f, 8.0f);
            char btn_id[64];
            snprintf(btn_id, sizeof btn_id, "Replace...##cf%u", slot);
            if (igButton(btn_id, (ImVec2){0,0})) {
                char picked[BE300_VM_PATH_MAX];
                if (ui_filepick_open("Select CF image", "bin,img,iso", NULL,
                        picked, sizeof picked)) {
                    if (vm_bundle_import_cf(&s_vm, slot, picked) == 0) {
                        snprintf(s_status, sizeof s_status,
                            "CF slot %u replaced.", slot);
                    } else {
                        snprintf(s_status, sizeof s_status,
                            "CF slot %u replace failed.", slot);
                    }
                }
            }
        }
        igCheckbox("PPSH debug shell", &c->enable_ppsh);
        igCheckbox("Recovery boot (--restore)", &c->restore);
    }

    if (igCollapsingHeader_TreeNodeFlags("Advanced", 0)) {
        int target_mhz = (int)c->target_mhz;
        if (igSliderInt("Target MHz", &target_mhz, 0, 600, "%d (0=unthrottled)", 0)) {
            c->target_mhz = (uint32_t)target_mhz;
        }
        igCheckbox("Initialise RTC from host time", &c->enable_rtc_host_time);
        igCheckbox("Casio AIU audio path", &c->enable_audio);
        igCheckbox("PCMCIA NE2000 Ethernet", &c->enable_ne2000);
        igCheckbox("Stowaway dock keyboard", &c->enable_stowaway_keyboard);
        igCheckbox("MMIO coverage trace", &c->mmio_coverage);
        igCheckbox("Stall detector", &c->detect_stall);
        igInputText("PCConnect bridge spec",
            s_pcconnect_bridge, sizeof s_pcconnect_bridge, 0, NULL, NULL);
        igInputText("PCConnect tee",
            s_pcconnect_tee, sizeof s_pcconnect_tee, 0, NULL, NULL);
        igInputText("serial0 bridge spec",
            s_serial0_bridge, sizeof s_serial0_bridge, 0, NULL, NULL);
        igInputText("serial0 tee",
            s_serial0_tee, sizeof s_serial0_tee, 0, NULL, NULL);
        igInputText("serial1 bridge spec",
            s_serial1_bridge, sizeof s_serial1_bridge, 0, NULL, NULL);
        igInputText("serial1 tee",
            s_serial1_tee, sizeof s_serial1_tee, 0, NULL, NULL);
    }

    igSpacing();
    igSeparator();
    igSpacing();
    if (s_status[0]) igTextDisabled("%s", s_status);

    if (igButton("Save", (ImVec2){120, 0})) {
        commit_buffers_back();
        if (vm_bundle_save(&s_vm) == 0) {
            snprintf(s_status, sizeof s_status, "Saved.");
            launcher_refresh_vms(L);
            L->view = LAUNCHER_VIEW_MANAGER;
            launcher_settings_reset();
        } else {
            snprintf(s_status, sizeof s_status, "Save failed.");
        }
    }
    igSameLine(0.0f, 8.0f);
    if (igButton("Cancel", (ImVec2){120, 0})) {
        L->view = LAUNCHER_VIEW_MANAGER;
        launcher_settings_reset();
    }

    igEnd();
}
