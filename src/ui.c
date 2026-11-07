/*
 *  ui.c — SDL2 desktop frontend for the BE-300 framebuffer.
 *
 *  Reads the GXemul dev_fb backing buffer and draws it in a native-feeling
 *  window: either the bare LCD with the touch panel's icon strip below it,
 *  or the BE-300 bezel artwork. The window is HiDPI-aware and resizable
 *  with a locked aspect ratio. Menus, the bezel's right-click menu and the
 *  keyboard shortcuts post ui_cmd_t commands (ui_commands.h) that
 *  ui_update() runs; native pieces live in ui_platform_*.
 *
 *  When HAVE_SDL2 is not defined, all functions are safe no-ops (headless).
 */

#include "ui.h"
#include "ui_internal.h"
#include "ui_button_mask.h"
#include "cbea_install.h"
#include "ui_commands.h"
#include "ui_layout.h"
#include "ui_platform.h"
#include "ui_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#ifdef HAVE_SDL2

#include <SDL.h>
#include <SDL_shape.h>

/* GXemul headers for framebuffer access */
#include "machine.h"
#include "devices.h"
#include "stowaway.h"

#define FRAME_INTERVAL_MS 33  /* ~30 fps */
#define TOUCH_MIN_DWELL_DEFAULT_MS 120
#define TOUCH_MIN_DWELL_MAX_MS 5000
#define BUTTON_MIN_DWELL_DEFAULT_MS 120
#define BUTTON_MIN_DWELL_MAX_MS 5000
#define RESAMPLE_DEBOUNCE_MS 120
#define UI_PATH_MAX 4096

static const ui_rectf_t fallback_frame_lcd_rect = { 187, 218, 644, 859 };

typedef enum {
    UI_POINTER_NONE = 0,
    UI_POINTER_TOUCH,
    UI_POINTER_BUTTON
} ui_pointer_mode_t;

/* Run state */
static bool quit_requested = false;
static bool sdl_video_initialized = false;
static bool restart_requested = false;
static char pending_package[UI_PATH_MAX];

/* Bezel artwork (trimmed RGBA), loaded once per ui_init. */
static uint8_t *art_pixels = NULL;
static uint32_t art_w = 0, art_h = 0;
static uint32_t frame_trim_x = 0, frame_trim_y = 0;
static ui_rectf_t art_lcd, art_icon;
static ui_button_mask_t button_mask;

/* Window */
static ui_layout_t layout;
static bool bezel_on = false;
static bool pixel_accurate = false;
static int win_w_pt = 0, win_h_pt = 0;    /* window size in points */
static int out_w_px = 0, out_h_px = 0;    /* backing size in pixels */
static SDL_Texture *frame_texture = NULL; /* bezel, resampled to backing size */
static SDL_Texture *strip_texture = NULL; /* frameless icon strip */
static int frame_tex_w = 0, frame_tex_h = 0;
static int strip_tex_w = 0, strip_tex_h = 0;
static bool art_textures_dirty = false;
static uint32_t art_textures_due = 0;
static bool shaped_window = false;
static char window_title[256];
static char window_subtitle[256];

/* Geometry carried across restarts in one session and across runs via the
 * preferences file. */
static struct {
    bool   valid;
    bool   have_pos;
    int    x, y;
    double zoom;
    bool   bezel;
    bool   pixel;
} saved_geom;
static bool prefs_loaded = false;

/* Screenshots requested from the menu/shortcut, taken at the next draw. */
static bool screenshot_pending = false;
static bool copy_pending = false;

/* Input */
static ui_pointer_mode_t pointer_mode = UI_POINTER_NONE;
static bool mouse_button_held = false;
static bool touch_active = false;
static bool touch_release_pending = false;
static uint16_t touch_release_x = 0;
static uint16_t touch_release_y = 0;
static uint32_t touch_down_tick = 0;
static uint32_t touch_release_due_tick = 0;
static uint32_t touch_min_dwell_ms = TOUCH_MIN_DWELL_DEFAULT_MS;
static bool button_release_pending = false;
static uint32_t button_down_tick = 0;
static uint32_t button_release_due_tick = 0;
static uint32_t button_min_dwell_ms = BUTTON_MIN_DWELL_DEFAULT_MS;

static uint32_t last_frame_tick = 0;

/* Staging buffer for visible rectangle extraction */
static uint16_t *staging_buf = NULL;

static void ui_free_art(void)
{
    ui_button_mask_free(&button_mask);
    free(art_pixels);
    art_pixels = NULL;
    art_w = art_h = 0;
    frame_trim_x = frame_trim_y = 0;
}

static void ui_destroy_textures(void)
{
    if (frame_texture) {
        SDL_DestroyTexture(frame_texture);
        frame_texture = NULL;
    }
    if (strip_texture) {
        SDL_DestroyTexture(strip_texture);
        strip_texture = NULL;
    }
    frame_tex_w = frame_tex_h = strip_tex_w = strip_tex_h = 0;
}

static void ui_close_window(machine_t *m)
{
    ui_destroy_textures();
    if (m->sdl_texture) {
        SDL_DestroyTexture((SDL_Texture *)m->sdl_texture);
        m->sdl_texture = NULL;
    }
    if (m->sdl_renderer) {
        SDL_DestroyRenderer((SDL_Renderer *)m->sdl_renderer);
        m->sdl_renderer = NULL;
    }
    if (m->sdl_window) {
        ui_platform_detach_window((SDL_Window *)m->sdl_window);
        SDL_DestroyWindow((SDL_Window *)m->sdl_window);
        m->sdl_window = NULL;
    }
    shaped_window = false;
}

static void ui_video_destroy(machine_t *m)
{
    ui_close_window(m);
    ui_free_art();
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

    lcd_copy_rgb565(&m->lcd, src, dst, m->fb_width, m->fb_height, m->fb_stride);
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

static void ui_trim_frame_transparent_edges(uint8_t **pixels_io,
    uint32_t *width_io, uint32_t *height_io)
{
    uint8_t *src = *pixels_io;
    uint32_t width = *width_io;
    uint32_t height = *height_io;
    uint32_t min_x = width, min_y = height, max_x = 0, max_y = 0;
    bool found = false;

    frame_trim_x = 0;
    frame_trim_y = 0;

    if (!src || width == 0 || height == 0)
        return;

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t alpha = src[((size_t)y * width + x) * 4u + 3u];
            if (alpha == 0)
                continue;
            if (x < min_x) min_x = x;
            if (y < min_y) min_y = y;
            if (x > max_x) max_x = x;
            if (y > max_y) max_y = y;
            found = true;
        }
    }

    if (!found || (min_x == 0 && min_y == 0 &&
        max_x + 1u == width && max_y + 1u == height))
        return;

    uint32_t new_w = max_x - min_x + 1u;
    uint32_t new_h = max_y - min_y + 1u;
    uint8_t *dst = malloc((size_t)new_w * new_h * 4u);
    if (!dst)
        return;

    for (uint32_t y = 0; y < new_h; y++) {
        memcpy(dst + (size_t)y * new_w * 4u,
               src + ((size_t)(min_y + y) * width + min_x) * 4u,
               (size_t)new_w * 4u);
    }

    free(src);
    frame_trim_x = min_x;
    frame_trim_y = min_y;
    *pixels_io = dst;
    *width_io = new_w;
    *height_io = new_h;
    fprintf(stderr,
        "[UI] Trimmed transparent frame margins: x=%u y=%u w=%u h=%u\n",
        min_x, min_y, new_w, new_h);
}

#ifdef HAVE_PNG
#include "frame_png_embedded.h"

static bool ui_load_frame_pixels(uint8_t **pixels_out, uint32_t *width_out,
    uint32_t *height_out)
{
    if (!ui_load_png_rgba(be300_frame_png, (size_t)be300_frame_png_len,
        pixels_out, width_out, height_out))
        return false;

    ui_trim_frame_transparent_edges(pixels_out, width_out, height_out);
    fprintf(stderr, "[UI] Loaded embedded frame asset (%u bytes)\n",
        (unsigned)be300_frame_png_len);
    return true;
}

static void ui_log_button_region(const char *name,
    const ui_button_region_t *region)
{
    if (!region->valid)
        return;
    fprintf(stderr,
        "[UI] Button mask %-6s area=%u bbox=%u,%u-%u,%u center=%.1f,%.1f\n",
        name, region->area, region->min_x, region->min_y,
        region->max_x, region->max_y, region->cx, region->cy);
}

static bool ui_configure_button_mask(const char *path, const uint8_t *rgba,
    uint32_t width, uint32_t height)
{
    if (!ui_button_mask_build(&button_mask, rgba, width, height))
        return false;

    fprintf(stderr, "[UI] Loaded button mask: %s (%ux%u)\n", path,
        width, height);
    ui_log_button_region("dpad", &button_mask.regions[UI_BUTTON_REGION_DPAD]);
    ui_log_button_region("rocket",
        &button_mask.regions[UI_BUTTON_REGION_ROCKET]);
    ui_log_button_region("power", &button_mask.regions[UI_BUTTON_REGION_POWER]);
    ui_log_button_region("ok", &button_mask.regions[UI_BUTTON_REGION_OK]);
    ui_log_button_region("esc", &button_mask.regions[UI_BUTTON_REGION_ESC]);
    return true;
}

#include "buttons_mask_png_embedded.h"

static bool ui_load_button_mask(void)
{
    uint8_t *pixels = NULL;
    uint32_t width = 0, height = 0;
    bool ok;

    if (!ui_load_png_rgba(be300_buttons_mask_png,
        (size_t)be300_buttons_mask_png_len, &pixels, &width, &height))
        return false;

    ok = ui_configure_button_mask("embedded:buttons_dpad_bw_mask.png",
        pixels, width, height);
    free(pixels);
    return ok;
}
#else
static bool ui_load_frame_pixels(uint8_t **pixels_out, uint32_t *width_out,
    uint32_t *height_out)
{
    (void)pixels_out;
    (void)width_out;
    (void)height_out;
    return false;
}

static bool ui_load_button_mask(void)
{
    return false;
}
#endif

/* Find the transparent LCD aperture in the artwork: the largest enclosed
 * transparent region with an LCD-like aspect ratio. */
static bool ui_detect_frame_lcd_rect(const uint8_t *pixels, uint32_t width,
    uint32_t height, ui_rectf_t *out)
{
    uint8_t *seen;
    uint32_t *queue;
    uint32_t best_area = 0;
    ui_rectf_t best = { 0, 0, 0, 0 };

    if (!pixels || width == 0 || height == 0)
        return false;

    seen = calloc((size_t)width * (size_t)height, 1);
    queue = malloc((size_t)width * (size_t)height * sizeof(*queue));
    if (!seen || !queue) {
        free(seen);
        free(queue);
        return false;
    }

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            size_t start = (size_t)y * (size_t)width + x;
            uint32_t head = 0, tail = 0, area = 0;
            uint32_t min_x = x, max_x = x, min_y = y, max_y = y;
            bool touches_edge = false;

            if (seen[start] || pixels[start * 4u + 3u] >= 16)
                continue;

            seen[start] = 1;
            queue[tail++] = (uint32_t)start;

            while (head < tail) {
                uint32_t p = queue[head++];
                uint32_t px = p % width;
                uint32_t py = p / width;
                uint32_t neighbors[4];

                area++;
                if (px < min_x) min_x = px;
                if (px > max_x) max_x = px;
                if (py < min_y) min_y = py;
                if (py > max_y) max_y = py;
                if (px == 0 || py == 0 ||
                    px + 1u == width || py + 1u == height)
                    touches_edge = true;

                neighbors[0] = px > 0 ? p - 1u : UINT32_MAX;
                neighbors[1] = px + 1u < width ? p + 1u : UINT32_MAX;
                neighbors[2] = py > 0 ? p - width : UINT32_MAX;
                neighbors[3] = py + 1u < height ? p + width : UINT32_MAX;
                for (size_t i = 0; i < 4; i++) {
                    uint32_t np = neighbors[i];
                    if (np == UINT32_MAX || seen[np] ||
                        pixels[(size_t)np * 4u + 3u] >= 16)
                        continue;
                    seen[np] = 1;
                    queue[tail++] = np;
                }
            }

            if (!touches_edge && area > best_area &&
                max_x > min_x + 120u && max_y > min_y + 160u) {
                double aspect = (double)(max_x - min_x + 1u) /
                    (double)(max_y - min_y + 1u);
                if (aspect > 0.65 && aspect < 0.85) {
                    best_area = area;
                    best.x = min_x;
                    best.y = min_y;
                    best.w = max_x - min_x + 1u;
                    best.h = max_y - min_y + 1u;
                }
            }
        }
    }

    free(seen);
    free(queue);

    if (best_area == 0)
        return false;
    *out = best;
    return true;
}

/* Load the bezel artwork, its LCD aperture, icon strip and button mask.
 * The artwork is used by both layouts: the bezel draws all of it, the
 * frameless window draws just the icon strip under the LCD. */
static bool ui_load_art(void)
{
    if (art_pixels)
        return true;
    if (!ui_load_frame_pixels(&art_pixels, &art_w, &art_h))
        return false;
    if (!ui_load_button_mask())
        fprintf(stderr,
            "[UI] Button mask unavailable - using legacy frame button hit-test\n");
    if (ui_detect_frame_lcd_rect(art_pixels, art_w, art_h, &art_lcd)) {
        fprintf(stderr, "[UI] Frame LCD aperture: x=%.0f y=%.0f w=%.0f h=%.0f\n",
            art_lcd.x, art_lcd.y, art_lcd.w, art_lcd.h);
    } else {
        art_lcd = fallback_frame_lcd_rect;
        fprintf(stderr,
            "[UI] Frame LCD aperture fallback: x=%.0f y=%.0f w=%.0f h=%.0f\n",
            art_lcd.x, art_lcd.y, art_lcd.w, art_lcd.h);
    }
    art_icon = ui_layout_icon_from_lcd(art_lcd, art_h);
    return true;
}

/* Area-average resample of a (cx, cy, cw, ch) crop of the artwork to
 * dw x dh pixels. SDL2 has no mipmaps, so plain linear filtering aliases
 * the bezel badly when it is drawn at a fraction of its size. */
static uint8_t *ui_resample_art(double cx, double cy, double cw, double ch,
    int dw, int dh)
{
    uint8_t *dst;
    double sx = cw / dw, sy = ch / dh;

    if (!art_pixels || dw <= 0 || dh <= 0)
        return NULL;
    dst = malloc((size_t)dw * (size_t)dh * 4u);
    if (!dst)
        return NULL;

    for (int y = 0; y < dh; y++) {
        double y0 = cy + y * sy, y1 = y0 + sy;
        int iy0 = (int)floor(y0), iy1 = (int)ceil(y1);

        if (iy0 < 0) iy0 = 0;
        if (iy1 > (int)art_h) iy1 = (int)art_h;
        for (int x = 0; x < dw; x++) {
            double x0 = cx + x * sx, x1 = x0 + sx;
            int ix0 = (int)floor(x0), ix1 = (int)ceil(x1);
            double acc[4] = { 0, 0, 0, 0 }, wsum = 0.0;
            uint8_t *d = dst + ((size_t)y * (size_t)dw + (size_t)x) * 4u;

            if (ix0 < 0) ix0 = 0;
            if (ix1 > (int)art_w) ix1 = (int)art_w;
            for (int py = iy0; py < iy1; py++) {
                double wy = fmin(y1, py + 1.0) - fmax(y0, (double)py);
                if (wy <= 0.0)
                    continue;
                for (int px = ix0; px < ix1; px++) {
                    double wx = fmin(x1, px + 1.0) - fmax(x0, (double)px);
                    const uint8_t *s;
                    double w, a;
                    if (wx <= 0.0)
                        continue;
                    s = art_pixels + ((size_t)py * art_w + (size_t)px) * 4u;
                    w = wx * wy;
                    a = s[3] * w;
                    /* Premultiply so transparent texels don't darken edges. */
                    acc[0] += s[0] * a;
                    acc[1] += s[1] * a;
                    acc[2] += s[2] * a;
                    acc[3] += a;
                    wsum += w;
                }
            }
            if (acc[3] > 0.0) {
                d[0] = (uint8_t)fmin(255.0, acc[0] / acc[3] + 0.5);
                d[1] = (uint8_t)fmin(255.0, acc[1] / acc[3] + 0.5);
                d[2] = (uint8_t)fmin(255.0, acc[2] / acc[3] + 0.5);
                d[3] = (uint8_t)fmin(255.0, acc[3] / wsum + 0.5);
            } else {
                d[0] = d[1] = d[2] = d[3] = 0;
            }
        }
    }
    return dst;
}

static SDL_Texture *ui_texture_from_rgba(SDL_Renderer *ren, const uint8_t *px,
    int w, int h)
{
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STATIC, w, h);

    if (!t)
        return NULL;
    if (SDL_UpdateTexture(t, NULL, px, w * 4) != 0) {
        SDL_DestroyTexture(t);
        return NULL;
    }
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    return t;
}

static double ui_backing_scale(void)
{
    return win_w_pt > 0 ? (double)out_w_px / (double)win_w_pt : 1.0;
}

/* Rebuild the artwork textures at the current backing resolution. */
static void ui_rebuild_art_textures(machine_t *m)
{
    SDL_Renderer *ren = (SDL_Renderer *)m->sdl_renderer;
    double s = ui_backing_scale();
    int w, h;
    uint8_t *px;

    art_textures_dirty = false;
    if (!ren || !art_pixels)
        return;

    if (layout.bezel) {
        w = (int)ceil(layout.frame_dst.w * s);
        h = (int)ceil(layout.frame_dst.h * s);
        if (frame_texture && w == frame_tex_w && h == frame_tex_h)
            return;
        px = ui_resample_art(0, 0, art_w, art_h, w, h);
        if (!px)
            return;
        if (frame_texture)
            SDL_DestroyTexture(frame_texture);
        frame_texture = ui_texture_from_rgba(ren, px, w, h);
        free(px);
        frame_tex_w = frame_texture ? w : 0;
        frame_tex_h = frame_texture ? h : 0;
        return;
    }

    if (!layout.strip)
        return;
    w = (int)ceil(layout.strip_dst.w * s);
    h = (int)ceil(layout.strip_dst.h * s);
    if (strip_texture && w == strip_tex_w && h == strip_tex_h)
        return;
    px = ui_resample_art(art_icon.x, art_icon.y, art_icon.w, art_icon.h, w, h);
    if (!px)
        return;
    if (strip_texture)
        SDL_DestroyTexture(strip_texture);
    strip_texture = ui_texture_from_rgba(ren, px, w, h);
    free(px);
    strip_tex_w = strip_texture ? w : 0;
    strip_tex_h = strip_texture ? h : 0;
}

/* Opacity of the bezel at a window point: the LCD is opaque, the rest
 * follows the artwork alpha. */
static uint8_t ui_bezel_alpha_at(double x, double y)
{
    double fx, fy;
    int sx, sy;

    if (x >= layout.lcd_dst.x && y >= layout.lcd_dst.y &&
        x < layout.lcd_dst.x + layout.lcd_dst.w &&
        y < layout.lcd_dst.y + layout.lcd_dst.h)
        return 255;
    if (!art_pixels || !ui_layout_window_to_frame(&layout, x, y, &fx, &fy))
        return 0;
    sx = (int)floor(fx);
    sy = (int)floor(fy);
    if (sx < 0 || sy < 0 || (uint32_t)sx >= art_w || (uint32_t)sy >= art_h)
        return 0;
    return art_pixels[((size_t)sy * art_w + (size_t)sx) * 4u + 3u];
}

/* Shape the borderless bezel window to the artwork: SDL's binary shape
 * (window points) everywhere, plus an anti-aliased layer mask at backing
 * resolution on macOS. */
static void ui_apply_bezel_shape(machine_t *m)
{
    SDL_Window *win = (SDL_Window *)m->sdl_window;
    double s = ui_backing_scale();
    uint8_t *mask;

    if (!win || !layout.bezel)
        return;

    if (shaped_window && SDL_IsShapedWindow(win)) {
        SDL_Surface *shape = SDL_CreateRGBSurfaceWithFormat(0, win_w_pt,
            win_h_pt, 32, SDL_PIXELFORMAT_RGBA32);
        if (shape && SDL_LockSurface(shape) == 0) {
            uint32_t clear = SDL_MapRGBA(shape->format, 0, 0, 0, 0);
            uint32_t solid = SDL_MapRGBA(shape->format, 255, 255, 255, 255);
            SDL_WindowShapeMode mode;

            for (int y = 0; y < win_h_pt; y++) {
                uint32_t *row = (uint32_t *)((uint8_t *)shape->pixels +
                    (size_t)y * (size_t)shape->pitch);
                for (int x = 0; x < win_w_pt; x++)
                    row[x] = ui_bezel_alpha_at(x + 0.5, y + 0.5) >= 16 ?
                        solid : clear;
            }
            SDL_UnlockSurface(shape);
            mode.mode = ShapeModeBinarizeAlpha;
            mode.parameters.binarizationCutoff = 1;
            if (SDL_SetWindowShape(win, shape, &mode) != 0)
                fprintf(stderr, "[UI] SDL window shape failed: %s\n",
                    SDL_GetError());
        }
        if (shape)
            SDL_FreeSurface(shape);
    }

    mask = malloc((size_t)out_w_px * (size_t)out_h_px);
    if (!mask)
        return;
    for (int y = 0; y < out_h_px; y++)
        for (int x = 0; x < out_w_px; x++)
            mask[(size_t)y * (size_t)out_w_px + (size_t)x] =
                ui_bezel_alpha_at((x + 0.5) / s, (y + 0.5) / s);
    ui_platform_make_window_transparent(win);
    ui_platform_apply_window_mask(win, mask, out_w_px, out_h_px);
    free(mask);
}

static void ui_update_menu_state(void)
{
    static const ui_cmd_t zooms[4] = {
        UI_CMD_ZOOM_1, UI_CMD_ZOOM_2, UI_CMD_ZOOM_3, UI_CMD_ZOOM_4
    };

    ui_cmd_set_checked(UI_CMD_BEZEL, bezel_on);
    ui_cmd_set_enabled(UI_CMD_BEZEL, art_pixels != NULL);
    ui_cmd_set_checked(UI_CMD_PIXEL_ACCURATE, pixel_accurate);
    for (int i = 0; i < 4; i++)
        ui_cmd_set_checked(zooms[i], fabs(layout.zoom - (i + 1)) < 0.01);
}

static void ui_update_lcd_scale_mode(machine_t *m)
{
    double px = layout.zoom * ui_backing_scale();
    bool integer = fabs(px - floor(px + 0.5)) < 0.01;

    if (m->sdl_texture)
        SDL_SetTextureScaleMode((SDL_Texture *)m->sdl_texture,
            (pixel_accurate || integer) ? SDL_ScaleModeNearest :
            SDL_ScaleModeLinear);
}

/* Recompute the layout for the window's current size. */
static void ui_relayout(machine_t *m, bool reshape)
{
    SDL_Window *win = (SDL_Window *)m->sdl_window;
    SDL_Renderer *ren = (SDL_Renderer *)m->sdl_renderer;
    double old_scale = ui_backing_scale();

    if (!win || !ren)
        return;
    SDL_GetWindowSize(win, &win_w_pt, &win_h_pt);
    if (SDL_GetRendererOutputSize(ren, &out_w_px, &out_h_px) != 0 ||
        out_w_px <= 0 || out_h_px <= 0) {
        out_w_px = win_w_pt;
        out_h_px = win_h_pt;
    }
    if (win_w_pt <= 0 || win_h_pt <= 0)
        return;
    SDL_RenderSetScale(ren, (float)out_w_px / (float)win_w_pt,
        (float)out_h_px / (float)win_h_pt);
    ui_layout_fit(&layout, win_w_pt, win_h_pt);
    ui_update_lcd_scale_mode(m);
    ui_update_menu_state();

    if (!frame_texture && !strip_texture) {
        ui_rebuild_art_textures(m);
    } else {
        art_textures_dirty = true;
        art_textures_due = SDL_GetTicks() + RESAMPLE_DEBOUNCE_MS;
    }
    if (layout.bezel && (reshape || old_scale != ui_backing_scale()))
        ui_apply_bezel_shape(m);
}

static void ui_configure_layout(machine_t *m, bool bezel)
{
    memset(&layout, 0, sizeof(layout));
    layout.fb_w = m->fb_width;
    layout.fb_h = m->fb_height;
    layout.bezel = bezel && art_pixels != NULL;
    layout.strip = art_pixels != NULL;
    layout.frame_w = art_w;
    layout.frame_h = art_h;
    layout.frame_lcd = art_lcd;
    layout.frame_icon = art_icon;
}

static double ui_clamp_zoom(double z)
{
    if (!(z >= UI_ZOOM_MIN))
        return 1.0;
    return z > UI_ZOOM_MAX ? UI_ZOOM_MAX : z;
}

static SDL_HitTestResult SDLCALL ui_window_hit_test(SDL_Window *win,
    const SDL_Point *area, void *data);

/* Create the window, renderer and textures for the requested layout. */
static bool ui_open_window(machine_t *m, bool bezel, double zoom,
    bool have_pos, int x, int y)
{
    Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN;
    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    int w, h;
    int px = have_pos ? x : (int)SDL_WINDOWPOS_CENTERED;
    int py = have_pos ? y : (int)SDL_WINDOWPOS_CENTERED;

    ui_configure_layout(m, bezel);
    bezel_on = layout.bezel;
    zoom = ui_clamp_zoom(zoom);
    /* Never open taller or wider than the display's usable area. */
    {
        SDL_Rect usable;
        int idx = 0;
        int n = SDL_GetNumVideoDisplays();

        for (int i = 0; have_pos && i < n; i++) {
            SDL_Rect r;
            if (SDL_GetDisplayBounds(i, &r) == 0 && x >= r.x && y >= r.y &&
                x < r.x + r.w && y < r.y + r.h)
                idx = i;
        }
        if (SDL_GetDisplayUsableBounds(idx, &usable) == 0) {
            double fit = ui_layout_zoom_for(&layout, usable.w,
                usable.h - (bezel_on ? 0 : 56));
            if (fit > 0.0 && zoom > fit)
                zoom = ui_clamp_zoom(fit);
        }
    }
    ui_layout_natural_size(&layout, zoom, &w, &h);

    if (bezel_on) {
        win = SDL_CreateShapedWindow(window_title, (unsigned)px, (unsigned)py,
            (unsigned)w, (unsigned)h, flags | SDL_WINDOW_BORDERLESS);
        shaped_window = win != NULL;
        if (!win) {
            fprintf(stderr,
                "[UI] SDL_CreateShapedWindow failed: %s - using borderless rectangle\n",
                SDL_GetError());
            win = SDL_CreateWindow(window_title, px, py, w, h,
                flags | SDL_WINDOW_BORDERLESS);
        }
    } else {
        win = SDL_CreateWindow(window_title, px, py, w, h,
            flags | SDL_WINDOW_RESIZABLE);
    }
    if (!win) {
        fprintf(stderr, "[UI] SDL_CreateWindow failed: %s — continuing headless\n",
            SDL_GetError());
        return false;
    }
    m->sdl_window = win;
    if (bezel_on) {
        /* Shaped windows are created fixed-size; the bezel's corners
         * resize it through the hit-test. */
        SDL_SetWindowResizable(win, SDL_TRUE);
        ui_platform_make_window_transparent(win);
        if (SDL_SetWindowHitTest(win, ui_window_hit_test, m) != 0)
            fprintf(stderr, "[UI] SDL window drag hit-test failed: %s\n",
                SDL_GetError());
    }

    /* Prefer the accelerated renderer even for the shaped frame window:
     * sdl2-compat (SDL3) applies SDL_SetWindowShape at present time with a
     * custom blend mode the software renderer lacks, which paints the opaque
     * white shape surface over the whole window. */
    ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (!ren)
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren)
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        fprintf(stderr, "[UI] SDL_CreateRenderer failed: %s — continuing headless\n",
            SDL_GetError());
        ui_close_window(m);
        return false;
    }
    m->sdl_renderer = ren;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    /* BE-300 hardware uses RGB565 (R=15:11, G=10:5, B=4:0) */
    m->sdl_texture = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING, (int)m->fb_width, (int)m->fb_height);
    if (!m->sdl_texture) {
        fprintf(stderr, "[UI] SDL_CreateTexture failed: %s — continuing headless\n",
            SDL_GetError());
        ui_close_window(m);
        return false;
    }

    ui_platform_attach_window(win, bezel_on, window_title, window_subtitle,
        ui_layout_aspect(&layout));
    ui_relayout(m, true);
    SDL_ShowWindow(win);
    SDL_RaiseWindow(win);
    fprintf(stderr, "[UI] %s window: zoom %.3gx, %dx%d points, %dx%d pixels\n",
        bezel_on ? "Bezel" : (layout.strip ? "LCD + icon strip" : "LCD-only"),
        layout.zoom, win_w_pt, win_h_pt, out_w_px, out_h_px);
    return true;
}

static void ui_remember_geometry(machine_t *m)
{
    SDL_Window *win = (SDL_Window *)m->sdl_window;

    if (!win)
        return;
    SDL_GetWindowPosition(win, &saved_geom.x, &saved_geom.y);
    saved_geom.have_pos = true;
    saved_geom.zoom = layout.zoom > 0.0 ? layout.zoom : 1.0;
    saved_geom.bezel = bezel_on;
    saved_geom.pixel = pixel_accurate;
    saved_geom.valid = true;
}

/* ---- Preferences: window position and view settings across runs ---- */

/* BE300_WINDOW_PREFS overrides the file; set it empty to disable. */
static bool ui_prefs_path(char *out, size_t cap)
{
    const char *env = getenv("BE300_WINDOW_PREFS");
    char *dir;
    int n;

    if (env) {
        n = snprintf(out, cap, "%s", env);
        return *env && n > 0 && (size_t)n < cap;
    }
    dir = SDL_GetPrefPath("BE300", "Emulator");
    if (!dir)
        return false;
    n = snprintf(out, cap, "%swindow.cfg", dir);
    SDL_free(dir);
    return n > 0 && (size_t)n < cap;
}

static void ui_prefs_load(void)
{
    char path[UI_PATH_MAX], line[128];
    FILE *f;

    if (prefs_loaded)
        return;
    prefs_loaded = true;
    if (!ui_prefs_path(path, sizeof(path)) || !(f = fopen(path, "r")))
        return;
    saved_geom.zoom = 1.0;
    while (fgets(line, sizeof(line), f)) {
        int iv;
        double dv;
        if (sscanf(line, "x=%d", &iv) == 1) {
            saved_geom.x = iv;
            saved_geom.have_pos = true;
        } else if (sscanf(line, "y=%d", &iv) == 1) {
            saved_geom.y = iv;
        } else if (sscanf(line, "zoom=%lf", &dv) == 1) {
            saved_geom.zoom = ui_clamp_zoom(dv);
        } else if (sscanf(line, "bezel=%d", &iv) == 1) {
            saved_geom.bezel = iv != 0;
        } else if (sscanf(line, "pixel=%d", &iv) == 1) {
            saved_geom.pixel = iv != 0;
        }
    }
    fclose(f);
    saved_geom.valid = true;
}

static void ui_prefs_save(void)
{
    char path[UI_PATH_MAX];
    FILE *f;

    if (!saved_geom.valid || !ui_prefs_path(path, sizeof(path)) ||
        !(f = fopen(path, "w")))
        return;
    fprintf(f, "x=%d\ny=%d\nzoom=%.4f\nbezel=%d\npixel=%d\n",
        saved_geom.x, saved_geom.y, saved_geom.zoom,
        saved_geom.bezel ? 1 : 0, saved_geom.pixel ? 1 : 0);
    fclose(f);
}

/* A saved position is only used when it is still on a display. */
static bool ui_position_visible(int x, int y)
{
    int n = SDL_GetNumVideoDisplays();

    for (int i = 0; i < n; i++) {
        SDL_Rect r;
        if (SDL_GetDisplayUsableBounds(i, &r) == 0 &&
            x + 40 >= r.x && y >= r.y - 10 &&
            x + 40 < r.x + r.w && y + 20 < r.y + r.h)
            return true;
    }
    return false;
}

/* ---- Window state changes from the menus ---- */

static void ui_reopen_window(machine_t *m, bool bezel, double zoom)
{
    SDL_Window *win = (SDL_Window *)m->sdl_window;
    int x = 0, y = 0, w = 0, h = 0, nw, nh;
    bool have_pos = false;

    if (win) {
        SDL_GetWindowPosition(win, &x, &y);
        SDL_GetWindowSize(win, &w, &h);
        have_pos = true;
    }
    mouse_button_held = false;
    ui_close_window(m);

    /* Keep the window centred where it was. */
    {
        ui_layout_t probe;
        ui_configure_layout(m, bezel);
        probe = layout;
        ui_layout_natural_size(&probe, ui_clamp_zoom(zoom), &nw, &nh);
    }
    if (have_pos) {
        x += (w - nw) / 2;
        y += (h - nh) / 2;
    }
    if (!ui_open_window(m, bezel, zoom, have_pos, x, y))
        fprintf(stderr, "[UI] Could not reopen the window\n");
}

static void ui_set_zoom(machine_t *m, double zoom)
{
    SDL_Window *win = (SDL_Window *)m->sdl_window;
    int w, h;

    if (!win)
        return;
    ui_layout_natural_size(&layout, ui_clamp_zoom(zoom), &w, &h);
    SDL_SetWindowSize(win, w, h);
    ui_relayout(m, true);
}

static void ui_fit_screen(machine_t *m)
{
    SDL_Window *win = (SDL_Window *)m->sdl_window;
    SDL_Rect usable;
    int top = 0, left = 0, bottom = 0, right = 0, w, h, idx;
    double z;

    if (!win)
        return;
    idx = SDL_GetWindowDisplayIndex(win);
    if (idx < 0 || SDL_GetDisplayUsableBounds(idx, &usable) != 0)
        return;
    if (SDL_GetWindowBordersSize(win, &top, &left, &bottom, &right) != 0 &&
        !bezel_on)
        top = 56;  /* title bar + toolbar where SDL can't report it */
    z = ui_layout_zoom_for(&layout,
        (int)((usable.w - left - right) * 0.96),
        (int)((usable.h - top - bottom) * 0.96));
    z = ui_clamp_zoom(z);
    ui_layout_natural_size(&layout, z, &w, &h);
    SDL_SetWindowSize(win, w, h);
    SDL_SetWindowPosition(win, usable.x + (usable.w - w) / 2,
        usable.y + top + (usable.h - top - h) / 2);
    ui_relayout(m, true);
}

/* Pixel Accurate: whole backing pixels per LCD pixel. */
static void ui_snap_pixel_zoom(machine_t *m)
{
    double s = ui_backing_scale();
    double px = floor(layout.zoom * s + 1e-6);

    if (!pixel_accurate || s <= 0.0)
        return;
    if (px < 1.0)
        px = 1.0;
    if (fabs(px / s - layout.zoom) * (double)m->fb_width > 1.0)
        ui_set_zoom(m, px / s);
}

static bool ui_point_in_ellipse(double x, double y, double cx, double cy,
    double rx, double ry)
{
    double dx = (x - cx) / rx;
    double dy = (y - cy) / ry;

    return dx * dx + dy * dy <= 1.0;
}

static bool ui_frame_hit_button_fallback(double bx, double by,
    uint8_t *btn_set1_out, uint8_t *btn_set2_out)
{
    if (ui_point_in_ellipse(bx, by, 512.0, 1335.0, 150.0, 115.0)) {
        double dx = (bx - 512.0) / 150.0;
        double dy = (by - 1335.0) / 115.0;

        if (dx * dx + dy * dy < 0.18) {
            *btn_set1_out = 0x04u;  /* ok */
        } else if (dy < -0.35 && -dy >= (dx < 0 ? -dx : dx)) {
            *btn_set1_out = 0x10u;  /* up */
        } else if (dy > 0.35 && dy >= (dx < 0 ? -dx : dx)) {
            *btn_set1_out = 0x20u;  /* down */
        } else if (dx > 0.0) {
            *btn_set1_out = 0x40u;  /* right */
        } else {
            *btn_set1_out = 0x80u;  /* left */
        }
        return true;
    }

    if (ui_point_in_ellipse(bx, by, 176.0, 1329.0, 105.0, 55.0)) {
        *btn_set2_out = 0x10u;      /* rocket/modifier */
        return true;
    }
    if (ui_point_in_ellipse(bx, by, 848.0, 1329.0, 105.0, 55.0)) {
        *btn_set2_out = 0x80u;      /* power */
        return true;
    }
    if (ui_point_in_ellipse(bx, by, 343.0, 1447.0, 85.0, 50.0)) {
        *btn_set1_out = 0x04u;      /* ok */
        return true;
    }
    if (ui_point_in_ellipse(bx, by, 681.0, 1447.0, 95.0, 55.0)) {
        *btn_set1_out = 0x08u;      /* esc */
        return true;
    }

    return false;
}

static bool ui_frame_hit_button(machine_t *m, int win_x, int win_y,
    uint8_t *btn_set1_out, uint8_t *btn_set2_out)
{
    double fx, fy, bx, by;

    (void)m;
    *btn_set1_out = 0;
    *btn_set2_out = 0;

    if (!ui_layout_window_to_frame(&layout, win_x, win_y, &fx, &fy))
        return false;

    bx = fx + (double)frame_trim_x;
    by = fy + (double)frame_trim_y;

    if (button_mask.active)
        return ui_button_mask_hit(&button_mask, bx, by, btn_set1_out,
            btn_set2_out);

    return ui_frame_hit_button_fallback(bx, by, btn_set1_out,
        btn_set2_out);
}

static SDL_HitTestResult SDLCALL ui_window_hit_test(SDL_Window *win,
    const SDL_Point *area, void *data)
{
    machine_t *m = (machine_t *)data;
    uint8_t s1, s2;
    double fx, fy;

    (void)win;
    if (!layout.bezel)
        return SDL_HITTEST_NORMAL;
    if (ui_layout_point_is_touch(&layout, area->x, area->y) ||
        ui_frame_hit_button(m, area->x, area->y, &s1, &s2))
        return SDL_HITTEST_NORMAL;
    /* The lower corners of the bezel body resize the window. */
    if (ui_layout_window_to_frame(&layout, area->x, area->y, &fx, &fy) &&
        fy > art_h * 0.90 && ui_bezel_alpha_at(area->x, area->y) >= 16) {
        if (fx > art_w * 0.85)
            return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (fx < art_w * 0.15)
            return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    }
    return SDL_HITTEST_DRAGGABLE;
}

static bool ui_window_to_touch(machine_t *m, int win_x, int win_y,
    uint16_t *x_out, uint16_t *y_out)
{
    (void)m;
    return ui_layout_window_to_touch(&layout, win_x, win_y, x_out, y_out);
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

    if (ui_window_to_touch(m, win_x, win_y, &tx, &ty))
        ui_latch_touch_position(m, tx, ty);
}

static bool ui_begin_touch(machine_t *m, int win_x, int win_y,
    uint32_t now)
{
    uint16_t tx, ty;

    if (!ui_window_to_touch(m, win_x, win_y, &tx, &ty))
        return false;

    touch_active = true;
    touch_release_pending = false;
    touch_down_tick = now;
    touch_release_x = tx;
    touch_release_y = ty;
    be300_set_touch(m, true, tx, ty);
    return true;
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

static void ui_press_pointer_button(machine_t *m, uint8_t btn_set1,
    uint8_t btn_set2, uint32_t now)
{
    pointer_mode = UI_POINTER_BUTTON;
    button_release_pending = false;
    button_down_tick = now;
    ui_buttons_set_pointer(m, btn_set1, btn_set2);
}

static void ui_release_pointer_button(machine_t *m)
{
    if (pointer_mode != UI_POINTER_BUTTON)
        return;

    ui_buttons_set_pointer(m, 0, 0);
    button_release_pending = false;
    pointer_mode = UI_POINTER_NONE;
}

static void ui_schedule_pointer_button_release(machine_t *m, uint32_t now)
{
    uint32_t dwell_due = button_down_tick + button_min_dwell_ms;

    button_release_due_tick = ui_later_tick(dwell_due, now);
    if (pointer_mode != UI_POINTER_BUTTON ||
        ui_tick_reached(now, button_release_due_tick)) {
        ui_release_pointer_button(m);
    } else {
        button_release_pending = true;
    }
}

/* ---- Commands ---- */

static void ui_command_button(machine_t *m, uint8_t set1, uint8_t set2,
    uint32_t now)
{
    if (pointer_mode == UI_POINTER_TOUCH)
        return;
    ui_press_pointer_button(m, set1, set2, now);
    ui_schedule_pointer_button_release(m, now);
}

static bool ui_has_suffix_ci(const char *s, const char *suffix)
{
    size_t n = strlen(s), k = strlen(suffix);

    if (k > n)
        return false;
    for (size_t i = 0; i < k; i++) {
        char a = s[n - k + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

static const char *ui_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    const char *b = strrchr(path, '\\');

    if (b && (!s || b > s))
        s = b;
    return s ? s + 1 : path;
}

static void ui_release_all_input(machine_t *m)
{
    mouse_button_held = false;
    ui_buttons_release_all(m);
    ui_release_pointer_button(m);
    ui_stowaway_release_keys();
    if (touch_active)
        ui_release_touch(m);
    pointer_mode = UI_POINTER_NONE;
    SDL_CaptureMouse(SDL_FALSE);
}

static bool ui_confirm(machine_t *m, const char *title, const char *text,
    const char *ok_label)
{
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel" },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, ok_label },
    };
    SDL_MessageBoxData box = {
        SDL_MESSAGEBOX_WARNING, (SDL_Window *)m->sdl_window, title, text,
        2, buttons, NULL
    };
    int pressed = 0;

    ui_release_all_input(m);
    return SDL_ShowMessageBox(&box, &pressed) == 0 && pressed == 1;
}

/* Try the install on a copy of the running machine's NAND so a package
 * the installer rejects is reported now, before the guest is restarted.
 * The real install happens after shutdown (src/be300_session.c). */
static bool ui_install_dry_run(machine_t *m, const char *path, char *log,
    size_t log_cap)
{
    FILE *f = fopen(path, "rb");
    uint8_t *pkg = NULL, *copy = NULL;
    size_t image_len = m->nand_size;
    long len = -1;
    bool ok = false;

    if (m->nand_file_size > 0 && m->nand_file_size < image_len)
        image_len = m->nand_file_size;
    if (f && fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 &&
        fseek(f, 0, SEEK_SET) == 0 && (pkg = malloc((size_t)len)) &&
        fread(pkg, 1, (size_t)len, f) == (size_t)len &&
        m->nand_data && (copy = malloc(image_len))) {
        memcpy(copy, m->nand_data, image_len);
        ok = cbea_install(copy, image_len, pkg, (size_t)len, log, log_cap) == 0;
    } else {
        snprintf(log, log_cap, "error: cannot read the package or the NAND image\n");
    }
    if (f)
        fclose(f);
    free(pkg);
    free(copy);
    return ok;
}

static void ui_offer_install(machine_t *m, const char *path)
{
    char text[UI_PATH_MAX + 256];
    char log[2048];

    if (!m->cfg.nand_path) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Can't Install App",
            "Apps are installed into the NAND image, and this machine was "
            "not started with one (--nand).", (SDL_Window *)m->sdl_window);
        return;
    }
    if (!ui_has_suffix_ci(path, ".cbea")) {
        snprintf(text, sizeof(text),
            "\"%s\" is not a CASIO app package (.cbea).", ui_basename(path));
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Can't Install App",
            text, (SDL_Window *)m->sdl_window);
        return;
    }
    if (!ui_install_dry_run(m, path, log, sizeof(log))) {
        snprintf(text, sizeof(text), "\"%s\" can't be installed.\n\n%s",
            ui_basename(path), log);
        fprintf(stderr, "[UI] Install check failed for %s:\n%s", path, log);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Can't Install App",
            text, (SDL_Window *)m->sdl_window);
        return;
    }
    snprintf(text, sizeof(text),
        "Install \"%s\" into the NAND image?\n\n"
        "The BE-300 restarts with a cold boot, like removing the battery. "
        "Unsaved work in the guest is lost.", ui_basename(path));
    if (!ui_confirm(m, "Install App", text, "Install and Restart"))
        return;
    snprintf(pending_package, sizeof(pending_package), "%s", path);
    restart_requested = true;
    quit_requested = true;
}

static void ui_restart(machine_t *m)
{
    if (!ui_confirm(m, "Restart BE-300",
        "Restart the BE-300 with a cold boot?\n\n"
        "This is like removing the battery. Unsaved work in the guest is lost.",
        "Restart"))
        return;
    pending_package[0] = '\0';
    restart_requested = true;
    quit_requested = true;
}

static void ui_format_screenshot_name(char *out, size_t cap, const char *dir,
    const char *ext)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);

    snprintf(out, cap, "%s/BE-300 Screenshot %04d-%02d-%02d at %02d.%02d.%02d.%s",
        dir, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
        tm->tm_hour, tm->tm_min, tm->tm_sec, ext);
}

static void ui_draw_scene(machine_t *m);

/* Render the window (bezel included) at backing resolution into RGBA. */
static uint8_t *ui_capture_window(machine_t *m, int *w_out, int *h_out)
{
    SDL_Renderer *ren = (SDL_Renderer *)m->sdl_renderer;
    SDL_Texture *target;
    uint8_t *px;
    int w = out_w_px, h = out_h_px;

    if (!ren || w <= 0 || h <= 0)
        return NULL;
    target = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_TARGET, w, h);
    if (!target)
        return NULL;
    px = malloc((size_t)w * (size_t)h * 4u);
    if (px) {
        SDL_SetRenderTarget(ren, target);
        SDL_RenderSetScale(ren, (float)w / (float)win_w_pt,
            (float)h / (float)win_h_pt);
        ui_draw_scene(m);
        /* Read in plain pixels: with the HiDPI render scale applied,
         * sdl2-compat scales the read rectangle past the target's edge. */
        SDL_RenderSetScale(ren, 1.0f, 1.0f);
        SDL_Rect all = { 0, 0, w, h };
        if (SDL_RenderReadPixels(ren, &all, SDL_PIXELFORMAT_RGBA32, px,
            w * 4) != 0) {
            free(px);
            px = NULL;
        }
        SDL_SetRenderTarget(ren, NULL);
        SDL_RenderSetScale(ren, (float)w / (float)win_w_pt,
            (float)h / (float)win_h_pt);
    }
    SDL_DestroyTexture(target);
    *w_out = w;
    *h_out = h;
    return px;
}

static void ui_take_user_screenshot(machine_t *m, bool to_clipboard)
{
    char dir[UI_PATH_MAX], path[UI_PATH_MAX + 64];
    uint8_t *px;
    int w = 0, h = 0;

    px = ui_capture_window(m, &w, &h);
    if (to_clipboard) {
        if (px && ui_platform_copy_image(px, w, h))
            fprintf(stderr, "[UI] Copied the screen to the clipboard\n");
        else
            fprintf(stderr, "[UI] Copy Screen is not available here\n");
        free(px);
        return;
    }
    if (!ui_platform_screenshot_dir(dir, sizeof(dir)))
        snprintf(dir, sizeof(dir), ".");
#ifdef HAVE_PNG
    if (px) {
        ui_format_screenshot_name(path, sizeof(path), dir, "png");
        if (ui_save_png_rgba(path, px, (uint32_t)w, (uint32_t)h)) {
            fprintf(stderr, "[UI] Window screenshot saved: %s\n", path);
            free(px);
            return;
        }
    }
#endif
    free(px);
    /* No PNG support or no render targets: the LCD as a BMP. */
    if (!ui_resolve_framebuffer(m) || !staging_buf)
        return;
    ui_copy_visible_frame(m, staging_buf);
    ui_format_screenshot_name(path, sizeof(path), dir, "bmp");
    if (ui_write_rgb565_bmp(path, staging_buf, m->fb_width, m->fb_height))
        fprintf(stderr, "[UI] Window screenshot saved: %s\n", path);
    else
        fprintf(stderr, "[UI] Window screenshot failed: %s\n", path);
}

static void ui_show_shortcuts(machine_t *m)
{
    char text[4096];

#ifdef __APPLE__
    ui_cmd_help_text(true, text, sizeof(text));
#else
    ui_cmd_help_text(false, text, sizeof(text));
#endif
    ui_release_all_input(m);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Keyboard Shortcuts",
        text, (SDL_Window *)m->sdl_window);
}

static void ui_run_command(machine_t *m, ui_cmd_t cmd, uint32_t now)
{
    char path[UI_PATH_MAX];

    switch (cmd) {
    case UI_CMD_QUIT:
    case UI_CMD_CLOSE:
        quit_requested = true;
        break;
    case UI_CMD_ABOUT:
        ui_platform_about((SDL_Window *)m->sdl_window);
        break;
    case UI_CMD_INSTALL_APP:
        ui_release_all_input(m);
        if (ui_platform_open_file((SDL_Window *)m->sdl_window,
            "Choose a CASIO app package to install", "cbea", path,
            sizeof(path)))
            ui_offer_install(m, path);
        break;
    case UI_CMD_SCREENSHOT:
        screenshot_pending = true;
        break;
    case UI_CMD_COPY_SCREEN:
        copy_pending = true;
        break;
    case UI_CMD_ROCKET: ui_command_button(m, 0, 0x10u, now); break;
    case UI_CMD_POWER:  ui_command_button(m, 0, 0x80u, now); break;
    case UI_CMD_OK:     ui_command_button(m, 0x04u, 0, now); break;
    case UI_CMD_ESC:    ui_command_button(m, 0x08u, 0, now); break;
    case UI_CMD_UP:     ui_command_button(m, 0x10u, 0, now); break;
    case UI_CMD_DOWN:   ui_command_button(m, 0x20u, 0, now); break;
    case UI_CMD_RIGHT:  ui_command_button(m, 0x40u, 0, now); break;
    case UI_CMD_LEFT:   ui_command_button(m, 0x80u, 0, now); break;
    case UI_CMD_RESTART:
        ui_restart(m);
        break;
    case UI_CMD_BEZEL:
        if (art_pixels) {
            ui_release_all_input(m);
            ui_reopen_window(m, !bezel_on, layout.zoom);
        }
        break;
    case UI_CMD_ZOOM_1: ui_set_zoom(m, 1.0); break;
    case UI_CMD_ZOOM_2: ui_set_zoom(m, 2.0); break;
    case UI_CMD_ZOOM_3: ui_set_zoom(m, 3.0); break;
    case UI_CMD_ZOOM_4: ui_set_zoom(m, 4.0); break;
    case UI_CMD_FIT:
        ui_fit_screen(m);
        break;
    case UI_CMD_PIXEL_ACCURATE:
        pixel_accurate = !pixel_accurate;
        ui_update_lcd_scale_mode(m);
        ui_snap_pixel_zoom(m);
        ui_update_menu_state();
        break;
    case UI_CMD_MINIMIZE:
        SDL_MinimizeWindow((SDL_Window *)m->sdl_window);
        break;
    case UI_CMD_SHORTCUTS:
        ui_show_shortcuts(m);
        break;
    default:
        break;
    }
}

/* Menu shortcuts typed in the window. Cmd on macOS, Ctrl elsewhere; with
 * the Stowaway keyboard on a non-Mac host Ctrl belongs to the guest, so
 * the shortcuts need Ctrl+Shift. Returns true when the key was a
 * shortcut (it must then not reach the guest). */
static bool ui_shortcut_key(machine_t *m, const SDL_KeyboardEvent *key)
{
    SDL_Keycode sym = key->keysym.sym;
    Uint16 mod = key->keysym.mod;
    unsigned mods = 0;
    ui_cmd_t cmd;
    char ch;

    if (sym >= 'a' && sym <= 'z')
        ch = (char)sym;
    else if (sym >= '0' && sym <= '9')
        ch = (char)sym;
    else
        return false;

#ifdef __APPLE__
    if (!(mod & KMOD_GUI))
        return false;
    mods |= UI_MOD_CMD;
    if (mod & KMOD_SHIFT)
        mods |= UI_MOD_SHIFT;
#else
    if (!(mod & KMOD_CTRL) || (mod & KMOD_GUI))
        return false;
    mods |= UI_MOD_CMD;
    if (m->cfg.enable_stowaway_keyboard) {
        if (!(mod & KMOD_SHIFT))
            return false;
        cmd = ui_cmd_for_shortcut(ch, UI_MOD_CMD | UI_MOD_SHIFT);
        if (cmd == UI_CMD_NONE)
            cmd = ui_cmd_for_shortcut(ch, UI_MOD_CMD |
                ((mod & KMOD_ALT) ? UI_MOD_ALT : 0));
        goto found;
    }
    if (mod & KMOD_SHIFT)
        mods |= UI_MOD_SHIFT;
#endif
    if (mod & KMOD_ALT)
        mods |= UI_MOD_ALT;
    cmd = ui_cmd_for_shortcut(ch, mods);
#ifndef __APPLE__
found:
#endif
    if (cmd == UI_CMD_NONE)
        return false;
    if (key->repeat)
        return true;
    /* The native menu bar already ran it from its key equivalent. */
    if (!ui_platform_menu_handles_shortcuts())
        ui_cmd_post(cmd);
    return true;
}

/* ---- Lifecycle ---- */

static void ui_build_titles(machine_t *m)
{
    const char *name = m->cfg.display_name;

    snprintf(window_title, sizeof(window_title), "BE-300");
    if (name && *name)
        snprintf(window_subtitle, sizeof(window_subtitle), "%s", name);
    else if (m->cfg.nand_path)
        snprintf(window_subtitle, sizeof(window_subtitle), "%s",
            ui_basename(m->cfg.nand_path));
    else
        window_subtitle[0] = '\0';
}

int ui_init(machine_t *m)
{
    bool bezel;
    double zoom;
    bool have_pos = false;
    int x = 0, y = 0;

    if (m->fb_width == 0)
        m->fb_width = 240;
    if (m->fb_height == 0)
        m->fb_height = 320;
    if (m->fb_stride == 0)
        m->fb_stride = 256;

#ifdef SDL_MAIN_HANDLED
    SDL_SetMainReady();
#endif

    quit_requested = false;
    restart_requested = false;
    pending_package[0] = '\0';
    screenshot_pending = copy_pending = false;
    ui_free_art();
    mouse_button_held = false;
    pointer_mode = UI_POINTER_NONE;
    ui_buttons_release_all(m);
    ui_stowaway_release_keys();
    touch_active = false;
    touch_release_pending = false;
    touch_down_tick = 0;
    touch_release_due_tick = 0;
    touch_min_dwell_ms = ui_ms_from_env("BE300_TOUCH_MIN_DWELL_MS",
        TOUCH_MIN_DWELL_DEFAULT_MS, TOUCH_MIN_DWELL_MAX_MS);
    button_release_pending = false;
    button_down_tick = 0;
    button_release_due_tick = 0;
    button_min_dwell_ms = ui_ms_from_env("BE300_BUTTON_MIN_DWELL_MS",
        BUTTON_MIN_DWELL_DEFAULT_MS, BUTTON_MIN_DWELL_MAX_MS);
    last_frame_tick = 0;
    ui_cmd_clear();

    ui_audio_init();

    /* Skip SDL when no display server is available (headless / Docker) */
#if !defined(__APPLE__) && !defined(_WIN32)
    if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
        fprintf(stderr, "[UI] No DISPLAY/WAYLAND_DISPLAY — running headless\n");
        return 0;
    }
#endif

    /* If something upstream (e.g., the VM manager launcher) already owns
     * SDL_INIT_VIDEO, init becomes a refcount bump and we must NOT call
     * SDL_QuitSubSystem on teardown. Only claim ownership when we
     * transition the subsystem from off → on. */
    bool video_was_up = (SDL_WasInit(SDL_INIT_VIDEO) != 0);
    if (!video_was_up)
        ui_platform_sdl_hints();
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[UI] SDL_Init failed: %s — continuing headless\n",
                SDL_GetError());
        return 0;
    }
    sdl_video_initialized = !video_was_up;
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    if (!ui_load_art())
        fprintf(stderr,
            "[UI] BE-300 artwork unavailable - LCD-only window, no icon strip\n");

    /* Window settings: a restart in this session keeps the window as it
     * was; otherwise the saved preferences fill in what the configuration
     * leaves open. */
    bezel = m->cfg.frame_visible;
    zoom = m->cfg.scale > 0.0 ? m->cfg.scale : 1.0;
    if (!saved_geom.valid)
        ui_prefs_load();
    if (saved_geom.valid) {
        if (saved_geom.have_pos &&
            ui_position_visible(saved_geom.x, saved_geom.y)) {
            have_pos = true;
            x = saved_geom.x;
            y = saved_geom.y;
        }
        if (m->cfg.restore_window_bezel)
            bezel = saved_geom.bezel;
        if (m->cfg.restore_window_zoom)
            zoom = saved_geom.zoom;
        pixel_accurate = saved_geom.pixel;
    }

    ui_build_titles(m);
    ui_cmd_set_emulator_active(true);
    ui_cmd_set_enabled(UI_CMD_INSTALL_APP, m->cfg.nand_path != NULL);
    ui_platform_install_menus();

    staging_buf = malloc(m->fb_width * m->fb_height * sizeof(uint16_t));
    if (!staging_buf) {
        fprintf(stderr, "[UI] staging buffer alloc failed — continuing headless\n");
        ui_video_destroy(m);
        return 0;
    }

    if (!ui_open_window(m, bezel, zoom, have_pos, x, y)) {
        ui_video_destroy(m);
        return 0;
    }
    return 0;
}

static void ui_draw_scene(machine_t *m)
{
    SDL_Renderer *ren = (SDL_Renderer *)m->sdl_renderer;
    SDL_FRect lcd = {
        (float)layout.lcd_dst.x, (float)layout.lcd_dst.y,
        (float)layout.lcd_dst.w, (float)layout.lcd_dst.h
    };

    if (layout.bezel)
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
    else
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopyF(ren, (SDL_Texture *)m->sdl_texture, NULL, &lcd);
    if (layout.bezel && frame_texture) {
        SDL_FRect dst = {
            (float)layout.frame_dst.x, (float)layout.frame_dst.y,
            (float)layout.frame_dst.w, (float)layout.frame_dst.h
        };
        SDL_RenderCopyF(ren, frame_texture, NULL, &dst);
    } else if (!layout.bezel && layout.strip && strip_texture) {
        SDL_FRect dst = {
            (float)layout.strip_dst.x, (float)layout.strip_dst.y,
            (float)layout.strip_dst.w, (float)layout.strip_dst.h
        };
        SDL_RenderCopyF(ren, strip_texture, NULL, &dst);
    }
}

static void ui_handle_window_event(machine_t *m, const SDL_WindowEvent *we)
{
    switch (we->event) {
    case SDL_WINDOWEVENT_FOCUS_LOST:
        ui_release_all_input(m);
        break;
    case SDL_WINDOWEVENT_SIZE_CHANGED:
    case SDL_WINDOWEVENT_RESIZED:
        ui_relayout(m, true);
        ui_snap_pixel_zoom(m);
        break;
    case SDL_WINDOWEVENT_DISPLAY_CHANGED:
    case SDL_WINDOWEVENT_MOVED:
        /* Moving to a display with another backing scale. */
        {
            int ow = 0, oh = 0;
            SDL_GetRendererOutputSize((SDL_Renderer *)m->sdl_renderer, &ow, &oh);
            if (ow != out_w_px || oh != out_h_px)
                ui_relayout(m, true);
        }
        break;
    case SDL_WINDOWEVENT_CLOSE:
        quit_requested = true;
        break;
    default:
        break;
    }
}

void ui_update(machine_t *m)
{
    ui_cmd_t cmd;

    if (!m->sdl_window)
        return;

    uint32_t now = SDL_GetTicks();
    if (touch_release_pending &&
        ui_tick_reached(now, touch_release_due_tick)) {
        ui_release_touch(m);
    }
    if (button_release_pending &&
        ui_tick_reached(now, button_release_due_tick)) {
        ui_release_pointer_button(m);
    }

    /* Poll input every emulator batch; only rendering is frame-limited. */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            quit_requested = true;
            return;

        case SDL_KEYDOWN:
            if (ui_shortcut_key(m, &ev.key))
                break;
            if ((ev.key.keysym.mod & KMOD_GUI) == 0 &&
                ui_stowaway_key_event(m, &ev.key, false))
                break;
            ui_button_key_event(m, &ev.key, false);
            break;

        case SDL_KEYUP:
            /* Release a forwarded key even if GUI was pressed after key-down. */
            if (!ui_stowaway_key_event(m, &ev.key, true))
                ui_button_key_event(m, &ev.key, true);
            break;

        case SDL_WINDOWEVENT:
            ui_handle_window_event(m, &ev.window);
            break;

        case SDL_DROPFILE:
            if (ev.drop.file) {
                ui_offer_install(m, ev.drop.file);
                SDL_free(ev.drop.file);
            }
            break;

        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.button == SDL_BUTTON_RIGHT && !mouse_button_held) {
                ui_platform_context_menu((SDL_Window *)m->sdl_window,
                    ev.button.x, ev.button.y);
                break;
            }
            if (ev.button.button == SDL_BUTTON_LEFT) {
                uint32_t ev_tick = ev.button.timestamp ?
                    ev.button.timestamp : now;
                uint8_t btn_set1 = 0;
                uint8_t btn_set2 = 0;

                if (layout.bezel &&
                    ui_frame_hit_button(m, ev.button.x, ev.button.y,
                        &btn_set1, &btn_set2)) {
                    mouse_button_held = true;
                    ui_press_pointer_button(m, btn_set1, btn_set2, ev_tick);
                    SDL_CaptureMouse(SDL_TRUE);
                    break;
                }

                if (touch_active) {
                    mouse_button_held = true;
                    pointer_mode = UI_POINTER_TOUCH;
                    ui_coalesce_touch_position(m, ev.button.x, ev.button.y);
                    touch_release_pending = false;
                    SDL_CaptureMouse(SDL_TRUE);
                } else if (ui_begin_touch(m, ev.button.x, ev.button.y,
                    ev_tick)) {
                    mouse_button_held = true;
                    pointer_mode = UI_POINTER_TOUCH;
                    SDL_CaptureMouse(SDL_TRUE);
                }
            }
            break;

        case SDL_MOUSEBUTTONUP:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                uint32_t ev_tick = ev.button.timestamp ?
                    ev.button.timestamp : now;

                mouse_button_held = false;
                if (pointer_mode == UI_POINTER_BUTTON) {
                    ui_schedule_pointer_button_release(m, ev_tick);
                } else if (touch_active) {
                    ui_schedule_touch_release(m, ev.button.x, ev.button.y,
                        ev_tick);
                    pointer_mode = UI_POINTER_NONE;
                }
                SDL_CaptureMouse(SDL_FALSE);
            }
            break;

        case SDL_MOUSEMOTION:
            if (mouse_button_held && pointer_mode == UI_POINTER_TOUCH) {
                if (touch_active) {
                    ui_coalesce_touch_position(m, ev.motion.x, ev.motion.y);
                }
            }
            break;

        default:
            break;
        }
        if (!m->sdl_window)
            return;
    }

    while (m->sdl_window && ui_cmd_next(&cmd))
        ui_run_command(m, cmd, now);
    if (!m->sdl_window)
        return;

    if (art_textures_dirty && ui_tick_reached(now, art_textures_due))
        ui_rebuild_art_textures(m);

    if (now - last_frame_tick < FRAME_INTERVAL_MS &&
        !screenshot_pending && !copy_pending)
        return;
    last_frame_tick = now;

    if (!ui_resolve_framebuffer(m))
        return;  /* dev_fb not yet initialized */

    ui_copy_visible_frame(m, staging_buf);
    SDL_UpdateTexture((SDL_Texture *)m->sdl_texture, NULL,
                      staging_buf, m->fb_width * sizeof(uint16_t));

    if (screenshot_pending || copy_pending) {
        bool copy = copy_pending;
        screenshot_pending = copy_pending = false;
        ui_take_user_screenshot(m, copy);
    }

    ui_draw_scene(m);
    SDL_RenderPresent((SDL_Renderer *)m->sdl_renderer);
    ui_platform_frame_presented((SDL_Window *)m->sdl_window, layout.bezel);
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
    ui_buttons_release_all(m);
    ui_stowaway_release_keys();
    if (touch_active)
        ui_release_touch(m);
    if (m->sdl_window) {
        ui_remember_geometry(m);
        ui_prefs_save();
    }
    ui_cmd_set_emulator_active(false);
    ui_cmd_clear();
    ui_video_destroy(m);
    ui_audio_destroy();
    /* SDL_Quit() shuts down every subsystem and tears down per-process state
     * (event handlers etc.). Skip it when something upstream still holds
     * subsystems open — e.g., the launcher keeps SDL_INIT_VIDEO live across
     * launcher↔emulator transitions. */
    if (SDL_WasInit(0) == 0)
        SDL_Quit();
}

void ui_session_begin(void)
{
    /* Each VM starts from its own configuration; only restarts inside one
     * session inherit the live window. Position persists regardless. */
    if (saved_geom.valid) {
        saved_geom.zoom = 1.0;
        saved_geom.bezel = false;
    }
}

bool ui_take_restart_request(char *package_out, size_t cap)
{
    bool r = restart_requested;

    if (cap)
        package_out[0] = '\0';
    if (r && cap)
        snprintf(package_out, cap, "%s", pending_package);
    restart_requested = false;
    pending_package[0] = '\0';
    if (r) {
        /* The next ui_init keeps the window as the user left it. */
        prefs_loaded = true;
    }
    return r;
}

bool ui_window_settings(bool *bezel_out, double *zoom_out)
{
    if (!saved_geom.valid)
        return false;
    *bezel_out = saved_geom.bezel;
    *zoom_out = saved_geom.zoom;
    return true;
}

#else  /* !HAVE_SDL2 — headless stubs */

int  ui_init(machine_t *m)         { (void)m; return 0; }
void ui_update(machine_t *m)       { (void)m; }
void ui_destroy(machine_t *m)      { (void)m; }
bool ui_should_quit(machine_t *m)  { (void)m; return false; }
void ui_save_screenshot(machine_t *m) { (void)m; }
void ui_session_begin(void)        {}
bool ui_take_restart_request(char *package_out, size_t cap)
{ if (cap) package_out[0] = '\0'; return false; }
bool ui_window_settings(bool *bezel_out, double *zoom_out)
{ (void)bezel_out; (void)zoom_out; return false; }

#endif  /* HAVE_SDL2 */
