#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef HAVE_PNG
/* Decode a PNG held in memory to 8-bit RGBA; *pixels_out is malloc'd. */
bool ui_load_png_rgba(const uint8_t *data, size_t len,
    uint8_t **pixels_out, uint32_t *width_out, uint32_t *height_out);
/* Write 8-bit RGBA (top-down, tightly packed) to a PNG file. */
bool ui_save_png_rgba(const char *path, const uint8_t *rgba,
    uint32_t width, uint32_t height);
#endif
