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
     * The BE-300 video memory starts at 0xAA200000 (PA 0x0A200000).
     * Each row occupies 512 bytes (256 pixels) even though only 240 pixels
     * (480 bytes) are visible — the remaining 16 pixels are padding.
     * stride=256 means guest_fb[y*256 + x] for x=0..239 indexes the correct
     * pixel at row y without drifting into adjacent rows.
     */
    m->fb_stride = 256;

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

    /*
     * Probe the framebuffer location once on first update.
     * The sfb driver uses kseg1 VA 0xAA200000 (= PA 0x0A200000) and does a
     * memset at sfb_init time, so at least one of these addresses should hold
     * the 0xE0E0 fill value if the kernel wrote to the expected region.
     */
    /*
     * Diagnose where the kernel's sfb_init memset actually landed.
     * sfb.c writes 0xE0 bytes to kseg1 VA 0xAA200000 (= PA 0x0A200000).
     * Probe three candidate addresses every 60 frames until non-zero data is
     * found at one of them, then log which address "won" and stop probing.
     */
    static int fb_probe_frame = 0;
    static bool fb_probe_resolved = false;
    if (!fb_probe_resolved && (fb_probe_frame++ % 60) == 0) {
        static const struct { uint64_t addr; const char *label; } probes[] = {
            { 0x0A200000u,                             "PA  0x0A200000          " },
            { 0xAA200000u,                             "VA  0xAA200000 (kseg1)  " },
            { (uint64_t)(int64_t)(int32_t)0xAA200000u, "VA  0xFFFFFFFFAA200000  " },
        };
        for (int pi = 0; pi < 3; pi++) {
            uint32_t sample[4] = {0};
            uc_err re = uc_mem_read(m->uc, probes[pi].addr, sample, sizeof(sample));
            bool nonzero = (re == UC_ERR_OK) &&
                           (sample[0] || sample[1] || sample[2] || sample[3]);
            if (nonzero || fb_probe_frame <= 61) {
                fprintf(stderr, "[FB_PROBE] %s read=%s %08X %08X %08X %08X%s\n",
                        probes[pi].label,
                        re == UC_ERR_OK ? "OK " : uc_strerror(re),
                        sample[0], sample[1], sample[2], sample[3],
                        nonzero ? " <-- DATA HERE" : "");
            }
            if (nonzero && pi == 0) fb_probe_resolved = true; /* PA matches — as expected */
        }
    }

    if (uc_mem_read(m->uc, PA_FRAMEBUFFER_BASE, guest_fb, PA_FRAMEBUFFER_SIZE) != UC_ERR_OK)
        return;

    uint16_t *pixels;
    int pitch;
    if (SDL_LockTexture(m->sdl_texture, NULL, (void**)&pixels, &pitch) == 0) {
        uint32_t stride = m->fb_stride;   /* pixels per row (16-bit units) */
        for (uint32_t y = 0; y < m->fb_height; y++) {
            for (uint32_t x = 0; x < m->fb_width; x++) {
                uint16_t p = guest_fb[y * stride + x];
                /*
                 * The sfb driver declares green as 5 bits at offset 5,
                 * so the pixel layout is R[15:11] X[10] G[9:5] B[4:0].
                 * SDL_PIXELFORMAT_RGB565 expects R[15:11] G[10:5] B[4:0]
                 * (6-bit green).  Expand the 5-bit green to 6 bits by
                 * replicating its MSB into the new LSB position.
                 */
                uint16_t g5 = (p >> 5) & 0x1Fu;
                uint16_t g6 = (g5 << 1) | (g5 >> 4);
                pixels[y * (pitch / 2) + x] = (p & 0xF81Fu) | (uint16_t)(g6 << 5);
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
