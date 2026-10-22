#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef HAVE_PNG
/* Decode a PNG held in memory to 8-bit RGBA; *pixels_out is malloc'd. */
bool ui_load_png_rgba(const uint8_t *data, size_t len,
    uint8_t **pixels_out, uint32_t *width_out, uint32_t *height_out);
#endif
