/*
 *  ui.c — SDL2 display frontend for BE-300 framebuffer.
 *
 *  Reads the GXemul dev_fb backing buffer via memory_paddr_to_hostaddr()
 *  and blits it to an SDL2 window at 2× scale (~30 fps).
 *
 *  When HAVE_SDL2 is not defined, all functions are safe no-ops (headless).
 */

#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static ui_frame_callback_t frame_cb = NULL;
static void *frame_cb_data = NULL;

#ifdef HAVE_SDL2

#include <SDL.h>

/* GXemul headers for framebuffer access */
#include "machine.h"
#include "devices.h"

/* Quit flag */
static bool quit_requested = false;

/* Frame rate limiting */
static uint32_t last_frame_tick = 0;
#define FRAME_INTERVAL_MS 33  /* ~30 fps */

/* Staging buffer for visible rectangle extraction */
static uint16_t *staging_buf = NULL;

int ui_init(machine_t *m)
{
    m->fb_width  = 240;
    m->fb_height = 320;
    m->fb_stride = 256;

    quit_requested = false;
    last_frame_tick = 0;

    /* Skip SDL when no display server is available (headless / Docker) */
#ifndef __APPLE__
    if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
        fprintf(stderr, "[UI] No DISPLAY/WAYLAND_DISPLAY — running headless\n");
        return 0;
    }
#endif

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[UI] SDL_Init failed: %s — continuing headless\n",
                SDL_GetError());
        return 0;
    }

    /* 2× scaled window */
    int win_w = m->fb_width  * 2;
    int win_h = m->fb_height * 2;

    SDL_Window *win = SDL_CreateWindow(
        "BE-300 Emulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h, 0);
    if (!win) {
        fprintf(stderr, "[UI] SDL_CreateWindow failed: %s — continuing headless\n",
                SDL_GetError());
        SDL_Quit();
        return 0;
    }
    m->sdl_window = win;

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) {
        /* Fall back to software renderer */
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!ren) {
        fprintf(stderr, "[UI] SDL_CreateRenderer failed: %s — continuing headless\n",
                SDL_GetError());
        SDL_DestroyWindow(win);
        m->sdl_window = NULL;
        SDL_Quit();
        return 0;
    }
    m->sdl_renderer = ren;

    /* BE-300 hardware uses RGB565 (R=15:11, G=10:5, B=4:0) */
    Uint32 fmt = SDL_PIXELFORMAT_RGB565;

    SDL_Texture *tex = SDL_CreateTexture(ren, fmt,
        SDL_TEXTUREACCESS_STREAMING,
        m->fb_width, m->fb_height);
    if (!tex) {
        fprintf(stderr, "[UI] SDL_CreateTexture failed: %s — continuing headless\n",
                SDL_GetError());
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        m->sdl_renderer = NULL;
        m->sdl_window = NULL;
        SDL_Quit();
        return 0;
    }
    m->sdl_texture = tex;

    /* Allocate staging buffer for visible rectangle */
    staging_buf = malloc(m->fb_width * m->fb_height * sizeof(uint16_t));
    if (!staging_buf) {
        fprintf(stderr, "[UI] staging buffer alloc failed — continuing headless\n");
        SDL_DestroyTexture(tex);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        m->sdl_texture = NULL;
        m->sdl_renderer = NULL;
        m->sdl_window = NULL;
        SDL_Quit();
        return 0;
    }

    fprintf(stderr, "[UI] SDL2 display: %ux%u @ 2x scale (RGB565)\n",
            m->fb_width, m->fb_height);
    return 0;
}

void ui_update(machine_t *m)
{
    if (!m->sdl_window)
        return;

    /* Rate-limit to ~30 fps */
    uint32_t now = SDL_GetTicks();
    if (now - last_frame_tick < FRAME_INTERVAL_MS)
        return;
    last_frame_tick = now;

    /* Poll events */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            quit_requested = true;
            return;
        case SDL_KEYDOWN:
            switch (ev.key.keysym.sym) {
            case SDLK_q:
                quit_requested = true;
                return;
            case SDLK_s:
                ui_save_screenshot(m);
                break;
            case SDLK_m:
                fprintf(stderr,
                    "[UI] Keys: Q=quit  S=screenshot  M=this help\n");
                break;
            default:
                break;
            }
            break;
        default:
            break;
        }
    }

    /* Lazy-resolve framebuffer host pointer from GXemul's vfb_data */
    if (!m->fb_data) {
        if (!m->gxe_machine->fb || !m->gxe_machine->fb->framebuffer)
            return;  /* dev_fb not yet initialized */
        m->fb_data = m->gxe_machine->fb->framebuffer;
        fprintf(stderr, "[UI] Framebuffer host pointer resolved (%ux%u, %d bpp)\n",
                m->gxe_machine->fb->visible_xsize,
                m->gxe_machine->fb->visible_ysize,
                m->gxe_machine->fb->bit_depth);
    }

    const uint16_t *src = (const uint16_t *)m->fb_data;

    /* Copy visible rectangle from stride-256 buffer (RGB565, no conversion) */
    for (uint32_t y = 0; y < m->fb_height; y++) {
        memcpy(staging_buf + y * m->fb_width,
               src + y * m->fb_stride,
               m->fb_width * sizeof(uint16_t));
    }

    /* Upload to texture and render */
    SDL_UpdateTexture((SDL_Texture *)m->sdl_texture, NULL,
                      staging_buf, m->fb_width * sizeof(uint16_t));
    SDL_RenderClear((SDL_Renderer *)m->sdl_renderer);
    SDL_RenderCopy((SDL_Renderer *)m->sdl_renderer,
                   (SDL_Texture *)m->sdl_texture, NULL, NULL);
    SDL_RenderPresent((SDL_Renderer *)m->sdl_renderer);

    /* Invoke frame callback if set */
    if (frame_cb) {
        frame_cb(staging_buf, m->fb_width, m->fb_height,
                 m->fb_width, frame_cb_data);
    }
}

bool ui_should_quit(machine_t *m)
{
    (void)m;
    return quit_requested;
}

void ui_save_screenshot(machine_t *m)
{
    if (!m->sdl_window || !staging_buf || !m->fb_data) {
        fprintf(stderr, "[UI] No valid frame — cannot save screenshot\n");
        return;
    }

    /* Create surface from staging buffer (RGB565) */
    Uint32 fmt = SDL_PIXELFORMAT_RGB565;

    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
        staging_buf,
        m->fb_width, m->fb_height,
        16, m->fb_width * sizeof(uint16_t),
        fmt);
    if (!surf) {
        fprintf(stderr, "[UI] SDL_CreateRGBSurfaceWithFormatFrom failed: %s\n",
                SDL_GetError());
        return;
    }

    /* Generate timestamped filename */
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char fname[64];
    snprintf(fname, sizeof(fname),
             "screenshot_%04d%02d%02d_%02d%02d%02d.bmp",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);

    if (SDL_SaveBMP(surf, fname) == 0) {
        fprintf(stderr, "[UI] Screenshot saved: %s\n", fname);
    } else {
        fprintf(stderr, "[UI] SDL_SaveBMP failed: %s\n", SDL_GetError());
    }

    SDL_FreeSurface(surf);
}

void ui_destroy(machine_t *m)
{
    if (m->sdl_texture) {
        SDL_DestroyTexture((SDL_Texture *)m->sdl_texture);
        m->sdl_texture = NULL;
    }
    if (m->sdl_renderer) {
        SDL_DestroyRenderer((SDL_Renderer *)m->sdl_renderer);
        m->sdl_renderer = NULL;
    }
    if (m->sdl_window) {
        SDL_DestroyWindow((SDL_Window *)m->sdl_window);
        m->sdl_window = NULL;
    }
    if (staging_buf) {
        free(staging_buf);
        staging_buf = NULL;
    }
    SDL_Quit();
}

#else  /* !HAVE_SDL2 — headless stubs */

int  ui_init(machine_t *m)         { (void)m; return 0; }
void ui_update(machine_t *m)       { (void)m; }
void ui_destroy(machine_t *m)      { (void)m; }
bool ui_should_quit(machine_t *m)  { (void)m; return false; }
void ui_save_screenshot(machine_t *m) { (void)m; }

#endif  /* HAVE_SDL2 */

void ui_set_frame_callback(ui_frame_callback_t cb, void *user_data)
{
    frame_cb = cb;
    frame_cb_data = user_data;
}
