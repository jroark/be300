#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "ui.h"
#include "bus.h"

#ifdef HAVE_SDL2
#include <SDL2/SDL.h>
#endif

static ui_frame_callback_t g_frame_cb = NULL;
static void *g_frame_cb_user = NULL;
static uint16_t *g_guest_fb = NULL;
static uint16_t *g_linear_fb = NULL;
static uint64_t g_fb_source_base = PA_FRAMEBUFFER_BASE;
static uint32_t g_fb_source_offset = 0;
static uint32_t g_fb_source_score = 0;

static uint32_t ui_score_fb_window(const uint16_t *fb,
                                   uint32_t start_px,
                                   uint32_t stride,
                                   uint32_t width,
                                   uint32_t height)
{
    uint32_t nonzero = 0;
    uint32_t transitions = 0;
    const uint32_t y_step = 12u;
    const uint32_t x_step = 8u;

    for (uint32_t y = 0; y < height; y += y_step) {
        const uint32_t row = start_px + y * stride;
        for (uint32_t x = 0; x < width; x += x_step) {
            uint16_t p = fb[row + x];
            if (p != 0u)
                nonzero++;
            if (x + x_step < width) {
                uint16_t q = fb[row + x + x_step];
                if (p != q)
                    transitions++;
            }
            if (y + y_step < height) {
                uint16_t q = fb[start_px + (y + y_step) * stride + x];
                if (p != q)
                    transitions++;
            }
        }
    }

    return nonzero * 4u + transitions;
}

static uint32_t ui_probe_fb_offset(machine_t *m, uint32_t *best_score_out)
{
    /*
     * Scan VRAM in 4KB steps for the most "active" 240x320 window.
     * Some kernels place the visible framebuffer at a non-zero offset.
     */
    const uint32_t frame_bytes = m->fb_height * m->fb_stride * sizeof(uint16_t);
    if (frame_bytes >= PA_FRAMEBUFFER_SIZE)
        return 0u;

    const uint32_t max_off = PA_FRAMEBUFFER_SIZE - frame_bytes;
    uint32_t best_off = 0u;
    uint32_t best_score = 0u;

    for (uint32_t off = 0; off <= max_off; off += 0x1000u) {
        uint32_t start_px = off / sizeof(uint16_t);
        uint32_t score = ui_score_fb_window(g_guest_fb,
                                            start_px,
                                            m->fb_stride,
                                            m->fb_width,
                                            m->fb_height);
        if (score > best_score) {
            best_score = score;
            best_off = off;
        }
    }

    if (best_score_out)
        *best_score_out = best_score;

    return best_off;
}

static bool ui_read_fb_base(machine_t *m, uint64_t base)
{
    uc_err fb_read_err = uc_mem_read(m->uc, base, g_guest_fb, PA_FRAMEBUFFER_SIZE);
    return fb_read_err == UC_ERR_OK;
}

static uint32_t ui_probe_fb_source(machine_t *m, uint64_t *best_base_out, uint32_t *best_score_out)
{
    static const uint64_t candidate_bases[] = {
        (uint64_t)PA_FRAMEBUFFER_BASE,     /* physical */
        UINT64_C(0x000000008A200000),      /* kseg0 zero-extended */
        UINT64_C(0x00000000AA200000),      /* kseg1 zero-extended */
        UINT64_C(0xFFFFFFFF8A200000),      /* kseg0 sign-extended */
        UINT64_C(0xFFFFFFFFAA200000),      /* kseg1 sign-extended */
    };

    uint64_t best_base = PA_FRAMEBUFFER_BASE;
    uint32_t best_off = 0u;
    uint32_t best_score = 0u;

    for (size_t i = 0; i < sizeof(candidate_bases) / sizeof(candidate_bases[0]); i++) {
        uint64_t base = candidate_bases[i];
        if (!ui_read_fb_base(m, base))
            continue;

        uint32_t score = 0u;
        uint32_t off = ui_probe_fb_offset(m, &score);
        if (score > best_score) {
            best_score = score;
            best_off = off;
            best_base = base;
        }
    }

    if (best_base_out)
        *best_base_out = best_base;
    if (best_score_out)
        *best_score_out = best_score;
    return best_off;
}

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

    static uint32_t fb_probe_frame = 0;
    if ((fb_probe_frame++ % 30u) == 0u) {
        static const uint32_t noise_floor = 24u;
        static const uint32_t score_switch_margin = 96u;
        uint64_t probe_base = g_fb_source_base;
        uint32_t best_score = 0u;

        uint32_t probe_off = 0u;
        if (m->cfg.sfb_5bit_green) {
            probe_off = ui_probe_fb_source(m, &probe_base, &best_score);
            if (best_score <= noise_floor) {
                probe_base = PA_FRAMEBUFFER_BASE;
                probe_off = 0u;
                best_score = 0u;
            }

            /* Avoid visible jitter from near-tied offsets. */
            if (probe_base == g_fb_source_base &&
                probe_off != g_fb_source_offset &&
                g_fb_source_score > 0u &&
                best_score <= g_fb_source_score + score_switch_margin) {
                probe_off = g_fb_source_offset;
                best_score = g_fb_source_score;
            }
        } else {
            /* 2.4 kernels are stable at base offset 0; avoid probe-induced wobble. */
            probe_base = PA_FRAMEBUFFER_BASE;
            probe_off = 0u;
            best_score = 0u;
        }

        static uint32_t probe_log_count = 0;
        if (probe_log_count < 64u) {
                fprintf(stderr,
                    "[FB_SCAN] base=0x%016" PRIX64 " best_off=0x%05X score=%u%s\n",
                    probe_base, probe_off, best_score, probe_off ? " (non-zero window)" : "");
            probe_log_count++;
        }

        if (probe_base != g_fb_source_base || probe_off != g_fb_source_offset ||
            best_score != g_fb_source_score) {
            static uint32_t relock_log_count = 0;
            if (relock_log_count < 64u) {
                fprintf(stderr,
                        "[FB_SCAN] source base 0x%016" PRIX64 " -> 0x%016" PRIX64
                        " off 0x%05X -> 0x%05X (score %u)\n",
                        g_fb_source_base, probe_base,
                        g_fb_source_offset, probe_off, best_score);
                relock_log_count++;
            }
            g_fb_source_base = probe_base;
            g_fb_source_offset = probe_off;
            g_fb_source_score = best_score;
        }
    }

    if (!ui_read_fb_base(m, g_fb_source_base)) {
        /*
         * Some Unicorn builds expose VRAM only through PA aliases.
         * Fall back to physical and clear source score if alias read fails.
         */
        g_fb_source_base = PA_FRAMEBUFFER_BASE;
        g_fb_source_offset = 0u;
        g_fb_source_score = 0u;
        if (!ui_read_fb_base(m, g_fb_source_base))
            return false;
    }

    uint32_t source_start_px = g_fb_source_offset / sizeof(uint16_t);

    if (m->cfg.sfb_5bit_green) {
        /*
         * Expand non-standard 2.6 layout:
         *   - Green is 5-bit at [9:5], expanded to G6 by MSB replication.
         */
        for (uint32_t y = 0; y < m->fb_height; y++) {
            for (uint32_t x = 0; x < m->fb_width; x++) {
                uint16_t p = g_guest_fb[source_start_px + y * m->fb_stride + x];
                uint16_t r5 = (p >> 11) & 0x1Fu;
                uint16_t g5 = (p >> 5) & 0x1Fu;
                uint16_t b5 = p & 0x1Fu;
                uint16_t g6 = (uint16_t)((g5 << 1) | (g5 >> 4));
                g_linear_fb[y * m->fb_width + x] =
                    (uint16_t)((r5 << 11) | (g6 << 5) | b5);
            }
        }
    } else {
        for (uint32_t y = 0; y < m->fb_height; y++) {
            memcpy(&g_linear_fb[y * m->fb_width],
                   &g_guest_fb[source_start_px + y * m->fb_stride],
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
    g_fb_source_base = PA_FRAMEBUFFER_BASE;
    g_fb_source_offset = 0;
    g_fb_source_score = 0;
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
    g_fb_source_base = PA_FRAMEBUFFER_BASE;
    g_fb_source_offset = 0;
    g_fb_source_score = 0;
}

#endif /* HAVE_SDL2 */
