/* Edit an existing VM. Mirrors the wizard layout (Basics / Boot media /
 * Advanced / Experimental) and supports renaming the VM by moving its
 * bundle directory on Save. */

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"

#include "launcher_state.h"
#include "ui_filepick.h"
#include "vm_bundle.h"
#include "vm_config_json.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static bool         s_loaded = false;
static int          s_loaded_index = -1;
static vm_bundle_t  s_vm;
static char         s_status[256];

/* Name edit buffer; separate from s_vm.name so we only rename on Save. */
static char         s_name[BE300_VM_NAME_MAX];

/* Bridge / tee buffers — exposed as InputText so we copy out of cfg's
 * const char* into editable storage, and back on commit. */
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
    snprintf(s_name, sizeof s_name, "%s", s_vm.name);
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
        igInputText("Name", s_name, sizeof s_name, 0, NULL, NULL);

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
        igCheckbox("Show BE-300 bezel", &c->frame_visible);
        int scale_int = (int)(c->scale + 0.5);
        if (scale_int < 1) scale_int = 1;
        if (scale_int > 4) scale_int = 4;
        if (igSliderInt("Render scale", &scale_int, 1, 4, "%dx",
                ImGuiSliderFlags_AlwaysClamp)) {
            c->scale = (double)scale_int;
        }
    }

    if (igCollapsingHeader_TreeNodeFlags("Boot media", 0)) {
        igText("CF slot 0: %s",
            c->cf_paths[0] ? c->cf_paths[0] : "(empty)");
        igSameLine(0.0f, 8.0f);
        if (igButton("Replace...##cf0", (ImVec2){0,0})) {
            char picked[BE300_VM_PATH_MAX];
            if (ui_filepick_open("Select CF image", "bin,img,iso", NULL,
                    picked, sizeof picked)) {
                if (vm_bundle_import_cf(&s_vm, 0, picked) == 0) {
                    snprintf(s_status, sizeof s_status, "CF slot 0 replaced.");
                } else {
                    snprintf(s_status, sizeof s_status, "CF slot 0 replace failed.");
                }
            }
        }
    }

    if (igCollapsingHeader_TreeNodeFlags("Advanced", 0)) {
        int target_mhz = (int)c->target_mhz;
        if (igSliderInt("Target MHz (0 = unthrottled)", &target_mhz, 0, 600,
                "%d", ImGuiSliderFlags_AlwaysClamp)) {
            c->target_mhz = (uint32_t)target_mhz;
        }
        igCheckbox("Initialise RTC from host time", &c->enable_rtc_host_time);
        igCheckbox("Casio AIU audio path", &c->enable_audio);
        igCheckbox("PCMCIA NE2000 Ethernet", &c->enable_ne2000);
        igCheckbox("Stowaway dock keyboard", &c->enable_stowaway_keyboard);
    }

    if (igCollapsingHeader_TreeNodeFlags(
            "Experimental (rarely needed; may break boot)", 0)) {
        int sdram_mb = (int)(c->sdram_size / (1024u * 1024u));
        if (igSliderInt("SDRAM (MB)", &sdram_mb, 8, 64, "%d",
                ImGuiSliderFlags_AlwaysClamp)) {
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
        igCheckbox("PPSH debug shell", &c->enable_ppsh);
        igCheckbox("Recovery boot (--restore)", &c->restore);

        igText("CF slot 1: %s",
            c->cf_paths[1] ? c->cf_paths[1] : "(empty)");
        igSameLine(0.0f, 8.0f);
        if (igButton("Replace...##cf1", (ImVec2){0,0})) {
            char picked[BE300_VM_PATH_MAX];
            if (ui_filepick_open("Select CF image", "bin,img,iso", NULL,
                    picked, sizeof picked)) {
                if (vm_bundle_import_cf(&s_vm, 1, picked) == 0) {
                    snprintf(s_status, sizeof s_status, "CF slot 1 replaced.");
                } else {
                    snprintf(s_status, sizeof s_status, "CF slot 1 replace failed.");
                }
            }
        }

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
        /* If the user changed the name, rename the bundle dir first; on
         * success vm_bundle_rename rewrites s_vm.path and rebases any
         * cfg paths that pointed at the old location. */
        if (s_name[0] && strcmp(s_name, s_vm.name) != 0) {
            errno = 0;
            if (vm_bundle_rename(&s_vm, s_name) != 0) {
                int e = errno;
                const char *why = (e == EEXIST)
                    ? "a VM with that name already exists"
                    : (e ? strerror(e) : "unknown error");
                snprintf(s_status, sizeof s_status,
                    "Rename failed: %s", why);
                igEnd();
                return;
            }
        }
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
