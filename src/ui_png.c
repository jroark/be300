/*
 *  src/ui_png.c — decode the embedded frame and button-mask PNGs with
 *  libpng. Split out of src/ui.c.
 */

#include "ui_png.h"

#if defined(HAVE_PNG) && defined(BE300_PNG_WIC)
/* Windows builds without libpng decode and encode through the Windows
 * Imaging Component, which every supported Windows has. */
#define COBJMACROS
#include <windows.h>
#include <wincodec.h>
#include <stdlib.h>
#include <string.h>

static IWICImagingFactory *wic_factory(bool *uninit_out)
{
    IWICImagingFactory *f = NULL;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    *uninit_out = SUCCEEDED(hr);
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL,
            CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f))) {
        if (*uninit_out)
            CoUninitialize();
        *uninit_out = false;
        return NULL;
    }
    return f;
}

bool ui_load_png_rgba(const uint8_t *data, size_t len,
    uint8_t **pixels_out, uint32_t *width_out, uint32_t *height_out)
{
    bool uninit, ok = false;
    IWICImagingFactory *f = wic_factory(&uninit);
    IWICStream *stream = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    UINT w = 0, h = 0;
    uint8_t *px = NULL;

    if (!f)
        return false;
    if (FAILED(IWICImagingFactory_CreateStream(f, &stream)) ||
        FAILED(IWICStream_InitializeFromMemory(stream, (BYTE *)data, (DWORD)len)) ||
        FAILED(IWICImagingFactory_CreateDecoderFromStream(f, (IStream *)stream,
            NULL, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) ||
        FAILED(IWICImagingFactory_CreateFormatConverter(f, &conv)) ||
        FAILED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame,
            &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0,
            WICBitmapPaletteTypeCustom)) ||
        FAILED(IWICFormatConverter_GetSize(conv, &w, &h)) || !w || !h)
        goto out;
    px = malloc((size_t)w * h * 4u);
    if (!px || FAILED(IWICFormatConverter_CopyPixels(conv, NULL, w * 4u,
            w * h * 4u, px)))
        goto out;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint8_t b = px[i * 4u];
        px[i * 4u] = px[i * 4u + 2u];
        px[i * 4u + 2u] = b;
    }
    *pixels_out = px;
    *width_out = w;
    *height_out = h;
    px = NULL;
    ok = true;
out:
    free(px);
    if (conv) IWICFormatConverter_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (stream) IWICStream_Release(stream);
    IWICImagingFactory_Release(f);
    if (uninit)
        CoUninitialize();
    return ok;
}

bool ui_save_png_rgba(const char *path, const uint8_t *rgba,
    uint32_t width, uint32_t height)
{
    bool uninit, ok = false;
    IWICImagingFactory *f;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    size_t n = (size_t)width * height;
    uint8_t *bgra;
    wchar_t wpath[MAX_PATH];

    if (!path || !rgba || !width || !height ||
        !MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH))
        return false;
    bgra = malloc(n * 4u);
    if (!bgra)
        return false;
    for (size_t i = 0; i < n; i++) {
        bgra[i * 4u + 0u] = rgba[i * 4u + 2u];
        bgra[i * 4u + 1u] = rgba[i * 4u + 1u];
        bgra[i * 4u + 2u] = rgba[i * 4u + 0u];
        bgra[i * 4u + 3u] = rgba[i * 4u + 3u];
    }
    f = wic_factory(&uninit);
    if (!f) {
        free(bgra);
        return false;
    }
    ok = SUCCEEDED(IWICImagingFactory_CreateStream(f, &stream)) &&
        SUCCEEDED(IWICStream_InitializeFromFilename(stream, wpath, GENERIC_WRITE)) &&
        SUCCEEDED(IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatPng,
            NULL, &enc)) &&
        SUCCEEDED(IWICBitmapEncoder_Initialize(enc, (IStream *)stream,
            WICBitmapEncoderNoCache)) &&
        SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(enc, &frame, NULL)) &&
        SUCCEEDED(IWICBitmapFrameEncode_Initialize(frame, NULL)) &&
        SUCCEEDED(IWICBitmapFrameEncode_SetSize(frame, width, height)) &&
        SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(frame, &fmt)) &&
        IsEqualGUID(&fmt, &GUID_WICPixelFormat32bppBGRA) &&
        SUCCEEDED(IWICBitmapFrameEncode_WritePixels(frame, height, width * 4u,
            (UINT)(n * 4u), bgra)) &&
        SUCCEEDED(IWICBitmapFrameEncode_Commit(frame)) &&
        SUCCEEDED(IWICBitmapEncoder_Commit(enc));
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (stream) IWICStream_Release(stream);
    IWICImagingFactory_Release(f);
    if (uninit)
        CoUninitialize();
    free(bgra);
    if (!ok)
        DeleteFileW(wpath);
    return ok;
}

#elif defined(HAVE_PNG)

#include <png.h>
#include <setjmp.h>
#include <stdio.h>
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

bool ui_save_png_rgba(const char *path, const uint8_t *rgba,
    uint32_t width, uint32_t height)
{
    FILE *f;
    png_structp png;
    png_infop info = NULL;
    volatile bool ok = false;

    if (!path || !rgba || width == 0 || height == 0)
        return false;
    f = fopen(path, "wb");
    if (!f)
        return false;
    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png)
        info = png_create_info_struct(png);
    if (!png || !info)
        goto out;
    if (setjmp(png_jmpbuf(png)))
        goto out;
    png_init_io(png, f);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGBA,
        PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
        PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (uint32_t y = 0; y < height; y++)
        png_write_row(png, (png_const_bytep)(rgba + (size_t)y * width * 4u));
    png_write_end(png, NULL);
    ok = true;

out:
    if (png)
        png_destroy_write_struct(&png, info ? &info : NULL);
    if (fclose(f) != 0)
        ok = false;
    if (!ok)
        remove(path);
    return ok;
}

#endif /* HAVE_PNG */
