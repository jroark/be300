/*
 *  src/ui_stowaway.c — SDL keysym → BE-300 Stowaway dock scancode mapping.
 *
 *  Pure lookup + per-key reference-count debounce. Calls into stowaway.c
 *  via stowaway_queue_key() to deliver press/release events.
 *
 *  Split out of src/ui.c. The mapping itself was inferred from
 *  Stowaway.dll RE — see project memory `project_stowaway_protocol_re.md`.
 */

#include "ui.h"

#ifdef HAVE_SDL2

#include <stdint.h>

#include <SDL.h>

#include "stowaway.h"
#include "ui_internal.h"

bool ui_stowaway_lookup_key(SDL_Keycode key, unsigned *scancode_out)
{
    switch (key) {
    case SDLK_1: *scancode_out = 0; return true;
    case SDLK_2: *scancode_out = 1; return true;
    case SDLK_3: *scancode_out = 2; return true;
    case SDLK_z: *scancode_out = 3; return true;
    case SDLK_4: *scancode_out = 4; return true;
    case SDLK_5: *scancode_out = 5; return true;
    case SDLK_6: *scancode_out = 6; return true;
    case SDLK_7: *scancode_out = 7; return true;
    case SDLK_q: *scancode_out = 9; return true;
    case SDLK_w: *scancode_out = 10; return true;
    case SDLK_e: *scancode_out = 11; return true;
    case SDLK_r: *scancode_out = 12; return true;
    case SDLK_t: *scancode_out = 13; return true;
    case SDLK_y: *scancode_out = 14; return true;
    case SDLK_BACKQUOTE: *scancode_out = 15; return true;
    case SDLK_x: *scancode_out = 16; return true;
    case SDLK_a: *scancode_out = 17; return true;
    case SDLK_s: *scancode_out = 18; return true;
    case SDLK_d: *scancode_out = 19; return true;
    case SDLK_f: *scancode_out = 20; return true;
    case SDLK_g: *scancode_out = 21; return true;
    case SDLK_h: *scancode_out = 22; return true;
    case SDLK_SPACE: *scancode_out = 23; return true;
    case SDLK_CAPSLOCK: *scancode_out = 24; return true;
    case SDLK_TAB: *scancode_out = 25; return true;
    case SDLK_LCTRL:
    case SDLK_RCTRL: *scancode_out = 26; return true;
    case SDLK_LALT:
    case SDLK_RALT: *scancode_out = 35; return true;
    case SDLK_c: *scancode_out = 44; return true;
    case SDLK_v: *scancode_out = 45; return true;
    case SDLK_b: *scancode_out = 46; return true;
    case SDLK_n: *scancode_out = 47; return true;
    case SDLK_MINUS: *scancode_out = 48; return true;
    case SDLK_EQUALS: *scancode_out = 49; return true;
    case SDLK_BACKSPACE: *scancode_out = 50; return true;
    case SDLK_HOME: *scancode_out = 51; return true;
    case SDLK_8: *scancode_out = 52; return true;
    case SDLK_9: *scancode_out = 53; return true;
    case SDLK_0: *scancode_out = 54; return true;
    case SDLK_ESCAPE: *scancode_out = 55; return true;
    case SDLK_LEFTBRACKET: *scancode_out = 56; return true;
    case SDLK_RIGHTBRACKET: *scancode_out = 57; return true;
    case SDLK_BACKSLASH: *scancode_out = 58; return true;
    case SDLK_END: *scancode_out = 59; return true;
    case SDLK_u: *scancode_out = 60; return true;
    case SDLK_i: *scancode_out = 61; return true;
    case SDLK_o: *scancode_out = 62; return true;
    case SDLK_p: *scancode_out = 63; return true;
    case SDLK_QUOTE: *scancode_out = 64; return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: *scancode_out = 65; return true;
    case SDLK_PAGEUP: *scancode_out = 66; return true;
    case SDLK_j: *scancode_out = 68; return true;
    case SDLK_k: *scancode_out = 69; return true;
    case SDLK_l: *scancode_out = 70; return true;
    case SDLK_SEMICOLON: *scancode_out = 71; return true;
    case SDLK_SLASH: *scancode_out = 72; return true;
    case SDLK_UP: *scancode_out = 73; return true;
    case SDLK_PAGEDOWN: *scancode_out = 74; return true;
    case SDLK_m: *scancode_out = 76; return true;
    case SDLK_COMMA: *scancode_out = 77; return true;
    case SDLK_PERIOD: *scancode_out = 78; return true;
    case SDLK_INSERT: *scancode_out = 79; return true;
    case SDLK_DELETE: *scancode_out = 80; return true;
    case SDLK_LEFT: *scancode_out = 81; return true;
    case SDLK_DOWN: *scancode_out = 82; return true;
    case SDLK_RIGHT: *scancode_out = 83; return true;
    case SDLK_LSHIFT: *scancode_out = 87; return true;
    case SDLK_RSHIFT: *scancode_out = 88; return true;
    case SDLK_F1: *scancode_out = 105; return true;
    case SDLK_F2: *scancode_out = 106; return true;
    case SDLK_F3: *scancode_out = 107; return true;
    case SDLK_F4: *scancode_out = 108; return true;
    case SDLK_F5: *scancode_out = 109; return true;
    case SDLK_F6: *scancode_out = 110; return true;
    case SDLK_F7: *scancode_out = 111; return true;
    case SDLK_F8: *scancode_out = 112; return true;
    case SDLK_F9: *scancode_out = 113; return true;
    case SDLK_F10: *scancode_out = 114; return true;
    case SDLK_F11: *scancode_out = 115; return true;
    case SDLK_F12: *scancode_out = 116; return true;
    default:
        return false;
    }
}

bool ui_stowaway_key_event(machine_t *m, const SDL_KeyboardEvent *key,
    bool release)
{
    static uint8_t key_refs[128];
    unsigned scancode;

    if (!m->cfg.enable_stowaway_keyboard)
        return false;
    if (!ui_stowaway_lookup_key(key->keysym.sym, &scancode))
        return false;

    if (release) {
        if (key_refs[scancode] == 0)
            return false;
        key_refs[scancode]--;
        if (key_refs[scancode] == 0)
            stowaway_queue_key(scancode, true);
        return true;
    }

    if (key->repeat)
        return true;

    if (key_refs[scancode] == 0)
        stowaway_queue_key(scancode, false);
    if (key_refs[scancode] != UINT8_MAX)
        key_refs[scancode]++;
    return true;
}

#endif /* HAVE_SDL2 */
