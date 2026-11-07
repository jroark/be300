/*
 *  ui_platform.h — native desktop integration for the SDL frontend:
 *  menu bars, window chrome, clipboard, file dialogs and the bezel window's
 *  transparency. Implemented by src/ui_platform_macos.m (Cocoa),
 *  src/ui_platform_win32.c (Win32) and src/ui_platform_stub.c (others).
 *
 *  Menus and shortcuts post ui_cmd_t values (src/ui_commands.h); nothing
 *  here touches emulator state.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef HAVE_SDL2
#include <SDL.h>

/* Set SDL hints (DPI awareness and similar). Call before SDL video init. */
void ui_platform_sdl_hints(void);

/* Install the application menu bar. Safe to call more than once; menus
 * flagged UI_ITEM_EMULATOR follow ui_cmd_emulator_active(). */
void ui_platform_install_menus(void);

/* True when the native menu bar delivers its own key equivalents, so the
 * SDL key handler must not run the same shortcut a second time. */
bool ui_platform_menu_handles_shortcuts(void);

/* Attach native chrome to a window: title, menu bar (Windows), toolbar
 * (macOS frameless), dark title bar, aspect-ratio lock. `bezel` windows are
 * borderless and shaped. `aspect` is width / height of the content. */
void ui_platform_attach_window(SDL_Window *win, bool bezel,
    const char *title, const char *subtitle, double aspect);
void ui_platform_detach_window(SDL_Window *win);

/* Update the aspect-ratio lock after the content changes shape. */
void ui_platform_set_aspect(SDL_Window *win, double aspect);

/* Call after presenting frames; refreshes the bezel window's shadow. */
void ui_platform_frame_presented(SDL_Window *win, bool bezel);

/* Bezel window transparency. `mask` is mask_w x mask_h, one byte per
 * pixel, non-zero for opaque; it may be at backing (pixel) resolution. */
void ui_platform_make_window_transparent(SDL_Window *win);
void ui_platform_apply_window_mask(SDL_Window *win, const uint8_t *mask,
    int mask_w, int mask_h);

/* Pop up the right-click menu at window point (x, y). */
void ui_platform_context_menu(SDL_Window *win, int x, int y);

/* Native modal dialogs. */
void ui_platform_about(SDL_Window *win);
bool ui_platform_open_file(SDL_Window *win, const char *title,
    const char *extension, char *out, size_t cap);

/* Put an RGBA image (8 bits per channel, top-down) on the clipboard. */
bool ui_platform_copy_image(const uint8_t *rgba, int w, int h);

/* Directory for user screenshots (the Desktop). */
bool ui_platform_screenshot_dir(char *out, size_t cap);

#endif /* HAVE_SDL2 */
