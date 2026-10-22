/*
 *  src/ui_png.c — decode the embedded frame and button-mask PNGs with
 *  libpng. Split out of src/ui.c.
 */

#include "ui_png.h"

#ifdef HAVE_PNG

#include <png.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

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

bool ui_load_png_rgba(const uint8_t *data, size_t len,
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

#endif /* HAVE_PNG */
