#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "bus.h"

#ifdef HAVE_SDL2
#include <SDL2/SDL.h>
#endif

static ui_frame_callback_t g_frame_cb = NULL;
static void *g_frame_cb_user = NULL;
static uint16_t *g_guest_fb = NULL;
static uint16_t *g_linear_fb = NULL;

static void ui_set_fb_geometry(machine_t *m)
{
    m->fb_width = 240;
    m->fb_height = 320;
    /*
     * The BE-300 VRAM row pitch is 256 RGB565 pixels (512 bytes),
     * with only 240 visible pixels.
     */
    m->fb_stride = 256;
}

static bool ui_prepare_buffers(machine_t *m)
{
    if (!g_guest_fb) {
        g_guest_fb = malloc(PA_FRAMEBUFFER_SIZE);
        if (!g_guest_fb)
            return false;
    }

    size_t linear_pixels = (size_t)m->fb_width * (size_t)m->fb_height;
    if (!g_linear_fb) {
        g_linear_fb = malloc(linear_pixels * sizeof(uint16_t));
        if (!g_linear_fb)
            return false;
    }

    return true;
}

static void ui_emit_frame(machine_t *m)
{
    if (!g_frame_cb || !g_linear_fb)
        return;
    g_frame_cb(g_linear_fb, m->fb_width, m->fb_height, m->fb_width, g_frame_cb_user);
}

static bool ui_capture_framebuffer(machine_t *m)
{
    if (!ui_prepare_buffers(m))
        return false;

    /*
     * Probe where guest framebuffer writes land. The expected mapping is
     * kseg1 VA 0xAA200000 -> PA 0x0A200000.
     */
    static int fb_probe_frame = 0;
    static bool fb_probe_resolved = false;
    if (!fb_probe_resolved && (fb_probe_frame++ % 60) == 0) {
        static const struct {
            uint64_t addr;
            const char *label;
        } probes[] = {
            { 0x0A200000u,                              "PA  0x0A200000          " },
            { 0xAA200000u,                              "VA  0xAA200000 (kseg1)  " },
            { (uint64_t)(int64_t)(int32_t)0xAA200000u, "VA  0xFFFFFFFFAA200000  " },
        };

        for (int pi = 0; pi < 3; pi++) {
            uint32_t sample[4] = {0};
            uc_err re = uc_mem_read(m->uc, probes[pi].addr, sample, sizeof(sample));
            bool nonzero = (re == UC_ERR_OK) &&
                           (sample[0] || sample[1] || sample[2] || sample[3]);
            if (nonzero || fb_probe_frame <= 61) {
                fprintf(stderr,
                        "[FB_PROBE] %s read=%s %08X %08X %08X %08X%s\n",
                        probes[pi].label,
                        re == UC_ERR_OK ? "OK " : uc_strerror(re),
                        sample[0], sample[1], sample[2], sample[3],
                        nonzero ? " <-- DATA HERE" : "");
            }
            if (nonzero && pi == 0)
                fb_probe_resolved = true;
        }
    }

    if (uc_mem_read(m->uc, PA_FRAMEBUFFER_BASE, g_guest_fb, PA_FRAMEBUFFER_SIZE) != UC_ERR_OK)
        return false;

    if (m->cfg.sfb_5bit_green) {
        /*
         * Expand non-standard R5X1G5B5 green to RGB565 G6 by replicating
         * the green MSB into the new LSB.
         */
        for (uint32_t y = 0; y < m->fb_height; y++) {
            for (uint32_t x = 0; x < m->fb_width; x++) {
                uint16_t p = g_guest_fb[y * m->fb_stride + x];
                uint16_t g5 = (p >> 5) & 0x1Fu;
                uint16_t g6 = (uint16_t)((g5 << 1) | (g5 >> 4));
                g_linear_fb[y * m->fb_width + x] = (uint16_t)((p & 0xF81Fu) | (g6 << 5));
            }
        }
    } else {
        for (uint32_t y = 0; y < m->fb_height; y++) {
            memcpy(&g_linear_fb[y * m->fb_width],
                   &g_guest_fb[y * m->fb_stride],
                   (size_t)m->fb_width * sizeof(uint16_t));
        }
    }

    return true;
}

void ui_set_frame_callback(ui_frame_callback_t cb, void *user_data)
{
    g_frame_cb = cb;
    g_frame_cb_user = user_data;
}

#ifdef HAVE_SDL2

int ui_init(machine_t *m)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    ui_set_fb_geometry(m);

    m->sdl_window = SDL_CreateWindow("BE-300 Emulator",
                                      SDL_WINDOWPOS_CENTERED,
                                      SDL_WINDOWPOS_CENTERED,
                                      m->fb_width * 2,
                                      m->fb_height * 2,
                                      SDL_WINDOW_SHOWN);
    if (!m->sdl_window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }

    m->sdl_renderer = SDL_CreateRenderer(m->sdl_window,
                                         -1,
                                         SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!m->sdl_renderer)
        m->sdl_renderer = SDL_CreateRenderer(m->sdl_window, -1, SDL_RENDERER_SOFTWARE);
    if (!m->sdl_renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return -1;
    }

    m->sdl_texture = SDL_CreateTexture(m->sdl_renderer,
                                       SDL_PIXELFORMAT_RGB565,
                                       SDL_TEXTUREACCESS_STREAMING,
                                       m->fb_width,
                                       m->fb_height);
    if (!m->sdl_texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return -1;
    }

    return 0;
}

void ui_update(machine_t *m)
{
    if (!m->sdl_texture)
        return;

    if (!ui_capture_framebuffer(m))
        return;

    uint16_t *pixels = NULL;
    int pitch = 0;
    if (SDL_LockTexture(m->sdl_texture, NULL, (void **)&pixels, &pitch) == 0) {
        for (uint32_t y = 0; y < m->fb_height; y++) {
            memcpy(&pixels[y * ((uint32_t)pitch / 2u)],
                   &g_linear_fb[y * m->fb_width],
                   (size_t)m->fb_width * sizeof(uint16_t));
        }
        SDL_UnlockTexture(m->sdl_texture);
    }

    SDL_RenderClear(m->sdl_renderer);
    SDL_RenderCopy(m->sdl_renderer, m->sdl_texture, NULL, NULL);
    SDL_RenderPresent(m->sdl_renderer);

    ui_emit_frame(m);
}

bool ui_should_quit(machine_t *m)
{
    (void)m;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            return true;
    }
    return false;
}

void ui_destroy(machine_t *m)
{
    if (m->sdl_texture)
        SDL_DestroyTexture(m->sdl_texture);
    if (m->sdl_renderer)
        SDL_DestroyRenderer(m->sdl_renderer);
    if (m->sdl_window)
        SDL_DestroyWindow(m->sdl_window);
    m->sdl_texture = NULL;
    m->sdl_renderer = NULL;
    m->sdl_window = NULL;
    SDL_Quit();

    free(g_guest_fb);
    g_guest_fb = NULL;
    free(g_linear_fb);
    g_linear_fb = NULL;
}

#else /* !HAVE_SDL2 */

int ui_init(machine_t *m)
{
    ui_set_fb_geometry(m);
    return 0;
}

void ui_update(machine_t *m)
{
    if (!ui_capture_framebuffer(m))
        return;
    ui_emit_frame(m);
}

bool ui_should_quit(machine_t *m)
{
    (void)m;
    return false;
}

void ui_destroy(machine_t *m)
{
    (void)m;
    free(g_guest_fb);
    g_guest_fb = NULL;
    free(g_linear_fb);
    g_linear_fb = NULL;
}

#endif /* HAVE_SDL2 */
