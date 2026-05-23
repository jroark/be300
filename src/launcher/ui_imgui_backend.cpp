// Small C++ shim that bridges the launcher's C code to Dear ImGui's
// SDL2 + SDL_Renderer2 backends. cimgui already covers the SDL2 platform
// backend; the SDL_Renderer renderer backend is C++ only, so we wrap it
// here in five extern "C" functions consumed by launcher_main.c.

#include "ui_imgui_backend.h"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

extern "C" bool launcher_imgui_init(SDL_Window *win, SDL_Renderer *ren)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;          // do not write imgui.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL2_InitForSDLRenderer(win, ren)) return false;
    if (!ImGui_ImplSDLRenderer2_Init(ren)) {
        ImGui_ImplSDL2_Shutdown();
        return false;
    }
    return true;
}

extern "C" void launcher_imgui_shutdown(void)
{
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
}

extern "C" void launcher_imgui_process_event(const SDL_Event *ev)
{
    if (ev) ImGui_ImplSDL2_ProcessEvent(ev);
}

extern "C" void launcher_imgui_new_frame(void)
{
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

extern "C" void launcher_imgui_render(SDL_Renderer *ren)
{
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
}

/* Apply the points→pixels scale so SDL_RenderClear and SDL_RenderGeometry
 * (the latter via the SDLRenderer2 backend) both cover the full pixel
 * framebuffer on HiDPI displays. Call this once per frame before clearing.
 * Safe to call before ImGui_ImplSDL2_NewFrame because we just read the
 * underlying SDL state, not ImGui's DisplaySize. */
extern "C" void launcher_imgui_apply_hidpi_scale(SDL_Window *win, SDL_Renderer *ren)
{
    if (!win || !ren) return;
    int w, h, pw, ph;
    SDL_GetWindowSize(win, &w, &h);
    SDL_GetRendererOutputSize(ren, &pw, &ph);
    if (w > 0 && h > 0 && pw > 0 && ph > 0) {
        SDL_RenderSetScale(ren, (float)pw / (float)w, (float)ph / (float)h);
    }
}
