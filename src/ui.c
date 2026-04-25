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

/* Mouse button held state for touch drag tracking */
static bool mouse_button_held = false;
static bool touch_release_pending = false;
static uint16_t touch_release_x = 0;
static uint16_t touch_release_y = 0;
static uint32_t touch_release_due_tick = 0;

/* Frame rate limiting */
static uint32_t last_frame_tick = 0;
#define FRAME_INTERVAL_MS 33  /* ~30 fps */

/* Staging buffer for visible rectangle extraction */
static uint16_t *staging_buf = NULL;

static void ui_window_to_touch(machine_t *m, int win_x, int win_y,
    uint16_t *x_out, uint16_t *y_out)
{
    int tx, ty;

    /* SDL window is 2x scaled; divide to get screen pixel coords. */
    tx = win_x / 2;
    ty = win_y / 2;
    if (tx < 0) tx = 0;
    if (ty < 0) ty = 0;
    if ((uint32_t)tx >= m->fb_width)  tx = (int)m->fb_width  - 1;
    if ((uint32_t)ty >= m->fb_height) ty = (int)m->fb_height - 1;

    *x_out = (uint16_t)tx;
    *y_out = (uint16_t)ty;
}

static void ui_set_touch_from_window(machine_t *m, bool down, int win_x,
    int win_y)
{
    uint16_t tx, ty;

    ui_window_to_touch(m, win_x, win_y, &tx, &ty);
    be300_set_touch(m, down, tx, ty);
}

static bool ui_tick_reached(uint32_t now, uint32_t due)
{
    return (int32_t)(now - due) >= 0;
}

int ui_init(machine_t *m)
{
    m->fb_width  = 240;
    m->fb_height = 320;
    m->fb_stride = 256;

    quit_requested = false;
    mouse_button_held = false;
    touch_release_pending = false;
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

    return 0;
}

void ui_update(machine_t *m)
{
    if (!m->sdl_window)
        return;

    /* Rate-limit to ~30 fps */
    uint32_t now = SDL_GetTicks();
    if (touch_release_pending &&
        ui_tick_reached(now, touch_release_due_tick)) {
        be300_set_touch(m, false, touch_release_x, touch_release_y);
        touch_release_pending = false;
    }

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
                    "[UI] Keys: Q=quit  S=screenshot  M=this help\n"
                    "[UI]       Arrows=d-pad  Enter=ok(Enter)  Tab=esc(Tab)\n"
                    "[UI]       LShift/RShift=rocket modifier\n"
                    "[UI]       Mouse click/drag=touchpanel\n");
                break;
            /* D-pad */
            case SDLK_UP:     m->btn_set1 |= 0x10u; break;
            case SDLK_DOWN:   m->btn_set1 |= 0x20u; break;
            case SDLK_RIGHT:  m->btn_set1 |= 0x40u; break;
            case SDLK_LEFT:   m->btn_set1 |= 0x80u; break;
            /* Function buttons */
            case SDLK_RETURN: m->btn_set1 |= 0x04u; break;  /* ok/enter */
            case SDLK_TAB:    m->btn_set1 |= 0x08u; break;  /* esc/tab */
            /* Rocket modifier (either shift key) */
            case SDLK_LSHIFT:
            case SDLK_RSHIFT: m->btn_set2 |= 0x10u; break;
            /* Power/reboot (btn_set2 0x80) intentionally not mapped */
            default:
                break;
            }
            break;

        case SDL_KEYUP:
            switch (ev.key.keysym.sym) {
            case SDLK_UP:     m->btn_set1 &= ~0x10u; break;
            case SDLK_DOWN:   m->btn_set1 &= ~0x20u; break;
            case SDLK_RIGHT:  m->btn_set1 &= ~0x40u; break;
            case SDLK_LEFT:   m->btn_set1 &= ~0x80u; break;
            case SDLK_RETURN: m->btn_set1 &= ~0x04u; break;
            case SDLK_TAB:    m->btn_set1 &= ~0x08u; break;
            case SDLK_LSHIFT:
            case SDLK_RSHIFT: m->btn_set2 &= ~0x10u; break;
            default:
                break;
            }
            break;

        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                mouse_button_held = true;
                touch_release_pending = false;
                ui_set_touch_from_window(m, true, ev.button.x, ev.button.y);
            }
            break;

        case SDL_MOUSEBUTTONUP:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                mouse_button_held = false;
                ui_window_to_touch(m, ev.button.x, ev.button.y,
                    &touch_release_x, &touch_release_y);
                touch_release_due_tick = now + FRAME_INTERVAL_MS;
                touch_release_pending = true;
            }
            break;

        case SDL_MOUSEMOTION:
            if (mouse_button_held) {
                ui_set_touch_from_window(m, true, ev.motion.x, ev.motion.y);
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
    }

    const uint16_t *src = (const uint16_t *)m->fb_data;

    /* Copy visible rectangle from stride-256 buffer into staging (RGB565). */
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
