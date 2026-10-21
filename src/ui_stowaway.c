/*
 *  src/ui_stowaway.c — SDL physical key → BE-300 Stowaway dock scancode.
 *
 *  Lookup in the shared src/stowaway_keys.def table (decoded from
 *  Stowaway.dll's own layouts) + per-key reference-count debounce. Calls
 *  into stowaway.c via stowaway_queue_mapped_key() to deliver press/release
 *  events, with Fn chords for keys the dock only has on its Fn layer.
 */

#include "ui.h"

#ifdef HAVE_SDL2

#include <stdint.h>

#include <SDL.h>

#include "stowaway.h"
#include "ui_internal.h"

bool ui_stowaway_lookup_key(SDL_Scancode key, unsigned *value_out)
{
    switch (key) {
#define STOWAWAY_KEY(code, sdl, android, value) \
    case SDL_SCANCODE_##sdl: *value_out = (value); return true;
#include "stowaway_keys.def"
#undef STOWAWAY_KEY
    default:
        return false;
    }
}

bool ui_stowaway_key_event(machine_t *m, const SDL_KeyboardEvent *key,
    bool release)
{
    static uint8_t key_refs[2u * STOWAWAY_FN];
    unsigned value;

    if (!m->cfg.enable_stowaway_keyboard)
        return false;
    if (!ui_stowaway_lookup_key(key->keysym.scancode, &value))
        return false;

    if (release) {
        if (key_refs[value] == 0)
            return false;
        key_refs[value]--;
        if (key_refs[value] == 0)
            stowaway_queue_mapped_key(value, true);
        return true;
    }

    if (key->repeat)
        return true;

    if (key_refs[value] == 0)
        stowaway_queue_mapped_key(value, false);
    if (key_refs[value] != UINT8_MAX)
        key_refs[value]++;
    return true;
}

#endif /* HAVE_SDL2 */
