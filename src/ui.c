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
#include <stdlib.h>
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
static bool sdl_video_initialized = false;
static bool sdl_audio_initialized = false;
static SDL_AudioDeviceID audio_device = 0;

/* Frame rate limiting */
#define FRAME_INTERVAL_MS 33  /* ~30 fps */
#define TOUCH_MIN_DWELL_DEFAULT_MS 120
#define TOUCH_MIN_DWELL_MAX_MS 5000

/* Mouse button held state for touch drag tracking */
static bool mouse_button_held = false;
static bool touch_active = false;
static bool touch_release_pending = false;
static uint16_t touch_release_x = 0;
static uint16_t touch_release_y = 0;
static uint32_t touch_down_tick = 0;
static uint32_t touch_release_due_tick = 0;
static uint32_t touch_min_dwell_ms = TOUCH_MIN_DWELL_DEFAULT_MS;

static uint32_t last_frame_tick = 0;

/* Staging buffer for visible rectangle extraction */
static uint16_t *staging_buf = NULL;

#define BUZZER_SAMPLE_RATE      44100
#define BUZZER_MAX_QUEUE_MS       250
#define BUZZER_RETRIGGER_GAP_MS   120
#define BUZZER_DEFAULT_HZ        1200
#define BUZZER_DEFAULT_MS          60
static uint32_t last_buzzer_tick = 0;

static bool ui_audio_env_enabled(void)
{
    const char *v = getenv("BE300_AUDIO");

    if (!v || !*v)
        return true;
    return strcmp(v, "0") != 0 &&
        strcmp(v, "false") != 0 &&
        strcmp(v, "FALSE") != 0 &&
        strcmp(v, "off") != 0 &&
        strcmp(v, "OFF") != 0;
}

static void ui_audio_init(void)
{
    SDL_AudioSpec want, have;

    if (!ui_audio_env_enabled())
        return;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[UI] SDL audio init failed: %s — continuing silent\n",
                SDL_GetError());
        return;
    }
    sdl_audio_initialized = true;

    memset(&want, 0, sizeof(want));
    want.freq = BUZZER_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;

    audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!audio_device) {
        fprintf(stderr,
            "[UI] SDL_OpenAudioDevice failed: %s — continuing silent\n",
            SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        sdl_audio_initialized = false;
        return;
    }

    SDL_PauseAudioDevice(audio_device, 0);
}

static void ui_audio_destroy(void)
{
    if (audio_device) {
        SDL_ClearQueuedAudio(audio_device);
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
    }
    if (sdl_audio_initialized) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        sdl_audio_initialized = false;
    }
}

static void ui_video_destroy(machine_t *m)
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
    if (sdl_video_initialized) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        sdl_video_initialized = false;
    }
}

static bool ui_resolve_framebuffer(machine_t *m)
{
    if (m->fb_data)
        return true;
    if (!m->gxe_machine || !m->gxe_machine->fb ||
        !m->gxe_machine->fb->framebuffer)
        return false;

    m->fb_data = m->gxe_machine->fb->framebuffer;
    return true;
}

static void ui_copy_visible_frame(machine_t *m, uint16_t *dst)
{
    const uint16_t *src = (const uint16_t *)m->fb_data;

    for (uint32_t y = 0; y < m->fb_height; y++) {
        memcpy(dst + y * m->fb_width,
               src + y * m->fb_stride,
               m->fb_width * sizeof(uint16_t));
    }
}

static void ui_put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)(v >> 8);
}

static void ui_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static bool ui_write_rgb565_bmp(const char *fname, const uint16_t *pixels,
    uint32_t width, uint32_t height)
{
    enum {
        BMP_FILE_HEADER_SIZE = 14,
        BMP_INFO_HEADER_SIZE = 40,
        BMP_MASK_BYTES = 12,
        BMP_OFF_BITS = BMP_FILE_HEADER_SIZE + BMP_INFO_HEADER_SIZE +
            BMP_MASK_BYTES
    };
    uint32_t row_bytes = ((width * 16u + 31u) / 32u) * 4u;
    uint32_t image_bytes = row_bytes * height;
    uint32_t file_bytes = BMP_OFF_BITS + image_bytes;
    uint8_t header[BMP_OFF_BITS];
    uint8_t pad[3] = { 0, 0, 0 };
    FILE *f;

    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    ui_put_le32(header + 2, file_bytes);
    ui_put_le32(header + 10, BMP_OFF_BITS);
    ui_put_le32(header + 14, BMP_INFO_HEADER_SIZE);
    ui_put_le32(header + 18, width);
    ui_put_le32(header + 22, height);
    ui_put_le16(header + 26, 1);
    ui_put_le16(header + 28, 16);
    ui_put_le32(header + 30, 3);  /* BI_BITFIELDS */
    ui_put_le32(header + 34, image_bytes);
    ui_put_le32(header + 54, 0xf800u);
    ui_put_le32(header + 58, 0x07e0u);
    ui_put_le32(header + 62, 0x001fu);

    f = fopen(fname, "wb");
    if (!f)
        return false;

    if (fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return false;
    }

    for (uint32_t y = height; y > 0; y--) {
        const uint16_t *row = pixels + (y - 1u) * width;
        for (uint32_t x = 0; x < width; x++) {
            uint8_t px[2];
            ui_put_le16(px, row[x]);
            if (fwrite(px, 1, sizeof(px), f) != sizeof(px)) {
                fclose(f);
                return false;
            }
        }
        if (row_bytes > width * sizeof(uint16_t)) {
            size_t n_pad = row_bytes - width * sizeof(uint16_t);
            if (fwrite(pad, 1, n_pad, f) != n_pad) {
                fclose(f);
                return false;
            }
        }
    }

    if (fclose(f) != 0)
        return false;

    return true;
}

static uint32_t ui_ms_from_env(const char *name, uint32_t default_ms,
    uint32_t max_ms)
{
    const char *v = getenv(name);
    char *end = NULL;
    unsigned long ms;

    if (!v || !*v)
        return default_ms;

    ms = strtoul(v, &end, 10);
    if (end == v || (end && *end != '\0') || ms > max_ms)
        return default_ms;

    return (uint32_t)ms;
}

static void ui_window_to_touch(machine_t *m, int win_x, int win_y,
    uint16_t *x_out, uint16_t *y_out)
{
    int win_w, win_h;
    int tx, ty;

    win_w = 0;
    win_h = 0;
    if (m->sdl_window)
        SDL_GetWindowSize((SDL_Window *)m->sdl_window, &win_w, &win_h);

    if (win_w <= 0)
        win_w = (int)m->fb_width;
    if (win_h <= 0)
        win_h = (int)m->fb_height;

    tx = (int)(((int64_t)win_x * (int64_t)m->fb_width) / win_w);
    ty = (int)(((int64_t)win_y * (int64_t)m->fb_height) / win_h);
    if (tx < 0) tx = 0;
    if (ty < 0) ty = 0;
    if ((uint32_t)tx >= m->fb_width)  tx = (int)m->fb_width  - 1;
    if ((uint32_t)ty >= m->fb_height) ty = (int)m->fb_height - 1;

    *x_out = (uint16_t)tx;
    *y_out = (uint16_t)ty;
}

static bool ui_tick_reached(uint32_t now, uint32_t due)
{
    return (int32_t)(now - due) >= 0;
}

static uint32_t ui_later_tick(uint32_t a, uint32_t b)
{
    return ui_tick_reached(a, b) ? a : b;
}

static void ui_latch_touch_position(machine_t *m, uint16_t tx, uint16_t ty)
{
    touch_release_x = tx;
    touch_release_y = ty;

    if (touch_active && m->touch_down) {
        m->touch_x = tx;
        m->touch_y = ty;
        __sync_synchronize();
    }
}

static void ui_coalesce_touch_position(machine_t *m, int win_x, int win_y)
{
    uint16_t tx, ty;

    ui_window_to_touch(m, win_x, win_y, &tx, &ty);
    ui_latch_touch_position(m, tx, ty);
}

static void ui_begin_touch(machine_t *m, int win_x, int win_y,
    uint32_t now)
{
    uint16_t tx, ty;

    ui_window_to_touch(m, win_x, win_y, &tx, &ty);
    touch_active = true;
    touch_release_pending = false;
    touch_down_tick = now;
    touch_release_x = tx;
    touch_release_y = ty;
    be300_set_touch(m, true, tx, ty);
}

static void ui_release_touch(machine_t *m)
{
    be300_set_touch(m, false, touch_release_x, touch_release_y);
    touch_release_pending = false;
    touch_active = false;
}

static void ui_schedule_touch_release(machine_t *m, int win_x, int win_y,
    uint32_t now)
{
    uint32_t dwell_due = touch_down_tick + touch_min_dwell_ms;

    ui_coalesce_touch_position(m, win_x, win_y);
    touch_release_due_tick = ui_later_tick(dwell_due, now);

    if (!touch_active || ui_tick_reached(now, touch_release_due_tick)) {
        ui_release_touch(m);
    } else {
        touch_release_pending = true;
    }
}

int ui_init(machine_t *m)
{
    m->fb_width  = 240;
    m->fb_height = 320;
    m->fb_stride = 256;

    quit_requested = false;
    mouse_button_held = false;
    touch_active = false;
    touch_release_pending = false;
    touch_down_tick = 0;
    touch_release_due_tick = 0;
    touch_min_dwell_ms = ui_ms_from_env("BE300_TOUCH_MIN_DWELL_MS",
        TOUCH_MIN_DWELL_DEFAULT_MS, TOUCH_MIN_DWELL_MAX_MS);
    last_frame_tick = 0;
    last_buzzer_tick = 0;

    ui_audio_init();

    /* Skip SDL when no display server is available (headless / Docker) */
#ifndef __APPLE__
    if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
        fprintf(stderr, "[UI] No DISPLAY/WAYLAND_DISPLAY — running headless\n");
        return 0;
    }
#endif

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[UI] SDL_Init failed: %s — continuing headless\n",
                SDL_GetError());
        return 0;
    }
    sdl_video_initialized = true;

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
        ui_video_destroy(m);
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
        ui_video_destroy(m);
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
        ui_video_destroy(m);
        return 0;
    }
    m->sdl_texture = tex;

    /* Allocate staging buffer for visible rectangle */
    staging_buf = malloc(m->fb_width * m->fb_height * sizeof(uint16_t));
    if (!staging_buf) {
        fprintf(stderr, "[UI] staging buffer alloc failed — continuing headless\n");
        ui_video_destroy(m);
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
        ui_release_touch(m);
    }

    /* Poll input every emulator batch; only rendering is frame-limited. */
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
                uint32_t ev_tick = ev.button.timestamp ?
                    ev.button.timestamp : now;

                mouse_button_held = true;
                if (touch_active) {
                    ui_coalesce_touch_position(m, ev.button.x, ev.button.y);
                    touch_release_pending = false;
                } else {
                    ui_begin_touch(m, ev.button.x, ev.button.y, ev_tick);
                }
                SDL_CaptureMouse(SDL_TRUE);
            }
            break;

        case SDL_MOUSEBUTTONUP:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                uint32_t ev_tick = ev.button.timestamp ?
                    ev.button.timestamp : now;

                mouse_button_held = false;
                if (touch_active) {
                    ui_schedule_touch_release(m, ev.button.x, ev.button.y,
                        ev_tick);
                }
                SDL_CaptureMouse(SDL_FALSE);
            }
            break;

        case SDL_MOUSEMOTION:
            if (mouse_button_held) {
                if (touch_active) {
                    ui_coalesce_touch_position(m, ev.motion.x, ev.motion.y);
                }
            }
            break;

        default:
            break;
        }
    }

    if (now - last_frame_tick < FRAME_INTERVAL_MS)
        return;
    last_frame_tick = now;

    if (!ui_resolve_framebuffer(m))
        return;  /* dev_fb not yet initialized */

    ui_copy_visible_frame(m, staging_buf);

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
    uint16_t *pixels = staging_buf;
    uint16_t *scratch = NULL;

    if (!ui_resolve_framebuffer(m)) {
        fprintf(stderr, "[UI] No valid frame — cannot save screenshot\n");
        return;
    }

    if (!pixels) {
        scratch = malloc(m->fb_width * m->fb_height * sizeof(uint16_t));
        if (!scratch) {
            fprintf(stderr, "[UI] Screenshot buffer alloc failed\n");
            return;
        }
        pixels = scratch;
    }

    ui_copy_visible_frame(m, pixels);

    /* Generate timestamped filename */
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char fname[64];
    snprintf(fname, sizeof(fname),
             "screenshot_%04d%02d%02d_%02d%02d%02d.bmp",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);

    if (ui_write_rgb565_bmp(fname, pixels, m->fb_width, m->fb_height)) {
        fprintf(stderr, "[UI] Screenshot saved: %s\n", fname);
    } else {
        fprintf(stderr, "[UI] Screenshot save failed: %s\n", fname);
    }

    free(scratch);
}

void ui_destroy(machine_t *m)
{
    ui_video_destroy(m);
    ui_audio_destroy();
    if (SDL_WasInit(0) == 0)
        SDL_Quit();
}

void ui_buzzer_pulse(uint32_t frequency_hz, uint32_t duration_ms)
{
    uint32_t queued_max;
    uint32_t now;
    uint32_t period;
    uint32_t samples;
    int16_t *pcm;

    if (!audio_device)
        return;

    now = SDL_GetTicks();
    if (SDL_GetQueuedAudioSize(audio_device) > 0 ||
        (last_buzzer_tick != 0 &&
         !ui_tick_reached(now, last_buzzer_tick + BUZZER_RETRIGGER_GAP_MS)))
        return;
    last_buzzer_tick = now;

    if (frequency_hz == 0)
        frequency_hz = BUZZER_DEFAULT_HZ;
    if (duration_ms == 0)
        duration_ms = BUZZER_DEFAULT_MS;

    if (frequency_hz < 80)
        frequency_hz = 80;
    if (frequency_hz > 6000)
        frequency_hz = 6000;
    if (duration_ms > 500)
        duration_ms = 500;

    samples = (BUZZER_SAMPLE_RATE * duration_ms) / 1000u;
    if (samples == 0)
        return;

    pcm = malloc((size_t)samples * sizeof(*pcm));
    if (!pcm)
        return;

    period = BUZZER_SAMPLE_RATE / frequency_hz;
    if (period < 2)
        period = 2;

    for (uint32_t i = 0; i < samples; i++) {
        uint32_t edge = (i * 12u) / samples;
        int32_t amp = 6500;

        if (edge < 12u)
            amp = (amp * (int32_t)edge) / 12;
        if (i > samples - (samples / 8u + 1u)) {
            uint32_t rem = samples - i;
            uint32_t tail = samples / 8u + 1u;
            amp = (amp * (int32_t)rem) / (int32_t)tail;
        }

        pcm[i] = ((i % period) < (period / 2u))
            ? (int16_t)amp
            : (int16_t)-amp;
    }

    queued_max = (BUZZER_SAMPLE_RATE * sizeof(int16_t) *
        BUZZER_MAX_QUEUE_MS) / 1000u;
    if (SDL_GetQueuedAudioSize(audio_device) > queued_max)
        SDL_ClearQueuedAudio(audio_device);

    SDL_QueueAudio(audio_device, pcm, samples * sizeof(*pcm));
    free(pcm);
}

#else  /* !HAVE_SDL2 — headless stubs */

int  ui_init(machine_t *m)         { (void)m; return 0; }
void ui_update(machine_t *m)       { (void)m; }
void ui_destroy(machine_t *m)      { (void)m; }
bool ui_should_quit(machine_t *m)  { (void)m; return false; }
void ui_save_screenshot(machine_t *m) { (void)m; }
void ui_buzzer_pulse(uint32_t frequency_hz, uint32_t duration_ms)
    { (void)frequency_hz; (void)duration_ms; }

#endif  /* HAVE_SDL2 */

void ui_set_frame_callback(ui_frame_callback_t cb, void *user_data)
{
    frame_cb = cb;
    frame_cb_data = user_data;
}
