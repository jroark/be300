/* Host sources for the existing BE-300 button inputs. Keep keyboard and
 * pointer ownership separate: releasing one source must not release another. */
#include "ui_internal.h"

#ifdef HAVE_SDL2
#include <string.h>

static bool held_keys[SDL_NUM_SCANCODES];
static uint8_t pointer_set1, pointer_set2;

static bool button_for_key(SDL_Scancode code, uint8_t *set1, uint8_t *set2)
{
    *set1 = *set2 = 0;
    switch (code) {
    case SDL_SCANCODE_UP:       *set1 = 0x10; break;
    case SDL_SCANCODE_DOWN:     *set1 = 0x20; break;
    case SDL_SCANCODE_RIGHT:    *set1 = 0x40; break;
    case SDL_SCANCODE_LEFT:     *set1 = 0x80; break;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: *set1 = 0x04; break;
    case SDL_SCANCODE_ESCAPE:   *set1 = 0x08; break;
    case SDL_SCANCODE_TAB:      *set2 = 0x10; break;
    case SDL_SCANCODE_F12:      *set2 = 0x80; break;
    default: return false;
    }
    return true;
}

static void sync_buttons(machine_t *m)
{
    uint8_t set1 = pointer_set1, set2 = pointer_set2;
    for (unsigned code = 0; code < SDL_NUM_SCANCODES; code++) {
        uint8_t key1, key2;
        if (held_keys[code] && button_for_key((SDL_Scancode)code, &key1, &key2)) {
            set1 |= key1;
            set2 |= key2;
        }
    }
    be300_set_buttons(m, set1, set2);
}

bool ui_button_key_event(machine_t *m, const SDL_KeyboardEvent *key, bool release)
{
    uint8_t set1, set2;
    SDL_Scancode code = key->keysym.scancode;
    if (!button_for_key(code, &set1, &set2))
        return false;
    if (held_keys[code] == !release)
        return true;
    held_keys[code] = !release;
    sync_buttons(m);
    return true;
}

void ui_buttons_set_pointer(machine_t *m, uint8_t set1, uint8_t set2)
{
    pointer_set1 = set1;
    pointer_set2 = set2;
    sync_buttons(m);
}

void ui_buttons_release_all(machine_t *m)
{
    memset(held_keys, 0, sizeof(held_keys));
    pointer_set1 = pointer_set2 = 0;
    sync_buttons(m);
}
#endif
