/* New VM wizard — single modal-style window with three sections.
 *
 * The wizard owns a pair of buffers (name + initial NAND path) so the user
 * can fill them out incrementally; only on "Create" do we commit by calling
 * vm_bundle_create() + vm_bundle_import_nand(). */

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
static int   g_target_mhz = 166;
static bool  g_frame_visible = false;
static bool  g_enable_audio = false;
static bool  g_enable_rtc_host_time = false;
static bool  g_enable_ne2000 = false;
static bool  g_enable_ppsh = false;
static bool  g_enable_stowaway = false;
static int   g_fb_choice = 0;       /* 0 = 240x320, 1 = 480x640 */
static float g_scale = 1.0f;
static char  g_status[256];

static void wizard_reset(void)
{
    g_name[0] = g_nand[0] = g_cf0[0] = g_cf1[0] = g_status[0] = '\0';
    g_sdram_mb = 16;
    g_target_mhz = 166;
    g_frame_visible = false;
    g_enable_audio = false;
    g_enable_rtc_host_time = false;
    g_enable_ne2000 = false;
    g_enable_ppsh = false;
    g_enable_stowaway = false;
    g_fb_choice = 0;
    g_scale = 1.0f;
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
    cfg->scale = (double)g_scale;
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

        igSliderInt("SDRAM (MB)", &g_sdram_mb, 8, 64, "%d", 0);
        const char *fb_items[] = { "240x320", "480x640 (experimental)" };
        igCombo_Str_arr("Framebuffer", &g_fb_choice, fb_items, 2, -1);
        igCheckbox("Show BE-300 bezel", &g_frame_visible);
        igSliderFloat("Render scale", &g_scale, 1.0f, 4.0f, "%.2fx", 0);
    }

    igSpacing();

    if (igCollapsingHeader_TreeNodeFlags("Boot media / CF", 0)) {
        igInputText("CF slot 0", g_cf0, sizeof g_cf0, 0, NULL, NULL);
        igSameLine(0.0f, 4.0f);
        if (igButton("Browse...##cf0", (ImVec2){0,0})) {
            ui_filepick_open("Select CF image", "bin,img,iso", NULL,
                g_cf0, sizeof g_cf0);
        }
        igInputText("CF slot 1", g_cf1, sizeof g_cf1, 0, NULL, NULL);
        igSameLine(0.0f, 4.0f);
        if (igButton("Browse...##cf1", (ImVec2){0,0})) {
            ui_filepick_open("Select CF image", "bin,img,iso", NULL,
                g_cf1, sizeof g_cf1);
        }
        igCheckbox("PPSH debug shell", &g_enable_ppsh);
    }

    igSpacing();

    if (igCollapsingHeader_TreeNodeFlags("Advanced", 0)) {
        igSliderInt("Target MHz (throttle)", &g_target_mhz, 50, 600, "%d", 0);
        igCheckbox("Initialise RTC from host time", &g_enable_rtc_host_time);
        igCheckbox("Casio AIU audio path", &g_enable_audio);
        igCheckbox("PCMCIA NE2000 Ethernet", &g_enable_ne2000);
        igCheckbox("Stowaway dock keyboard", &g_enable_stowaway);
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
