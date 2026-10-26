/* Exercise the real SDL event loop with a dummy display and recording guest
 * input sinks. No guest binary or hardware register behavior is synthesized. */
#include "ui_internal.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static struct { unsigned value; bool release; } dock[64];
static size_t dock_count;

void be300_set_buttons(machine_t *m, uint8_t set1, uint8_t set2)
{ m->btn_set1 = set1; m->btn_set2 = set2; }
void be300_set_touch(machine_t *m, bool down, uint16_t x, uint16_t y)
{ m->touch_down = down; m->touch_x = x; m->touch_y = y; }
bool stowaway_queue_mapped_key(unsigned value, bool release)
{
    CHECK(dock_count < sizeof(dock) / sizeof(dock[0]));
    dock[dock_count].value = value;
    dock[dock_count++].release = release;
    return true;
}
void ui_audio_init(void) {}
void ui_audio_destroy(void) {}
void ui_macos_make_window_transparent(SDL_Window *win) { (void)win; }
void ui_macos_apply_window_mask(SDL_Window *win, const uint8_t *mask, int w, int h)
{ (void)win; (void)mask; (void)w; (void)h; }

static void key(machine_t *m, SDL_Scancode code, bool release, unsigned repeat, SDL_Keymod mod)
{
    SDL_Event e = {0};
    e.type = release ? SDL_KEYUP : SDL_KEYDOWN;
    e.key.keysym.scancode = code;
    e.key.keysym.sym = SDL_GetKeyFromScancode(code);
    e.key.keysym.mod = mod;
    e.key.repeat = repeat;
    CHECK(SDL_PushEvent(&e) == 1);
    ui_update(m);
}

static void lose_focus(machine_t *m)
{
    SDL_Event e = {0};
    e.type = SDL_WINDOWEVENT;
    e.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    CHECK(SDL_PushEvent(&e) == 1);
    ui_update(m);
}

int main(void)
{
    machine_t m = {0};
    CHECK(ui_init(&m) == 0);
    CHECK(m.sdl_window != NULL);
    ui_update(&m); /* discard startup window events */

    key(&m, SDL_SCANCODE_RETURN, false, 0, KMOD_NONE);
    key(&m, SDL_SCANCODE_KP_ENTER, false, 0, KMOD_NONE);
    ui_buttons_set_pointer(&m, 4, 0);
    key(&m, SDL_SCANCODE_RETURN, true, 0, KMOD_NONE);
    CHECK(m.btn_set1 == 4);
    ui_buttons_set_pointer(&m, 0, 0);
    CHECK(m.btn_set1 == 4); /* keypad still owns OK */
    key(&m, SDL_SCANCODE_KP_ENTER, true, 0, KMOD_NONE);
    CHECK(m.btn_set1 == 0);
    key(&m, SDL_SCANCODE_UP, false, 0, KMOD_NONE);
    key(&m, SDL_SCANCODE_F12, false, 0, KMOD_NONE);
    CHECK(m.btn_set1 == 16 && m.btn_set2 == 128);
    key(&m, SDL_SCANCODE_F12, true, 0, KMOD_NONE);
    CHECK(m.btn_set1 == 16 && m.btn_set2 == 0);

    SDL_Event touch = {0};
    touch.type = SDL_MOUSEBUTTONDOWN;
    touch.button.button = SDL_BUTTON_LEFT;
    touch.button.x = touch.button.y = 30;
    CHECK(SDL_PushEvent(&touch) == 1);
    ui_update(&m);
    CHECK(m.touch_down);
    lose_focus(&m);
    CHECK(!m.touch_down && !m.btn_set1 && !m.btn_set2);
    key(&m, SDL_SCANCODE_UP, false, 0, KMOD_NONE);
    CHECK(m.btn_set1 == 16);
    key(&m, SDL_SCANCODE_UP, true, 0, KMOD_NONE);
    CHECK(m.btn_set1 == 0);

    m.cfg.enable_stowaway_keyboard = true;
    key(&m, SDL_SCANCODE_LCTRL, false, 0, KMOD_NONE);
    key(&m, SDL_SCANCODE_LCTRL, false, 0, KMOD_NONE); /* duplicate physical down */
    key(&m, SDL_SCANCODE_RCTRL, false, 0, KMOD_NONE);
    key(&m, SDL_SCANCODE_LCTRL, true, 0, KMOD_NONE);
    CHECK(dock_count == 1 && dock[0].value == 26 && !dock[0].release);
    key(&m, SDL_SCANCODE_RCTRL, true, 0, KMOD_GUI);
    CHECK(dock_count == 2 && dock[1].value == 26 && dock[1].release);
    key(&m, SDL_SCANCODE_A, false, 0, KMOD_NONE);
    key(&m, SDL_SCANCODE_A, false, 1, KMOD_NONE);
    CHECK(dock_count == 3 && dock[2].value == 17);
    key(&m, SDL_SCANCODE_F12, false, 0, KMOD_NONE);
    CHECK(m.btn_set2 == 128); /* Power remains available with the dock */
    lose_focus(&m);
    CHECK(dock_count == 4 && dock[3].value == 17 && dock[3].release);
    CHECK(!m.btn_set1 && !m.btn_set2);
    key(&m, SDL_SCANCODE_A, true, 0, KMOD_NONE);
    CHECK(dock_count == 4);
    key(&m, SDL_SCANCODE_A, false, 0, KMOD_NONE);
    CHECK(dock_count == 5 && !dock[4].release);
    ui_destroy(&m);
    CHECK(dock_count == 6 && dock[5].release);
    CHECK(!m.btn_set1 && !m.btn_set2);
    CHECK(ui_init(&m) == 0 && m.sdl_window != NULL);
    ui_update(&m);
    key(&m, SDL_SCANCODE_A, false, 0, KMOD_NONE);
    CHECK(dock_count == 7 && !dock[6].release);
    ui_destroy(&m);
    CHECK(dock_count == 8 && dock[7].release);
    puts("SDL input ownership and focus release passed");
    return 0;
}
