#include <stdio.h>
#include <stdlib.h>
#include "ui.h"
#include "bus.h"

#ifdef HAVE_SDL2
#include <SDL2/SDL.h>

int ui_init(machine_t *m)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    m->fb_width = 240;
    m->fb_height = 320;
    /*
     * fb_stride: pixels per row in the 16-bit RGB565 framebuffer.
     * The BE-300 display is 240 pixels wide with no row padding.
     * Previous value (256) treated the buffer as 32-bit words, causing
     * every other pixel to be read at double stride and wrapping to the
     * next row at x=120, producing a doubled side-by-side image.
     */
    m->fb_stride = 240;

    m->sdl_window = SDL_CreateWindow("BE-300 Emulator",
                                    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    m->fb_width * 2, m->fb_height * 2,
                                    SDL_WINDOW_SHOWN);
    if (!m->sdl_window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }

    m->sdl_renderer = SDL_CreateRenderer(m->sdl_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!m->sdl_renderer)
        m->sdl_renderer = SDL_CreateRenderer(m->sdl_window, -1, SDL_RENDERER_SOFTWARE);
    if (!m->sdl_renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return -1;
    }

    m->sdl_texture = SDL_CreateTexture(m->sdl_renderer,
                                      SDL_PIXELFORMAT_RGB565,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      m->fb_width, m->fb_height);
    if (!m->sdl_texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return -1;
    }

    return 0;
}

void ui_update(machine_t *m)
{
    if (!m->sdl_texture) return;

    /* Read as 16-bit RGB565 pixels matching the BE-300 framebuffer format. */
    static uint16_t *guest_fb = NULL;
    if (!guest_fb)
        guest_fb = malloc(PA_FRAMEBUFFER_SIZE);

    if (uc_mem_read(m->uc, PA_FRAMEBUFFER_BASE, guest_fb, PA_FRAMEBUFFER_SIZE) != UC_ERR_OK)
        return;

    uint16_t *pixels;
    int pitch;
    if (SDL_LockTexture(m->sdl_texture, NULL, (void**)&pixels, &pitch) == 0) {
        uint32_t stride = m->fb_stride;   /* pixels per row (16-bit units) */
        for (uint32_t y = 0; y < m->fb_height; y++) {
            for (uint32_t x = 0; x < m->fb_width; x++) {
                pixels[y * (pitch / 2) + x] = guest_fb[y * stride + x];
            }
        }
        SDL_UnlockTexture(m->sdl_texture);
    }

    SDL_RenderClear(m->sdl_renderer);
    SDL_RenderCopy(m->sdl_renderer, m->sdl_texture, NULL, NULL);
    SDL_RenderPresent(m->sdl_renderer);
}

bool ui_should_quit(machine_t *m)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            return true;
    }
    return false;
}

void ui_destroy(machine_t *m)
{
    if (m->sdl_texture)  SDL_DestroyTexture(m->sdl_texture);
    if (m->sdl_renderer) SDL_DestroyRenderer(m->sdl_renderer);
    if (m->sdl_window)   SDL_DestroyWindow(m->sdl_window);
    SDL_Quit();
}

#else /* !HAVE_SDL2 — headless stub */

int  ui_init(machine_t *m)       { (void)m; return 0; }
void ui_update(machine_t *m)     { (void)m; }
bool ui_should_quit(machine_t *m){ (void)m; return false; }
void ui_destroy(machine_t *m)    { (void)m; }

#endif /* HAVE_SDL2 */
