/*
 *  ppsh_term.h — minimal SDL2 console window for the PPSH debug shell.
 *
 *  When --ppsh is enabled and SDL2 is available, the emulator opens a
 *  second window that renders PPSH shell output and feeds keystrokes back
 *  to the guest. This keeps PPSH traffic off stdout/stderr, which remain
 *  the serial console and diagnostic log.
 *
 *  All functions tolerate a NULL handle, making it safe to treat the
 *  terminal as optional (headless / SDL init failure falls back to stdio).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct ppsh_term ppsh_term_t;

ppsh_term_t *ppsh_term_create(const char *title);
void         ppsh_term_destroy(ppsh_term_t *t);

/* Push PPSH-shell output bytes into the terminal's screen grid. */
void         ppsh_term_write(ppsh_term_t *t, const uint8_t *buf, size_t n);

/* Drain queued keystrokes from the terminal's input ring. */
size_t       ppsh_term_read(ppsh_term_t *t, uint8_t *buf, size_t cap);

/* Render the current grid to the window. Safe to call every UI tick. */
void         ppsh_term_render(ppsh_term_t *t);

/* SDL window ID, or 0 if none. Used by ui.c to route events by window. */
uint32_t     ppsh_term_window_id(ppsh_term_t *t);

/* Feed a single SDL event to the terminal (const SDL_Event *). Events for
 * other windows are ignored. Passed as void* so SDL headers stay out of
 * this header. */
void         ppsh_term_handle_sdl_event(ppsh_term_t *t, const void *sdl_event);
