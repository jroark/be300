/* New VM wizard.
 *
 * Sections (top to bottom):
 *   - Basics:        name, NAND, frame, scale (integer 1..4x)
 *   - Advanced:      CF slot 0, throttle MHz (default 0 = unthrottled),
 *                    RTC, audio, NE2000, Stowaway
 *   - Experimental:  SDRAM, framebuffer geometry, PPSH, recovery boot,
 *                    CF slot 1, MMIO coverage, stall detector,
 *                    PCConnect / serial0 / serial1 bridges
 *
 * The wizard's static state holds the form values; vm_bundle_create()
 * runs only on "Create". */

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"

#include "launcher_state.h"
#include "ui_filepick.h"
#include "vm_bundle.h"
#include "vm_config_json.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static char  g_name[BE300_VM_NAME_MAX];
static char  g_nand[BE300_VM_PATH_MAX];
static char  g_cf0[BE300_VM_PATH_MAX];
static char  g_cf1[BE300_VM_PATH_MAX];
static int   g_sdram_mb = 16;
static int   g_target_mhz = 0;       /* unthrottled */
static bool  g_frame_visible = false;
static bool  g_enable_audio = false;
static bool  g_enable_rtc_host_time = false;
static bool  g_enable_ne2000 = false;
static bool  g_enable_ppsh = false;
static bool  g_enable_stowaway = false;
static bool  g_restore = false;
static bool  g_mmio_coverage = false;
static bool  g_detect_stall = false;
static int   g_fb_choice = 0;        /* 0 = 240x320, 1 = 480x640 */
static int   g_scale_int = 1;        /* integer render scale 1..4 */
static char  g_pcc_bridge[BE300_VM_BRIDGE_MAX];
static char  g_serial0_bridge[BE300_VM_BRIDGE_MAX];
static char  g_serial1_bridge[BE300_VM_BRIDGE_MAX];
static char  g_status[256];

static void wizard_reset(void)
{
    g_name[0] = g_nand[0] = g_cf0[0] = g_cf1[0] = g_status[0] = '\0';
    g_pcc_bridge[0] = g_serial0_bridge[0] = g_serial1_bridge[0] = '\0';
    g_sdram_mb = 16;
    g_target_mhz = 0;
    g_frame_visible = false;
    g_enable_audio = false;
    g_enable_rtc_host_time = false;
    g_enable_ne2000 = false;
    g_enable_ppsh = false;
    g_enable_stowaway = false;
    g_restore = false;
    g_mmio_coverage = false;
    g_detect_stall = false;
    g_fb_choice = 0;
    g_scale_int = 1;
}

static void apply_to_cfg(machine_config_t *cfg)
{
    cfg->sdram_size = (uint32_t)g_sdram_mb * 1024u * 1024u;
    cfg->target_mhz = (uint32_t)g_target_mhz;
    cfg->frame_visible = g_frame_visible;
    cfg->enable_audio = g_enable_audio;
    cfg->enable_rtc_host_time = g_enable_rtc_host_time;
    cfg->enable_ne2000 = g_enable_ne2000;
    cfg->enable_ppsh = g_enable_ppsh;
    cfg->enable_stowaway_keyboard = g_enable_stowaway;
    cfg->restore = g_restore;
    cfg->mmio_coverage = g_mmio_coverage;
    cfg->detect_stall = g_detect_stall;
    cfg->scale = (double)g_scale_int;
    if (g_fb_choice == 1) {
        cfg->fb_width = 480; cfg->fb_height = 640; cfg->fb_stride = 512;
    } else {
        cfg->fb_width = 0; cfg->fb_height = 0; cfg->fb_stride = 0;
    }
}

void launcher_wizard_draw(launcher_state_t *L)
{
    static bool first_time = true;
    if (first_time) {
        wizard_reset();
        first_time = false;
    }

    ImGuiViewport *vp = igGetMainViewport();
    igSetNextWindowPos(vp->WorkPos, ImGuiCond_Always, (ImVec2){0,0});
    igSetNextWindowSize(vp->WorkSize, ImGuiCond_Always);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    if (!igBegin("New VM", NULL, flags)) { igEnd(); return; }

    igPushFont(NULL, 18.0f);
    igText("Create a new BE-300 VM");
    igPopFont();
    igSpacing();
    igSeparator();
    igSpacing();

    if (igCollapsingHeader_TreeNodeFlags("Basics", ImGuiTreeNodeFlags_DefaultOpen)) {
        igInputText("Name", g_name, sizeof g_name, 0, NULL, NULL);

        igInputText("NAND image", g_nand, sizeof g_nand, 0, NULL, NULL);
        igSameLine(0.0f, 4.0f);
        if (igButton("Browse...##nand", (ImVec2){0,0})) {
            ui_filepick_open("Select NAND image", "bin,img", NULL,
                g_nand, sizeof g_nand);
        }
        igCheckbox("Show BE-300 bezel", &g_frame_visible);
        igSliderInt("Render scale", &g_scale_int, 1, 4, "%dx",
            ImGuiSliderFlags_AlwaysClamp);
    }

    igSpacing();

    if (igCollapsingHeader_TreeNodeFlags("Advanced", 0)) {
        igInputText("CF slot 0", g_cf0, sizeof g_cf0, 0, NULL, NULL);
        igSameLine(0.0f, 4.0f);
        if (igButton("Browse...##cf0", (ImVec2){0,0})) {
            ui_filepick_open("Select CF image", "bin,img,iso", NULL,
                g_cf0, sizeof g_cf0);
        }
        igSliderInt("Target MHz (0 = unthrottled)",
            &g_target_mhz, 0, 600, "%d", ImGuiSliderFlags_AlwaysClamp);
        igCheckbox("Initialise RTC from host time", &g_enable_rtc_host_time);
        igCheckbox("Casio AIU audio path", &g_enable_audio);
        igCheckbox("PCMCIA NE2000 Ethernet", &g_enable_ne2000);
        igCheckbox("Stowaway dock keyboard", &g_enable_stowaway);
    }

    igSpacing();

    if (igCollapsingHeader_TreeNodeFlags(
            "Experimental (rarely needed; may break boot)", 0)) {
        igSliderInt("SDRAM (MB)", &g_sdram_mb, 8, 64, "%d",
            ImGuiSliderFlags_AlwaysClamp);
        const char *fb_items[] = { "240x320", "480x640 (experimental)" };
        igCombo_Str_arr("Framebuffer", &g_fb_choice, fb_items, 2, -1);
        igCheckbox("PPSH debug shell", &g_enable_ppsh);
        igCheckbox("Recovery boot (--restore; needs CF slot 0)", &g_restore);

        igInputText("CF slot 1", g_cf1, sizeof g_cf1, 0, NULL, NULL);
        igSameLine(0.0f, 4.0f);
        if (igButton("Browse...##cf1", (ImVec2){0,0})) {
            ui_filepick_open("Select CF image", "bin,img,iso", NULL,
                g_cf1, sizeof g_cf1);
        }

        igCheckbox("MMIO coverage trace", &g_mmio_coverage);
        igCheckbox("Stall detector", &g_detect_stall);

        igInputText("PCConnect bridge spec",
            g_pcc_bridge, sizeof g_pcc_bridge, 0, NULL, NULL);
        igInputText("serial0 bridge spec",
            g_serial0_bridge, sizeof g_serial0_bridge, 0, NULL, NULL);
        igInputText("serial1 bridge spec",
            g_serial1_bridge, sizeof g_serial1_bridge, 0, NULL, NULL);
    }

    igSpacing();
    igSeparator();
    igSpacing();

    if (g_status[0]) {
        igTextDisabled("%s", g_status);
    }

    bool can_create = (g_name[0] != '\0');
    igBeginDisabled(!can_create);
    if (igButton("Create", (ImVec2){120, 0})) {
        vm_bundle_t vm;
        errno = 0;
        if (vm_bundle_create(g_name, &vm) != 0) {
            int e = errno;
            const char *why = (e == EEXIST)
                ? "a VM with that name already exists"
                : (e ? strerror(e) : "unknown error");
            snprintf(g_status, sizeof g_status,
                "Could not create bundle: %s", why);
        } else {
            apply_to_cfg(&vm.cfg);
            /* Bridge specs are not under the bundle dir; copy into the
             * vm's owned buffers and rebind so they survive save+reload. */
            snprintf(vm.pcconnect_bridge_buf,
                sizeof vm.pcconnect_bridge_buf, "%s", g_pcc_bridge);
            snprintf(vm.serial0_bridge_buf,
                sizeof vm.serial0_bridge_buf, "%s", g_serial0_bridge);
            snprintf(vm.serial1_bridge_buf,
                sizeof vm.serial1_bridge_buf, "%s", g_serial1_bridge);
            vm_bundle_rebind_strings(&vm);

            if (g_nand[0]) {
                if (vm_bundle_import_nand(&vm, g_nand) != 0) {
                    snprintf(g_status, sizeof g_status,
                        "NAND import failed; bundle created without NAND.");
                }
            }
            if (g_cf0[0]) {
                if (vm_bundle_import_cf(&vm, 0, g_cf0) != 0) {
                    snprintf(g_status, sizeof g_status,
                        "CF slot 0 import failed.");
                }
            }
            if (g_cf1[0]) {
                if (vm_bundle_import_cf(&vm, 1, g_cf1) != 0) {
                    snprintf(g_status, sizeof g_status,
                        "CF slot 1 import failed.");
                }
            }
            vm_bundle_save(&vm);
            wizard_reset();
            first_time = true;
            L->view = LAUNCHER_VIEW_MANAGER;
            launcher_refresh_vms(L);
        }
    }
    igEndDisabled();
    igSameLine(0.0f, 8.0f);
    if (igButton("Cancel", (ImVec2){120, 0})) {
        wizard_reset();
        first_time = true;
        L->view = LAUNCHER_VIEW_MANAGER;
    }

    igEnd();
}
