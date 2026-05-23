#ifndef BE300_LAUNCHER_UI_IMGUI_BACKEND_H
#define BE300_LAUNCHER_UI_IMGUI_BACKEND_H

#include <stdbool.h>
#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wraps both halves of the ImGui SDL2 + SDL_Renderer integration so the
 * launcher core stays in C. cimgui exposes the platform half (ImGui_ImplSDL2_*)
 * already; the SDL_Renderer2 half is *not* in cimgui_impl.cpp, so the
 * launcher's small C++ shim wraps it here. */

bool launcher_imgui_init(SDL_Window *win, SDL_Renderer *ren);
void launcher_imgui_shutdown(void);
void launcher_imgui_process_event(const SDL_Event *ev);
void launcher_imgui_new_frame(void);
void launcher_imgui_render(SDL_Renderer *ren);

/* Apply the renderer's points→pixels scale so both SDL_RenderClear and
 * ImGui's vertex coords fill the full pixel framebuffer on HiDPI displays.
 * Call once per frame before SDL_RenderClear. */
void launcher_imgui_apply_hidpi_scale(SDL_Window *win, SDL_Renderer *ren);

#ifdef __cplusplus
}
#endif

#endif /* BE300_LAUNCHER_UI_IMGUI_BACKEND_H */
