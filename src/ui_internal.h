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
bool ui_stowaway_lookup_key(SDL_Scancode key, unsigned *value_out);
bool ui_stowaway_key_event(machine_t *m, const SDL_KeyboardEvent *key,
    bool release);
void ui_stowaway_release_keys(void);

/* Source ownership for host keyboard and framed pointer buttons. */
bool ui_button_key_event(machine_t *m, const SDL_KeyboardEvent *key, bool release);
void ui_buttons_set_pointer(machine_t *m, uint8_t set1, uint8_t set2);
void ui_buttons_release_all(machine_t *m);

/* SDL_GetTicks() comparison that survives the 49-day wrap. */
static inline bool ui_tick_reached(uint32_t now, uint32_t due)
{
    return (int32_t)(now - due) >= 0;
}

/* Host audio device (definitions in src/ui_audio.c). */
void ui_audio_init(void);
void ui_audio_destroy(void);

/* Frame window transparency and mask on macOS, no-ops elsewhere
 * (definitions in src/ui_macos.c). */
void ui_macos_make_window_transparent(SDL_Window *win);
void ui_macos_apply_window_mask(SDL_Window *win,
    const uint8_t *window_mask, int win_w, int win_h);

#endif /* HAVE_SDL2 */
