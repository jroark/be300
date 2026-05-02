/*
 *  ui.c — SDL2 display frontend for BE-300 framebuffer.
 *
 *  Reads the GXemul dev_fb backing buffer via memory_paddr_to_hostaddr()
 *  and blits it to an SDL2 window at the configured scale (~30 fps).
 *
 *  When HAVE_SDL2 is not defined, all functions are safe no-ops (headless).
 */

#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

static ui_frame_callback_t frame_cb = NULL;
static void *frame_cb_data = NULL;

#ifdef HAVE_SDL2

#include <SDL.h>
#include <SDL_shape.h>
#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <SDL_syswm.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#endif
#ifdef HAVE_PNG
#include <png.h>
#endif

/* GXemul headers for framebuffer access */
#include "machine.h"
#include "devices.h"
#include "stowaway.h"

/* Quit flag */
static bool quit_requested = false;
static bool sdl_video_initialized = false;
static bool sdl_audio_initialized = false;
static SDL_AudioDeviceID audio_device = 0;
static SDL_Texture *frame_texture = NULL;
static bool frame_enabled = false;
static uint32_t frame_width = 0;
static uint32_t frame_height = 0;
static double frame_scale_x = 1.0;
static double frame_scale_y = 1.0;
static double frame_dst_x = 0.0;
static double frame_dst_y = 0.0;
static double frame_dst_w = 0.0;
static double frame_dst_h = 0.0;
static uint32_t frame_trim_x = 0;
static uint32_t frame_trim_y = 0;
static SDL_Rect frame_lcd_rect;
static SDL_Rect frame_icon_rect;
static SDL_Rect lcd_dst_rect;

typedef enum {
    UI_BUTTON_REGION_NONE = 0,
    UI_BUTTON_REGION_DPAD,
    UI_BUTTON_REGION_ROCKET,
    UI_BUTTON_REGION_POWER,
    UI_BUTTON_REGION_OK,
    UI_BUTTON_REGION_ESC,
    UI_BUTTON_REGION_COUNT
} ui_button_region_kind_t;

typedef struct {
    bool valid;
    uint32_t area;
    uint32_t min_x;
    uint32_t min_y;
    uint32_t max_x;
    uint32_t max_y;
    double cx;
    double cy;
} ui_button_region_t;

typedef struct {
    uint32_t area;
    uint32_t min_x;
    uint32_t min_y;
    uint32_t max_x;
    uint32_t max_y;
    double cx;
    double cy;
} ui_button_component_t;

static uint8_t *button_mask_pixels = NULL;
static uint32_t button_mask_width = 0;
static uint32_t button_mask_height = 0;
static ui_button_region_t button_regions[UI_BUTTON_REGION_COUNT];

/* Frame rate limiting */
#define FRAME_INTERVAL_MS 33  /* ~30 fps */
#define TOUCH_MIN_DWELL_DEFAULT_MS 120
#define TOUCH_MIN_DWELL_MAX_MS 5000
#define BUTTON_MIN_DWELL_DEFAULT_MS 120
#define BUTTON_MIN_DWELL_MAX_MS 5000
#define BE300_FRAME_LCD_SCALE_DEFAULT 1.0
#define BE300_FRAME_LCD_SCALE_MAX 4.0
#define UI_BUTTON_MASK_THRESHOLD 128u
#define UI_BUTTON_MASK_MIN_AREA 500u
#define UI_BUTTON_MASK_MAX_COMPONENTS 16u

static const SDL_Rect fallback_frame_lcd_rect = { 187, 218, 644, 859 };

typedef enum {
    UI_POINTER_NONE = 0,
    UI_POINTER_TOUCH,
    UI_POINTER_BUTTON
} ui_pointer_mode_t;

/* Mouse button held state for touch drag tracking */
static bool mouse_button_held = false;
static ui_pointer_mode_t pointer_mode = UI_POINTER_NONE;
static uint8_t pointer_btn_set1 = 0;
static uint8_t pointer_btn_set2 = 0;
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

static void ui_reset_button_regions(void)
{
    memset(button_regions, 0, sizeof(button_regions));
}

static void ui_free_button_mask(void)
{
    free(button_mask_pixels);
    button_mask_pixels = NULL;
    button_mask_width = 0;
    button_mask_height = 0;
    ui_reset_button_regions();
}

static void ui_video_destroy(machine_t *m)
{
    ui_free_button_mask();
    if (frame_texture) {
        SDL_DestroyTexture(frame_texture);
        frame_texture = NULL;
    }
    frame_enabled = false;
    frame_width = 0;
    frame_height = 0;
    frame_scale_x = 1.0;
    frame_scale_y = 1.0;
    frame_dst_x = 0.0;
    frame_dst_y = 0.0;
    frame_dst_w = 0.0;
    frame_dst_h = 0.0;
    frame_trim_x = 0;
    frame_trim_y = 0;
    memset(&frame_lcd_rect, 0, sizeof(frame_lcd_rect));
    memset(&frame_icon_rect, 0, sizeof(frame_icon_rect));
    memset(&lcd_dst_rect, 0, sizeof(lcd_dst_rect));
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

static double ui_scale(machine_t *m)
{
    double s = m->cfg.scale;

    if (!(s >= 1.0))
        return BE300_FRAME_LCD_SCALE_DEFAULT;
    if (s > BE300_FRAME_LCD_SCALE_MAX)
        return BE300_FRAME_LCD_SCALE_MAX;
    return s;
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
typedef struct {
    const uint8_t *cur;
    size_t remaining;
} ui_png_mem_reader_t;

static void ui_png_read_from_memory(png_structp png, png_bytep dst, size_t len)
{
    ui_png_mem_reader_t *r = (ui_png_mem_reader_t *)png_get_io_ptr(png);

    if (!r || r->remaining < len) {
        png_error(png, "ui_png_read_from_memory: short read");
        return;
    }
    memcpy(dst, r->cur, len);
    r->cur += len;
    r->remaining -= len;
}

static bool ui_load_png_rgba(const uint8_t *data, size_t len,
    uint8_t **pixels_out, uint32_t *width_out, uint32_t *height_out)
{
    ui_png_mem_reader_t reader;
    png_structp png = NULL;
    png_infop info = NULL;
    png_bytep *rows = NULL;
    uint8_t *pixels = NULL;
    png_uint_32 width, height;
    int bit_depth, color_type;
    size_t rowbytes;
    bool ok = false;

    if (!data || len < 8 || png_sig_cmp((png_bytep)data, 0, 8) != 0)
        return false;

    reader.cur = data + 8;
    reader.remaining = len - 8;

    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png)
        goto out;
    info = png_create_info_struct(png);
    if (!info)
        goto out;

    if (setjmp(png_jmpbuf(png)))
        goto out;

    png_set_read_fn(png, &reader, ui_png_read_from_memory);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);

    width = png_get_image_width(png, info);
    height = png_get_image_height(png, info);
    bit_depth = png_get_bit_depth(png, info);
    color_type = png_get_color_type(png, info);

    if (bit_depth == 16)
        png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    if (!(color_type & PNG_COLOR_MASK_ALPHA))
        png_set_filler(png, 0xff, PNG_FILLER_AFTER);

    png_read_update_info(png, info);
    rowbytes = png_get_rowbytes(png, info);
    if (width == 0 || height == 0 || rowbytes < width * 4u)
        goto out;

    pixels = malloc((size_t)width * (size_t)height * 4u);
    rows = malloc((size_t)height * sizeof(*rows));
    if (!pixels || !rows)
        goto out;

    for (png_uint_32 y = 0; y < height; y++)
        rows[y] = pixels + (size_t)y * (size_t)width * 4u;

    if (rowbytes == width * 4u) {
        png_read_image(png, rows);
    } else {
        uint8_t *tmp = malloc(rowbytes * (size_t)height);
        if (!tmp)
            goto out;
        for (png_uint_32 y = 0; y < height; y++)
            rows[y] = tmp + (size_t)y * rowbytes;
        png_read_image(png, rows);
        for (png_uint_32 y = 0; y < height; y++) {
            memcpy(pixels + (size_t)y * (size_t)width * 4u,
                   tmp + (size_t)y * rowbytes,
                   (size_t)width * 4u);
        }
        free(tmp);
    }
    png_read_end(png, NULL);

    *pixels_out = pixels;
    *width_out = (uint32_t)width;
    *height_out = (uint32_t)height;
    pixels = NULL;
    ok = true;

out:
    free(rows);
    free(pixels);
    if (png || info)
        png_destroy_read_struct(&png, &info, NULL);
    return ok;
}

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

static int ui_button_component_area_desc(const void *ap, const void *bp)
{
    const ui_button_component_t *a = (const ui_button_component_t *)ap;
    const ui_button_component_t *b = (const ui_button_component_t *)bp;

    if (a->area < b->area)
        return 1;
    if (a->area > b->area)
        return -1;
    return 0;
}

static void ui_insert_button_component(ui_button_component_t *components,
    size_t *count_io, const ui_button_component_t *component)
{
    size_t count = *count_io;

    if (component->area < UI_BUTTON_MASK_MIN_AREA)
        return;

    if (count < UI_BUTTON_MASK_MAX_COMPONENTS) {
        components[count] = *component;
        *count_io = count + 1u;
        return;
    }

    size_t smallest = 0;
    for (size_t i = 1; i < count; i++) {
        if (components[i].area < components[smallest].area)
            smallest = i;
    }
    if (component->area > components[smallest].area)
        components[smallest] = *component;
}

static bool ui_collect_button_components(const uint8_t *active,
    uint32_t width, uint32_t height, ui_button_component_t *components,
    size_t *count_out)
{
    size_t pixel_count = (size_t)width * (size_t)height;
    uint8_t *seen = NULL;
    uint32_t *queue = NULL;
    size_t count = 0;

    *count_out = 0;
    if (!active || width == 0 || height == 0)
        return false;

    seen = calloc(pixel_count, 1);
    queue = malloc(pixel_count * sizeof(*queue));
    if (!seen || !queue) {
        free(seen);
        free(queue);
        return false;
    }

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t start = y * width + x;
            uint32_t head = 0, tail = 0, area = 0;
            uint32_t min_x = x, min_y = y, max_x = x, max_y = y;
            uint64_t sum_x = 0, sum_y = 0;

            if (!active[start] || seen[start])
                continue;

            seen[start] = 1;
            queue[tail++] = start;

            while (head < tail) {
                uint32_t p = queue[head++];
                uint32_t px = p % width;
                uint32_t py = p / width;
                uint32_t y0 = py > 0 ? py - 1u : py;
                uint32_t y1 = py + 1u < height ? py + 1u : py;
                uint32_t x0 = px > 0 ? px - 1u : px;
                uint32_t x1 = px + 1u < width ? px + 1u : px;

                area++;
                sum_x += px;
                sum_y += py;
                if (px < min_x) min_x = px;
                if (px > max_x) max_x = px;
                if (py < min_y) min_y = py;
                if (py > max_y) max_y = py;

                for (uint32_t ny = y0; ny <= y1; ny++) {
                    for (uint32_t nx = x0; nx <= x1; nx++) {
                        uint32_t np;

                        if (nx == px && ny == py)
                            continue;
                        np = ny * width + nx;
                        if (!active[np] || seen[np])
                            continue;
                        seen[np] = 1;
                        queue[tail++] = np;
                    }
                }
            }

            if (area >= UI_BUTTON_MASK_MIN_AREA) {
                ui_button_component_t component = {
                    .area = area,
                    .min_x = min_x,
                    .min_y = min_y,
                    .max_x = max_x,
                    .max_y = max_y,
                    .cx = (double)sum_x / (double)area,
                    .cy = (double)sum_y / (double)area
                };
                ui_insert_button_component(components, &count,
                    &component);
            }
        }
    }

    free(seen);
    free(queue);

    if (count == 0)
        return false;

    qsort(components, count, sizeof(*components),
        ui_button_component_area_desc);
    *count_out = count;
    return true;
}

static void ui_region_from_component(ui_button_region_t *region,
    const ui_button_component_t *component)
{
    region->valid = true;
    region->area = component->area;
    region->min_x = component->min_x;
    region->min_y = component->min_y;
    region->max_x = component->max_x;
    region->max_y = component->max_y;
    region->cx = component->cx;
    region->cy = component->cy;
}

static void ui_split_side_component(const uint8_t *active, uint32_t width,
    const ui_button_component_t *component, bool left_side,
    ui_button_region_t *upper, ui_button_region_t *lower)
{
    double box_w = (double)(component->max_x - component->min_x + 1u);
    double box_h = (double)(component->max_y - component->min_y + 1u);
    double upper_x = (double)component->min_x +
        (left_side ? 0.25 : 0.75) * box_w;
    double upper_y = (double)component->min_y + 0.30 * box_h;
    double lower_x = (double)component->min_x +
        (left_side ? 0.75 : 0.25) * box_w;
    double lower_y = (double)component->min_y + 0.82 * box_h;

    for (int iter = 0; iter < 8; iter++) {
        double upper_sum_x = 0.0, upper_sum_y = 0.0;
        double lower_sum_x = 0.0, lower_sum_y = 0.0;
        uint32_t upper_count = 0, lower_count = 0;

        for (uint32_t y = component->min_y; y <= component->max_y; y++) {
            for (uint32_t x = component->min_x; x <= component->max_x; x++) {
                double du, dl;

                if (!active[(size_t)y * width + x])
                    continue;

                du = ((double)x - upper_x) * ((double)x - upper_x) +
                    ((double)y - upper_y) * ((double)y - upper_y);
                dl = ((double)x - lower_x) * ((double)x - lower_x) +
                    ((double)y - lower_y) * ((double)y - lower_y);
                if (du <= dl) {
                    upper_sum_x += x;
                    upper_sum_y += y;
                    upper_count++;
                } else {
                    lower_sum_x += x;
                    lower_sum_y += y;
                    lower_count++;
                }
            }
        }

        if (upper_count != 0) {
            upper_x = upper_sum_x / (double)upper_count;
            upper_y = upper_sum_y / (double)upper_count;
        }
        if (lower_count != 0) {
            lower_x = lower_sum_x / (double)lower_count;
            lower_y = lower_sum_y / (double)lower_count;
        }
    }

    ui_region_from_component(upper, component);
    ui_region_from_component(lower, component);
    upper->cx = upper_x;
    upper->cy = upper_y;
    lower->cx = lower_x;
    lower->cy = lower_y;
}

static bool ui_assign_button_regions(const ui_button_component_t *components,
    size_t count, const uint8_t *active, uint32_t width,
    ui_button_region_t *regions_out)
{
    int left_upper = -1, left_lower = -1;
    int right_upper = -1, right_lower = -1;
    size_t left_count = 0, right_count = 0;
    const ui_button_component_t *dpad;

    memset(regions_out, 0, sizeof(button_regions));
    if (!components || count < 3 || !active || width == 0)
        return false;

    dpad = &components[0];
    if (count < 5) {
        int left_side = -1, right_side = -1;

        for (size_t i = 1; i < count; i++) {
            if (components[i].cx < dpad->cx) {
                if (left_side < 0 ||
                    components[i].area > components[left_side].area)
                    left_side = (int)i;
            } else {
                if (right_side < 0 ||
                    components[i].area > components[right_side].area)
                    right_side = (int)i;
            }
        }

        if (left_side < 0 || right_side < 0)
            return false;

        ui_region_from_component(&regions_out[UI_BUTTON_REGION_DPAD],
            dpad);
        ui_split_side_component(active, width, &components[left_side],
            true, &regions_out[UI_BUTTON_REGION_ROCKET],
            &regions_out[UI_BUTTON_REGION_OK]);
        ui_split_side_component(active, width, &components[right_side],
            false, &regions_out[UI_BUTTON_REGION_POWER],
            &regions_out[UI_BUTTON_REGION_ESC]);
        return true;
    }

    for (size_t i = 1; i < count; i++) {
        const ui_button_component_t *component = &components[i];

        if (component->cx < dpad->cx) {
            left_count++;
            if (left_upper < 0 ||
                component->cy < components[left_upper].cy)
                left_upper = (int)i;
            if (left_lower < 0 ||
                component->cy > components[left_lower].cy)
                left_lower = (int)i;
        } else {
            right_count++;
            if (right_upper < 0 ||
                component->cy < components[right_upper].cy)
                right_upper = (int)i;
            if (right_lower < 0 ||
                component->cy > components[right_lower].cy)
                right_lower = (int)i;
        }
    }

    if (left_count < 2 || right_count < 2 ||
        left_upper < 0 || left_lower < 0 ||
        right_upper < 0 || right_lower < 0 ||
        left_upper == left_lower || right_upper == right_lower)
        return false;

    ui_region_from_component(&regions_out[UI_BUTTON_REGION_DPAD], dpad);
    ui_region_from_component(&regions_out[UI_BUTTON_REGION_ROCKET],
        &components[left_upper]);
    ui_region_from_component(&regions_out[UI_BUTTON_REGION_OK],
        &components[left_lower]);
    ui_region_from_component(&regions_out[UI_BUTTON_REGION_POWER],
        &components[right_upper]);
    ui_region_from_component(&regions_out[UI_BUTTON_REGION_ESC],
        &components[right_lower]);
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
    size_t pixel_count = (size_t)width * (size_t)height;
    uint8_t *active = NULL;
    ui_button_component_t components[UI_BUTTON_MASK_MAX_COMPONENTS];
    ui_button_region_t regions[UI_BUTTON_REGION_COUNT];
    size_t component_count = 0;

    if (!rgba || width == 0 || height == 0)
        return false;

    active = malloc(pixel_count);
    if (!active)
        return false;

    for (size_t i = 0; i < pixel_count; i++) {
        const uint8_t *p = rgba + i * 4u;
        uint32_t luma = (uint32_t)p[0] * 77u +
            (uint32_t)p[1] * 150u + (uint32_t)p[2] * 29u;

        active[i] = (p[3] >= 16u &&
            (luma >> 8) >= UI_BUTTON_MASK_THRESHOLD) ? 1u : 0u;
    }

    if (!ui_collect_button_components(active, width, height, components,
        &component_count) ||
        !ui_assign_button_regions(components, component_count, active,
            width, regions)) {
        free(active);
        return false;
    }

    ui_free_button_mask();
    button_mask_pixels = active;
    button_mask_width = width;
    button_mask_height = height;
    memcpy(button_regions, regions, sizeof(button_regions));

    fprintf(stderr, "[UI] Loaded button mask: %s (%ux%u)\n", path,
        width, height);
    ui_log_button_region("dpad", &button_regions[UI_BUTTON_REGION_DPAD]);
    ui_log_button_region("rocket", &button_regions[UI_BUTTON_REGION_ROCKET]);
    ui_log_button_region("power", &button_regions[UI_BUTTON_REGION_POWER]);
    ui_log_button_region("ok", &button_regions[UI_BUTTON_REGION_OK]);
    ui_log_button_region("esc", &button_regions[UI_BUTTON_REGION_ESC]);
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

static void ui_set_default_frame_rects(void)
{
    frame_lcd_rect = fallback_frame_lcd_rect;
    frame_icon_rect.x = frame_lcd_rect.x;
    frame_icon_rect.y = frame_lcd_rect.y + frame_lcd_rect.h;
    frame_icon_rect.w = frame_lcd_rect.w;
    frame_icon_rect.h = frame_lcd_rect.h / 8;
}

static void ui_set_icon_rect_from_lcd(void)
{
    int icon_h;

    frame_icon_rect.x = frame_lcd_rect.x;
    frame_icon_rect.y = frame_lcd_rect.y + frame_lcd_rect.h;
    frame_icon_rect.w = frame_lcd_rect.w;
    icon_h = frame_lcd_rect.h / 8;
    if (icon_h < 1)
        icon_h = 1;
    if (frame_icon_rect.y + icon_h > (int)frame_height)
        icon_h = (int)frame_height - frame_icon_rect.y;
    if (icon_h < 1)
        icon_h = 1;
    frame_icon_rect.h = icon_h;
}

static bool ui_detect_frame_lcd_rect(const uint8_t *pixels, uint32_t width,
    uint32_t height)
{
    uint8_t *seen;
    uint32_t *queue;
    uint32_t best_area = 0;
    SDL_Rect best = { 0, 0, 0, 0 };

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
                    best.x = (int)min_x;
                    best.y = (int)min_y;
                    best.w = (int)(max_x - min_x + 1u);
                    best.h = (int)(max_y - min_y + 1u);
                }
            }
        }
    }

    free(seen);
    free(queue);

    if (best_area == 0)
        return false;

    frame_lcd_rect = best;
    ui_set_icon_rect_from_lcd();
    fprintf(stderr, "[UI] Frame LCD aperture: x=%d y=%d w=%d h=%d\n",
        frame_lcd_rect.x, frame_lcd_rect.y,
        frame_lcd_rect.w, frame_lcd_rect.h);
    return true;
}

static void ui_configure_frame_layout(machine_t *m, double lcd_scale,
    int *window_w_out, int *window_h_out)
{
    uint32_t lcd_w = (uint32_t)floor((double)m->fb_width * lcd_scale + 0.5);
    uint32_t lcd_h = (uint32_t)floor((double)m->fb_height * lcd_scale + 0.5);

    if (lcd_w == 0)
        lcd_w = 1;
    if (lcd_h == 0)
        lcd_h = 1;

    frame_scale_x = (double)lcd_w / (double)frame_lcd_rect.w;
    frame_scale_y = (double)lcd_h / (double)frame_lcd_rect.h;

    lcd_dst_rect.x = (int)ceil((double)frame_lcd_rect.x * frame_scale_x);
    lcd_dst_rect.y = (int)ceil((double)frame_lcd_rect.y * frame_scale_y);
    lcd_dst_rect.w = (int)lcd_w;
    lcd_dst_rect.h = (int)lcd_h;

    frame_dst_x = (double)lcd_dst_rect.x -
        (double)frame_lcd_rect.x * frame_scale_x;
    frame_dst_y = (double)lcd_dst_rect.y -
        (double)frame_lcd_rect.y * frame_scale_y;
    frame_dst_w = (double)frame_width * frame_scale_x;
    frame_dst_h = (double)frame_height * frame_scale_y;

    *window_w_out = (int)ceil(frame_dst_x + frame_dst_w);
    *window_h_out = (int)ceil(frame_dst_y + frame_dst_h);
}

static uint8_t *ui_create_frame_window_mask(const uint8_t *frame_pixels,
    int win_w, int win_h)
{
    uint8_t *mask;

    if (!frame_pixels || win_w <= 0 || win_h <= 0)
        return NULL;

    mask = malloc((size_t)win_w * (size_t)win_h);
    if (!mask)
        return NULL;

    for (int y = 0; y < win_h; y++) {
        for (int x = 0; x < win_w; x++) {
            uint8_t alpha = 0;

            if (x >= lcd_dst_rect.x && y >= lcd_dst_rect.y &&
                x < lcd_dst_rect.x + lcd_dst_rect.w &&
                y < lcd_dst_rect.y + lcd_dst_rect.h) {
                mask[(size_t)y * (size_t)win_w + (size_t)x] = 255;
                continue;
            }

            double fx = ((double)x - frame_dst_x) / frame_scale_x;
            double fy = ((double)y - frame_dst_y) / frame_scale_y;
            int sx = (int)floor(fx);
            int sy = (int)floor(fy);

            if (sx >= 0 && sy >= 0 &&
                (uint32_t)sx < frame_width &&
                (uint32_t)sy < frame_height) {
                size_t off = ((size_t)sy * (size_t)frame_width +
                    (size_t)sx) * 4u;
                alpha = frame_pixels[off + 3u];
            }

            mask[(size_t)y * (size_t)win_w + (size_t)x] =
                alpha >= 16 ? 255 : 0;
        }
    }

    return mask;
}

static bool ui_apply_frame_window_shape(SDL_Window *win,
    const uint8_t *window_mask, int win_w, int win_h)
{
    SDL_Surface *shape;
    SDL_WindowShapeMode mode;
    uint32_t transparent;
    uint32_t opaque;
    bool ok = false;

    if (!win || !window_mask || !SDL_IsShapedWindow(win))
        return false;

    shape = SDL_CreateRGBSurfaceWithFormat(0, win_w, win_h, 32,
        SDL_PIXELFORMAT_RGBA32);
    if (!shape)
        return false;

    transparent = SDL_MapRGBA(shape->format, 0, 0, 0, 0);
    opaque = SDL_MapRGBA(shape->format, 255, 255, 255, 255);

    if (SDL_LockSurface(shape) == 0) {
        for (int y = 0; y < win_h; y++) {
            uint32_t *dst = (uint32_t *)((uint8_t *)shape->pixels +
                (size_t)y * (size_t)shape->pitch);

            for (int x = 0; x < win_w; x++) {
                uint8_t alpha = window_mask[(size_t)y * (size_t)win_w +
                    (size_t)x];
                dst[x] = alpha ? opaque : transparent;
            }
        }
        SDL_UnlockSurface(shape);

        mode.mode = ShapeModeBinarizeAlpha;
        mode.parameters.binarizationCutoff = 1;
        ok = SDL_SetWindowShape(win, shape, &mode) == 0;
    }

    SDL_FreeSurface(shape);
    if (!ok)
        fprintf(stderr, "[UI] SDL window shape failed: %s\n",
            SDL_GetError());
    return ok;
}

#ifdef __APPLE__
static void ui_macos_release_cg_data(void *info, const void *data,
    size_t size)
{
    (void)info;
    (void)size;
    free((void *)data);
}

static void ui_macos_make_window_transparent(SDL_Window *win)
{
    SDL_SysWMinfo info;
    Class ns_color_class;
    id ns_window;
    id content_view;
    id layer;
    id clear_color;
    id clear_cg_color = NULL;

    if (!win)
        return;

    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) ||
        info.subsystem != SDL_SYSWM_COCOA)
        return;

    ns_window = (id)info.info.cocoa.window;
    if (!ns_window)
        return;

    ((void (*)(id, SEL, BOOL))objc_msgSend)(ns_window,
        sel_registerName("setOpaque:"), (BOOL)0);

    ns_color_class = (Class)objc_getClass("NSColor");
    if (ns_color_class) {
        clear_color = ((id (*)(Class, SEL))objc_msgSend)(ns_color_class,
            sel_registerName("clearColor"));
        if (clear_color) {
            ((void (*)(id, SEL, id))objc_msgSend)(ns_window,
                sel_registerName("setBackgroundColor:"), clear_color);
            clear_cg_color = ((id (*)(id, SEL))objc_msgSend)(clear_color,
                sel_registerName("CGColor"));
        }
    }

    content_view = ((id (*)(id, SEL))objc_msgSend)(ns_window,
        sel_registerName("contentView"));
    if (!content_view)
        return;

    ((void (*)(id, SEL, BOOL))objc_msgSend)(content_view,
        sel_registerName("setWantsLayer:"), (BOOL)1);
    layer = ((id (*)(id, SEL))objc_msgSend)(content_view,
        sel_registerName("layer"));
    if (!layer)
        return;

    ((void (*)(id, SEL, BOOL))objc_msgSend)(layer,
        sel_registerName("setOpaque:"), (BOOL)0);
    if (clear_cg_color) {
        ((void (*)(id, SEL, id))objc_msgSend)(layer,
            sel_registerName("setBackgroundColor:"), clear_cg_color);
    }
}

static void ui_macos_apply_window_mask(SDL_Window *win,
    const uint8_t *window_mask, int win_w, int win_h)
{
    SDL_SysWMinfo info;
    Class ca_layer_class;
    id ns_window;
    id content_view;
    id layer;
    id mask_layer;
    CGColorSpaceRef color_space = NULL;
    CGDataProviderRef provider = NULL;
    CGImageRef image = NULL;
    uint8_t *rgba = NULL;
    size_t pixels;
    CGRect bounds;

    if (!win || !window_mask || win_w <= 0 || win_h <= 0)
        return;

    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) ||
        info.subsystem != SDL_SYSWM_COCOA)
        return;

    ns_window = (id)info.info.cocoa.window;
    if (!ns_window)
        return;

    content_view = ((id (*)(id, SEL))objc_msgSend)(ns_window,
        sel_registerName("contentView"));
    if (!content_view)
        return;

    ((void (*)(id, SEL, BOOL))objc_msgSend)(content_view,
        sel_registerName("setWantsLayer:"), (BOOL)1);
    layer = ((id (*)(id, SEL))objc_msgSend)(content_view,
        sel_registerName("layer"));
    if (!layer)
        return;

    pixels = (size_t)win_w * (size_t)win_h;
    rgba = malloc(pixels * 4u);
    if (!rgba)
        return;
    for (size_t i = 0; i < pixels; i++) {
        rgba[i * 4u + 0u] = 255;
        rgba[i * 4u + 1u] = 255;
        rgba[i * 4u + 2u] = 255;
        rgba[i * 4u + 3u] = window_mask[i];
    }

    color_space = CGColorSpaceCreateDeviceRGB();
    if (!color_space)
        goto out;
    provider = CGDataProviderCreateWithData(NULL, rgba, pixels * 4u,
        ui_macos_release_cg_data);
    if (!provider)
        goto out;
    rgba = NULL;  /* owned by provider */

    image = CGImageCreate((size_t)win_w, (size_t)win_h, 8, 32,
        (size_t)win_w * 4u, color_space,
        kCGBitmapByteOrder32Big | kCGImageAlphaLast,
        provider, NULL, false, kCGRenderingIntentDefault);
    if (!image)
        goto out;

    ca_layer_class = (Class)objc_getClass("CALayer");
    if (!ca_layer_class)
        goto out;
    mask_layer = ((id (*)(Class, SEL))objc_msgSend)(ca_layer_class,
        sel_registerName("layer"));
    if (!mask_layer)
        goto out;

    bounds = CGRectMake(0.0, 0.0, (CGFloat)win_w, (CGFloat)win_h);
    ((void (*)(id, SEL, CGRect))objc_msgSend)(mask_layer,
        sel_registerName("setFrame:"), bounds);
    ((void (*)(id, SEL, id))objc_msgSend)(mask_layer,
        sel_registerName("setContents:"), (id)image);
    ((void (*)(id, SEL, BOOL))objc_msgSend)(layer,
        sel_registerName("setMasksToBounds:"), (BOOL)1);
    ((void (*)(id, SEL, id))objc_msgSend)(layer,
        sel_registerName("setMask:"), mask_layer);

out:
    free(rgba);
    if (image)
        CGImageRelease(image);
    if (provider)
        CGDataProviderRelease(provider);
    if (color_space)
        CGColorSpaceRelease(color_space);
}
#else
static void ui_macos_make_window_transparent(SDL_Window *win)
{
    (void)win;
}

static void ui_macos_apply_window_mask(SDL_Window *win,
    const uint8_t *window_mask, int win_w, int win_h)
{
    (void)win;
    (void)window_mask;
    (void)win_w;
    (void)win_h;
}
#endif

static bool ui_window_to_frame(machine_t *m, int win_x, int win_y,
    double *frame_x_out, double *frame_y_out)
{
    (void)m;
    if (!frame_enabled || frame_scale_x <= 0.0 || frame_scale_y <= 0.0)
        return false;

    *frame_x_out = ((double)win_x - frame_dst_x) / frame_scale_x;
    *frame_y_out = ((double)win_y - frame_dst_y) / frame_scale_y;
    return true;
}

static bool ui_frame_rect_to_touch(double fx, double fy, const SDL_Rect *rect,
    uint32_t y_base, uint32_t y_span, uint16_t *x_out, uint16_t *y_out)
{
    int tx, ty;

    if (fx < rect->x || fy < rect->y ||
        fx >= rect->x + rect->w || fy >= rect->y + rect->h)
        return false;

    tx = (int)(((fx - rect->x) * 240.0) / rect->w);
    ty = (int)(y_base + (((fy - rect->y) * (double)y_span) / rect->h));
    if (tx < 0) tx = 0;
    if (ty < 0) ty = 0;
    if (tx > 239) tx = 239;
    if (ty > 359) ty = 359;

    *x_out = (uint16_t)tx;
    *y_out = (uint16_t)ty;
    return true;
}

static bool ui_window_to_touch(machine_t *m, int win_x, int win_y,
    uint16_t *x_out, uint16_t *y_out)
{
    int win_w, win_h;
    int tx, ty;

    if (frame_enabled) {
        double fx, fy;
        if (!ui_window_to_frame(m, win_x, win_y, &fx, &fy))
            return false;
        if (ui_frame_rect_to_touch(fx, fy, &frame_lcd_rect, 0, 320,
            x_out, y_out))
            return true;
        if (ui_frame_rect_to_touch(fx, fy, &frame_icon_rect, 320, 40,
            x_out, y_out))
            return true;
        return false;
    }

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
    return true;
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

static bool ui_point_in_ellipse(double x, double y, double cx, double cy,
    double rx, double ry)
{
    double dx = (x - cx) / rx;
    double dy = (y - cy) / ry;

    return dx * dx + dy * dy <= 1.0;
}

static bool ui_frame_point_in_rect(double fx, double fy, int x, int y,
    int w, int h)
{
    return fx >= x && fy >= y && fx < x + w && fy < y + h;
}

static bool ui_button_region_contains(const ui_button_region_t *region,
    uint32_t x, uint32_t y)
{
    return region->valid &&
        x >= region->min_x && x <= region->max_x &&
        y >= region->min_y && y <= region->max_y;
}

static double ui_button_region_distance_sq(const ui_button_region_t *region,
    uint32_t x, uint32_t y)
{
    double dx = (double)x + 0.5 - region->cx;
    double dy = (double)y + 0.5 - region->cy;

    return dx * dx + dy * dy;
}

static void ui_set_side_button_from_regions(
    const ui_button_region_t *upper, const ui_button_region_t *lower,
    uint32_t x, uint32_t y, uint8_t upper_btn_set1,
    uint8_t upper_btn_set2, uint8_t lower_btn_set1,
    uint8_t lower_btn_set2, uint8_t *btn_set1_out,
    uint8_t *btn_set2_out)
{
    bool use_upper = ui_button_region_distance_sq(upper, x, y) <=
        ui_button_region_distance_sq(lower, x, y);

    if (use_upper) {
        *btn_set1_out = upper_btn_set1;
        *btn_set2_out = upper_btn_set2;
    } else {
        *btn_set1_out = lower_btn_set1;
        *btn_set2_out = lower_btn_set2;
    }
}

static bool ui_frame_hit_button_mask(double bx, double by,
    uint8_t *btn_set1_out, uint8_t *btn_set2_out)
{
    int ix = (int)floor(bx);
    int iy = (int)floor(by);
    ui_button_region_kind_t kind = UI_BUTTON_REGION_NONE;
    const ui_button_region_t *region = NULL;

    if (!button_mask_pixels || ix < 0 || iy < 0 ||
        (uint32_t)ix >= button_mask_width ||
        (uint32_t)iy >= button_mask_height)
        return false;

    if (!button_mask_pixels[(size_t)iy * button_mask_width + (uint32_t)ix])
        return false;

    if (ui_button_region_contains(&button_regions[UI_BUTTON_REGION_DPAD],
        (uint32_t)ix, (uint32_t)iy)) {
        kind = UI_BUTTON_REGION_DPAD;
        region = &button_regions[UI_BUTTON_REGION_DPAD];
    } else if (ui_button_region_contains(
        &button_regions[UI_BUTTON_REGION_ROCKET], (uint32_t)ix,
        (uint32_t)iy) &&
        ui_button_region_contains(&button_regions[UI_BUTTON_REGION_OK],
        (uint32_t)ix, (uint32_t)iy)) {
        ui_set_side_button_from_regions(
            &button_regions[UI_BUTTON_REGION_ROCKET],
            &button_regions[UI_BUTTON_REGION_OK],
            (uint32_t)ix, (uint32_t)iy,
            0, 0x10u, 0x04u, 0, btn_set1_out, btn_set2_out);
        return true;
    } else if (ui_button_region_contains(
        &button_regions[UI_BUTTON_REGION_POWER], (uint32_t)ix,
        (uint32_t)iy) &&
        ui_button_region_contains(&button_regions[UI_BUTTON_REGION_ESC],
        (uint32_t)ix, (uint32_t)iy)) {
        ui_set_side_button_from_regions(
            &button_regions[UI_BUTTON_REGION_POWER],
            &button_regions[UI_BUTTON_REGION_ESC],
            (uint32_t)ix, (uint32_t)iy,
            0, 0x80u, 0x08u, 0, btn_set1_out, btn_set2_out);
        return true;
    } else {
        for (ui_button_region_kind_t i = UI_BUTTON_REGION_ROCKET;
            i < UI_BUTTON_REGION_COUNT; i++) {
            if (ui_button_region_contains(&button_regions[i],
                (uint32_t)ix, (uint32_t)iy)) {
                kind = i;
                region = &button_regions[i];
                break;
            }
        }
    }

    if (!region)
        return false;

    switch (kind) {
    case UI_BUTTON_REGION_DPAD: {
        double rx = ((double)(region->max_x - region->min_x + 1u)) / 2.0;
        double ry = ((double)(region->max_y - region->min_y + 1u)) / 2.0;
        double dx, dy;

        if (rx <= 0.0 || ry <= 0.0)
            return false;

        dx = ((double)ix + 0.5 - region->cx) / rx;
        dy = ((double)iy + 0.5 - region->cy) / ry;
        if (dx * dx + dy * dy < 0.18) {
            *btn_set1_out = 0x04u;  /* ok */
        } else if (fabs(dy) >= fabs(dx)) {
            *btn_set1_out = dy < 0.0 ? 0x10u : 0x20u;  /* up/down */
        } else {
            *btn_set1_out = dx > 0.0 ? 0x40u : 0x80u;  /* right/left */
        }
        return true;
    }
    case UI_BUTTON_REGION_ROCKET:
        *btn_set2_out = 0x10u;
        return true;
    case UI_BUTTON_REGION_POWER:
        *btn_set2_out = 0x80u;
        return true;
    case UI_BUTTON_REGION_OK:
        *btn_set1_out = 0x04u;
        return true;
    case UI_BUTTON_REGION_ESC:
        *btn_set1_out = 0x08u;
        return true;
    default:
        return false;
    }
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

    *btn_set1_out = 0;
    *btn_set2_out = 0;

    if (!ui_window_to_frame(m, win_x, win_y, &fx, &fy))
        return false;

    bx = fx + (double)frame_trim_x;
    by = fy + (double)frame_trim_y;

    if (button_mask_pixels)
        return ui_frame_hit_button_mask(bx, by, btn_set1_out,
            btn_set2_out);

    return ui_frame_hit_button_fallback(bx, by, btn_set1_out,
        btn_set2_out);
}

static bool ui_frame_hit_interactive(machine_t *m, int win_x, int win_y)
{
    double fx, fy;
    uint8_t btn_set1 = 0;
    uint8_t btn_set2 = 0;

    if (!ui_window_to_frame(m, win_x, win_y, &fx, &fy))
        return false;

    if (ui_frame_point_in_rect(fx, fy, frame_lcd_rect.x,
        frame_lcd_rect.y, frame_lcd_rect.w, frame_lcd_rect.h))
        return true;
    if (ui_frame_point_in_rect(fx, fy, frame_icon_rect.x,
        frame_icon_rect.y, frame_icon_rect.w, frame_icon_rect.h))
        return true;

    return ui_frame_hit_button(m, win_x, win_y, &btn_set1, &btn_set2);
}

static SDL_HitTestResult SDLCALL ui_window_hit_test(SDL_Window *win,
    const SDL_Point *area, void *data)
{
    machine_t *m = (machine_t *)data;

    (void)win;
    if (!frame_enabled)
        return SDL_HITTEST_NORMAL;
    if (ui_frame_hit_interactive(m, area->x, area->y))
        return SDL_HITTEST_NORMAL;
    return SDL_HITTEST_DRAGGABLE;
}

static bool ui_stowaway_lookup_key(SDL_Keycode key, unsigned *scancode_out)
{
    switch (key) {
    case SDLK_1: *scancode_out = 0; return true;
    case SDLK_2: *scancode_out = 1; return true;
    case SDLK_3: *scancode_out = 2; return true;
    case SDLK_z: *scancode_out = 3; return true;
    case SDLK_4: *scancode_out = 4; return true;
    case SDLK_5: *scancode_out = 5; return true;
    case SDLK_6: *scancode_out = 6; return true;
    case SDLK_7: *scancode_out = 7; return true;
    case SDLK_q: *scancode_out = 9; return true;
    case SDLK_w: *scancode_out = 10; return true;
    case SDLK_e: *scancode_out = 11; return true;
    case SDLK_r: *scancode_out = 12; return true;
    case SDLK_t: *scancode_out = 13; return true;
    case SDLK_y: *scancode_out = 14; return true;
    case SDLK_BACKQUOTE: *scancode_out = 15; return true;
    case SDLK_x: *scancode_out = 16; return true;
    case SDLK_a: *scancode_out = 17; return true;
    case SDLK_s: *scancode_out = 18; return true;
    case SDLK_d: *scancode_out = 19; return true;
    case SDLK_f: *scancode_out = 20; return true;
    case SDLK_g: *scancode_out = 21; return true;
    case SDLK_h: *scancode_out = 22; return true;
    case SDLK_SPACE: *scancode_out = 23; return true;
    case SDLK_CAPSLOCK: *scancode_out = 24; return true;
    case SDLK_TAB: *scancode_out = 25; return true;
    case SDLK_LCTRL:
    case SDLK_RCTRL: *scancode_out = 26; return true;
    case SDLK_LALT:
    case SDLK_RALT: *scancode_out = 35; return true;
    case SDLK_c: *scancode_out = 44; return true;
    case SDLK_v: *scancode_out = 45; return true;
    case SDLK_b: *scancode_out = 46; return true;
    case SDLK_n: *scancode_out = 47; return true;
    case SDLK_MINUS: *scancode_out = 48; return true;
    case SDLK_EQUALS: *scancode_out = 49; return true;
    case SDLK_BACKSPACE: *scancode_out = 50; return true;
    case SDLK_HOME: *scancode_out = 51; return true;
    case SDLK_8: *scancode_out = 52; return true;
    case SDLK_9: *scancode_out = 53; return true;
    case SDLK_0: *scancode_out = 54; return true;
    case SDLK_ESCAPE: *scancode_out = 55; return true;
    case SDLK_LEFTBRACKET: *scancode_out = 56; return true;
    case SDLK_RIGHTBRACKET: *scancode_out = 57; return true;
    case SDLK_BACKSLASH: *scancode_out = 58; return true;
    case SDLK_END: *scancode_out = 59; return true;
    case SDLK_u: *scancode_out = 60; return true;
    case SDLK_i: *scancode_out = 61; return true;
    case SDLK_o: *scancode_out = 62; return true;
    case SDLK_p: *scancode_out = 63; return true;
    case SDLK_QUOTE: *scancode_out = 64; return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: *scancode_out = 65; return true;
    case SDLK_PAGEUP: *scancode_out = 66; return true;
    case SDLK_j: *scancode_out = 68; return true;
    case SDLK_k: *scancode_out = 69; return true;
    case SDLK_l: *scancode_out = 70; return true;
    case SDLK_SEMICOLON: *scancode_out = 71; return true;
    case SDLK_SLASH: *scancode_out = 72; return true;
    case SDLK_UP: *scancode_out = 73; return true;
    case SDLK_PAGEDOWN: *scancode_out = 74; return true;
    case SDLK_m: *scancode_out = 76; return true;
    case SDLK_COMMA: *scancode_out = 77; return true;
    case SDLK_PERIOD: *scancode_out = 78; return true;
    case SDLK_INSERT: *scancode_out = 79; return true;
    case SDLK_DELETE: *scancode_out = 80; return true;
    case SDLK_LEFT: *scancode_out = 81; return true;
    case SDLK_DOWN: *scancode_out = 82; return true;
    case SDLK_RIGHT: *scancode_out = 83; return true;
    case SDLK_LSHIFT: *scancode_out = 87; return true;
    case SDLK_RSHIFT: *scancode_out = 88; return true;
    case SDLK_F1: *scancode_out = 105; return true;
    case SDLK_F2: *scancode_out = 106; return true;
    case SDLK_F3: *scancode_out = 107; return true;
    case SDLK_F4: *scancode_out = 108; return true;
    case SDLK_F5: *scancode_out = 109; return true;
    case SDLK_F6: *scancode_out = 110; return true;
    case SDLK_F7: *scancode_out = 111; return true;
    case SDLK_F8: *scancode_out = 112; return true;
    case SDLK_F9: *scancode_out = 113; return true;
    case SDLK_F10: *scancode_out = 114; return true;
    case SDLK_F11: *scancode_out = 115; return true;
    case SDLK_F12: *scancode_out = 116; return true;
    default:
        return false;
    }
}

static bool ui_stowaway_key_event(machine_t *m, const SDL_KeyboardEvent *key,
    bool release)
{
    static uint8_t key_refs[128];
    unsigned scancode;

    if (!m->cfg.enable_stowaway_keyboard)
        return false;
    if (!ui_stowaway_lookup_key(key->keysym.sym, &scancode))
        return false;

    if (release) {
        if (key_refs[scancode] == 0)
            return false;
        key_refs[scancode]--;
        if (key_refs[scancode] == 0)
            stowaway_queue_key(scancode, true);
        return true;
    }

    if (key->repeat)
        return true;

    if (key_refs[scancode] == 0)
        stowaway_queue_key(scancode, false);
    if (key_refs[scancode] != UINT8_MAX)
        key_refs[scancode]++;
    return true;
}

static void ui_press_pointer_button(machine_t *m, uint8_t btn_set1,
    uint8_t btn_set2, uint32_t now)
{
    if (pointer_mode == UI_POINTER_BUTTON) {
        be300_set_buttons(m, m->btn_set1 & (uint8_t)~pointer_btn_set1,
            m->btn_set2 & (uint8_t)~pointer_btn_set2);
    }

    pointer_btn_set1 = btn_set1;
    pointer_btn_set2 = btn_set2;
    pointer_mode = UI_POINTER_BUTTON;
    button_release_pending = false;
    button_down_tick = now;
    be300_set_buttons(m, m->btn_set1 | btn_set1, m->btn_set2 | btn_set2);
}

static void ui_release_pointer_button(machine_t *m)
{
    if (pointer_mode != UI_POINTER_BUTTON)
        return;

    be300_set_buttons(m, m->btn_set1 & (uint8_t)~pointer_btn_set1,
        m->btn_set2 & (uint8_t)~pointer_btn_set2);
    pointer_btn_set1 = 0;
    pointer_btn_set2 = 0;
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

int ui_init(machine_t *m)
{
    uint8_t *frame_pixels = NULL;
    uint32_t loaded_frame_width = 0;
    uint32_t loaded_frame_height = 0;
    uint8_t *window_mask = NULL;
    bool have_frame_pixels = false;

    m->fb_width  = 240;
    m->fb_height = 320;
    m->fb_stride = 256;

    quit_requested = false;
    frame_enabled = false;
    frame_width = 0;
    frame_height = 0;
    ui_free_button_mask();
    mouse_button_held = false;
    pointer_mode = UI_POINTER_NONE;
    pointer_btn_set1 = 0;
    pointer_btn_set2 = 0;
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

    double scale = ui_scale(m);

    if (m->cfg.frame_visible) {
        have_frame_pixels = ui_load_frame_pixels(&frame_pixels,
            &loaded_frame_width, &loaded_frame_height);
        if (!have_frame_pixels) {
            fprintf(stderr,
                "[UI] BE-300 frame unavailable - using LCD-only SDL window\n");
        }
    }

    int win_w = (int)floor((double)m->fb_width * scale + 0.5);
    int win_h = (int)floor((double)m->fb_height * scale + 0.5);
    Uint32 win_flags = 0;

    if (have_frame_pixels) {
        frame_width = loaded_frame_width;
        frame_height = loaded_frame_height;
        if (!ui_load_button_mask())
            fprintf(stderr,
                "[UI] Button mask unavailable - using legacy frame button hit-test\n");
        if (!ui_detect_frame_lcd_rect(frame_pixels, frame_width,
            frame_height)) {
            ui_set_default_frame_rects();
            fprintf(stderr,
                "[UI] Frame LCD aperture fallback: x=%d y=%d w=%d h=%d\n",
                frame_lcd_rect.x, frame_lcd_rect.y,
                frame_lcd_rect.w, frame_lcd_rect.h);
        }
        ui_configure_frame_layout(m, scale, &win_w, &win_h);
        fprintf(stderr, "[UI] Frame LCD scale: %.3gx (window %dx%d)\n",
            scale, win_w, win_h);
        window_mask = ui_create_frame_window_mask(frame_pixels, win_w,
            win_h);
        if (!window_mask)
            fprintf(stderr,
                "[UI] Frame window mask unavailable - transparent edges may show\n");
        win_flags = SDL_WINDOW_BORDERLESS;
    } else {
        fprintf(stderr, "[UI] LCD-only window scale: %.3gx (window %dx%d)\n",
            scale, win_w, win_h);
    }

    SDL_Window *win = have_frame_pixels ?
        SDL_CreateShapedWindow("BE-300 Emulator",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            (unsigned int)win_w, (unsigned int)win_h, win_flags) :
        SDL_CreateWindow("BE-300 Emulator",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            win_w, win_h, win_flags);
    if (!win && have_frame_pixels) {
        fprintf(stderr,
            "[UI] SDL_CreateShapedWindow failed: %s - using borderless rectangle\n",
            SDL_GetError());
        win = SDL_CreateWindow("BE-300 Emulator",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            win_w, win_h, win_flags);
    }
    if (!win) {
        fprintf(stderr, "[UI] SDL_CreateWindow failed: %s — continuing headless\n",
                SDL_GetError());
        free(window_mask);
        free(frame_pixels);
        ui_video_destroy(m);
        return 0;
    }
    m->sdl_window = win;

    if (have_frame_pixels) {
        ui_macos_make_window_transparent(win);
        ui_apply_frame_window_shape(win, window_mask, win_w, win_h);
        ui_macos_apply_window_mask(win, window_mask, win_w, win_h);
        if (SDL_SetWindowHitTest(win, ui_window_hit_test, m) != 0) {
            fprintf(stderr, "[UI] SDL window drag hit-test failed: %s\n",
                SDL_GetError());
        }
    }

    SDL_Renderer *ren = NULL;
    if (have_frame_pixels)
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    else
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren && have_frame_pixels)
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren && !have_frame_pixels)
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        fprintf(stderr, "[UI] SDL_CreateRenderer failed: %s — continuing headless\n",
                SDL_GetError());
        free(window_mask);
        free(frame_pixels);
        ui_video_destroy(m);
        return 0;
    }
    m->sdl_renderer = ren;
    if (have_frame_pixels) {
        ui_macos_make_window_transparent(win);
        ui_macos_apply_window_mask(win, window_mask, win_w, win_h);
    }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);

    if (have_frame_pixels) {
        frame_texture = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STATIC,
            (int)loaded_frame_width, (int)loaded_frame_height);
        if (frame_texture &&
            SDL_UpdateTexture(frame_texture, NULL, frame_pixels,
                (int)loaded_frame_width * 4) == 0) {
            SDL_SetTextureBlendMode(frame_texture, SDL_BLENDMODE_BLEND);
            frame_enabled = true;
        } else {
            fprintf(stderr,
                "[UI] SDL frame texture failed: %s - using LCD-only window\n",
                SDL_GetError());
            if (frame_texture) {
                SDL_DestroyTexture(frame_texture);
                frame_texture = NULL;
            }
            frame_width = 0;
            frame_height = 0;
            frame_scale_x = 1.0;
            frame_scale_y = 1.0;
            frame_dst_x = 0.0;
            frame_dst_y = 0.0;
            frame_dst_w = 0.0;
            frame_dst_h = 0.0;
            ui_free_button_mask();
            memset(&lcd_dst_rect, 0, sizeof(lcd_dst_rect));
            SDL_SetWindowResizable(win, SDL_FALSE);
            SDL_SetWindowSize(win,
                (int)floor((double)m->fb_width * scale + 0.5),
                (int)floor((double)m->fb_height * scale + 0.5));
        }
        free(frame_pixels);
        frame_pixels = NULL;
        free(window_mask);
        window_mask = NULL;
    }

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
            if ((ev.key.keysym.mod & KMOD_GUI) == 0 &&
                ui_stowaway_key_event(m, &ev.key, false))
                break;
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
                    "[UI]       Arrows=d-pad  Enter=ok  Esc=esc  Tab=rocket\n"
                    "[UI]       Mouse click/drag=touchpanel; framed buttons click hardware keys\n");
                break;
            /* D-pad */
            case SDLK_UP:     be300_set_buttons(m, m->btn_set1 | 0x10u, m->btn_set2); break;
            case SDLK_DOWN:   be300_set_buttons(m, m->btn_set1 | 0x20u, m->btn_set2); break;
            case SDLK_RIGHT:  be300_set_buttons(m, m->btn_set1 | 0x40u, m->btn_set2); break;
            case SDLK_LEFT:   be300_set_buttons(m, m->btn_set1 | 0x80u, m->btn_set2); break;
            /* Function buttons */
            case SDLK_RETURN: be300_set_buttons(m, m->btn_set1 | 0x04u, m->btn_set2); break;  /* ok/enter */
            case SDLK_ESCAPE: be300_set_buttons(m, m->btn_set1 | 0x08u, m->btn_set2); break;  /* esc */
            case SDLK_TAB:    be300_set_buttons(m, m->btn_set1, m->btn_set2 | 0x10u); break;  /* rocket */
            /* Power/reboot (btn_set2 0x80) intentionally not mapped */
            default:
                break;
            }
            break;

        case SDL_KEYUP:
            if ((ev.key.keysym.mod & KMOD_GUI) == 0 &&
                ui_stowaway_key_event(m, &ev.key, true))
                break;
            switch (ev.key.keysym.sym) {
            case SDLK_UP:     be300_set_buttons(m, m->btn_set1 & (uint8_t)~0x10u, m->btn_set2); break;
            case SDLK_DOWN:   be300_set_buttons(m, m->btn_set1 & (uint8_t)~0x20u, m->btn_set2); break;
            case SDLK_RIGHT:  be300_set_buttons(m, m->btn_set1 & (uint8_t)~0x40u, m->btn_set2); break;
            case SDLK_LEFT:   be300_set_buttons(m, m->btn_set1 & (uint8_t)~0x80u, m->btn_set2); break;
            case SDLK_RETURN: be300_set_buttons(m, m->btn_set1 & (uint8_t)~0x04u, m->btn_set2); break;
            case SDLK_ESCAPE: be300_set_buttons(m, m->btn_set1 & (uint8_t)~0x08u, m->btn_set2); break;
            case SDLK_TAB:    be300_set_buttons(m, m->btn_set1, m->btn_set2 & (uint8_t)~0x10u); break;
            default:
                break;
            }
            break;

        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                uint32_t ev_tick = ev.button.timestamp ?
                    ev.button.timestamp : now;
                uint8_t btn_set1 = 0;
                uint8_t btn_set2 = 0;

                if (frame_enabled &&
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
    if (frame_enabled && frame_texture) {
        SDL_FRect frame_dst = {
            (float)frame_dst_x,
            (float)frame_dst_y,
            (float)frame_dst_w,
            (float)frame_dst_h
        };

        SDL_RenderCopy((SDL_Renderer *)m->sdl_renderer,
                       (SDL_Texture *)m->sdl_texture, NULL, &lcd_dst_rect);
        SDL_RenderCopyF((SDL_Renderer *)m->sdl_renderer,
                        frame_texture, NULL, &frame_dst);
    } else {
        SDL_RenderCopy((SDL_Renderer *)m->sdl_renderer,
                       (SDL_Texture *)m->sdl_texture, NULL, NULL);
    }
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
