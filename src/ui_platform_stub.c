/*
 *  ui_platform_stub.c — ui_platform.h for hosts without a native backend
 *  (Linux/X11/Wayland). No menu bar; the keyboard shortcuts in
 *  src/ui_commands.c still work through the SDL key handler, and file
 *  drops arrive as SDL_DROPFILE.
 */
#include "ui_platform.h"

#ifdef HAVE_SDL2

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BE300_VERSION_STRING
#define BE300_VERSION_STRING "0.1.0"
#endif

void ui_platform_sdl_hints(void) {}
void ui_platform_install_menus(void) {}
bool ui_platform_menu_handles_shortcuts(void) { return false; }

void ui_platform_attach_window(SDL_Window *win, bool bezel,
    const char *title, const char *subtitle, double aspect)
{
    char buf[512];

    (void)bezel;
    (void)aspect;
    if (!win)
        return;
    if (subtitle && *subtitle)
        snprintf(buf, sizeof(buf), "%s - %s", title ? title : "BE-300",
            subtitle);
    else
        snprintf(buf, sizeof(buf), "%s", title ? title : "BE-300");
    SDL_SetWindowTitle(win, buf);
}

void ui_platform_detach_window(SDL_Window *win) { (void)win; }
void ui_platform_set_aspect(SDL_Window *win, double aspect)
{ (void)win; (void)aspect; }
void ui_platform_frame_presented(SDL_Window *win, bool bezel)
{ (void)win; (void)bezel; }
void ui_platform_make_window_transparent(SDL_Window *win) { (void)win; }
void ui_platform_apply_window_mask(SDL_Window *win, const uint8_t *mask,
    int mask_w, int mask_h)
{ (void)win; (void)mask; (void)mask_w; (void)mask_h; }
void ui_platform_context_menu(SDL_Window *win, int x, int y)
{ (void)win; (void)x; (void)y; }

void ui_platform_about(SDL_Window *win)
{
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "About BE-300 Emulator",
        "BE-300 Emulator " BE300_VERSION_STRING "\n\n"
        "Casio CASSIOPEIA BE-300 (NEC VR4131) emulator.\n"
        "Boots Windows CE 3.0 from the real boot ROM.", win);
}

bool ui_platform_open_file(SDL_Window *win, const char *title,
    const char *extension, char *out, size_t cap)
{
    (void)win; (void)title; (void)extension; (void)out; (void)cap;
    return false;
}

bool ui_platform_copy_image(const uint8_t *rgba, int w, int h)
{
    (void)rgba; (void)w; (void)h;
    return false;
}

bool ui_platform_screenshot_dir(char *out, size_t cap)
{
    const char *home = getenv("HOME");
    int n;

    if (!home || !*home)
        return false;
    n = snprintf(out, cap, "%s/Desktop", home);
    if (n <= 0 || (size_t)n >= cap)
        return false;
    {
        FILE *probe;
        char test[4096];
        snprintf(test, sizeof(test), "%s/.", out);
        probe = fopen(test, "r");
        if (probe) {
            fclose(probe);
            return true;
        }
    }
    n = snprintf(out, cap, "%s", home);
    return n > 0 && (size_t)n < cap;
}

#endif /* HAVE_SDL2 */
