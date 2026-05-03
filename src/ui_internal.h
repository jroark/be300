/*
 *  ui_internal.h — declarations shared between src/ui.c and the
 *  topic-focused files split out of it. Not included by anything outside
 *  src/ui*.c.
 */
#pragma once

#include "ui.h"

#ifdef HAVE_SDL2
#include <SDL.h>

/* Stowaway dock keyboard event helpers (definition in src/ui_stowaway.c). */
bool ui_stowaway_lookup_key(SDL_Keycode key, unsigned *scancode_out);
bool ui_stowaway_key_event(machine_t *m, const SDL_KeyboardEvent *key,
    bool release);

#endif /* HAVE_SDL2 */
