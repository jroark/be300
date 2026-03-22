/*
 *  ui.c — Stub display implementation.
 *
 *  GXemul's dev_fb.c handles framebuffer emulation and X11 display.
 *  This stub satisfies the old API; a future pass can integrate SDL2
 *  with GXemul's framebuffer data via memory_paddr_to_hostaddr().
 */

#include "ui.h"

static ui_frame_callback_t frame_cb = NULL;
static void *frame_cb_data = NULL;

int  ui_init(machine_t *m) { (void)m; return 0; }
void ui_update(machine_t *m) { (void)m; }
void ui_destroy(machine_t *m) { (void)m; }
bool ui_should_quit(machine_t *m) { (void)m; return false; }
void ui_save_screenshot(machine_t *m) { (void)m; }
void ui_set_frame_callback(ui_frame_callback_t cb, void *user_data)
{
    frame_cb = cb;
    frame_cb_data = user_data;
}
