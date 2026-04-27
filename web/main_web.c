/*
 * web/main_web.c — Emscripten-only glue for the BE-300 emulator.
 *
 * Provides two `EMSCRIPTEN_KEEPALIVE` entry points:
 *
 *   - `be300_web_load_nand(data, len)` writes the user-supplied NAND image
 *     bytes into the MEMFS path that `be300_create()` later opens via
 *     `cfg.nand_path`. Returns 0 on full write, -1 on error.
 *
 *   - `be300_web_create(sdram_mb, target_mhz)` builds a `machine_config_t`,
 *     calls the existing public `be300_create()` API, then flips
 *     `web_mode`/`use_builtin_ui`/`mirror_serial_to_stdout` so the worker
 *     drives stepping/touch/serial entirely through the public API. Returns
 *     the opaque `machine_t *` (NULL on failure).
 *
 * No other functions are needed: the JS worker calls the existing public API
 * (`_be300_step`, `_be300_copy_frame_rgba8888`, `_be300_drain_serial`,
 * `_be300_set_touch`, `_be300_set_buttons`, `_be300_stop`, `_be300_destroy`)
 * directly. The emulator core (`src/`, `gxemul/`) is untouched — this file is
 * the only new C source the BUILD_WEB target adds on top of the existing
 * BE300/GXEMUL source lists.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
/* Allow IDE/host clang to index this file without the emcc sysroot. The
 * BUILD_WEB CMake block only ever compiles main_web.c with emcmake, so this
 * fallback never reaches a real binary. */
#define EMSCRIPTEN_KEEPALIVE
#endif

#include "be300.h"

static const char *NAND_PATH = "/All_nand_300.bin";

EMSCRIPTEN_KEEPALIVE
int be300_web_load_nand(const uint8_t *data, uint32_t len) {
    FILE *f = fopen(NAND_PATH, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return (w == (size_t)len) ? 0 : -1;
}

EMSCRIPTEN_KEEPALIVE
machine_t *be300_web_create(uint32_t sdram_mb, uint32_t target_mhz) {
    machine_config_t cfg = {0};
    cfg.nand_path = NAND_PATH;
    cfg.sdram_size = sdram_mb * 1024u * 1024u;
    cfg.target_mhz = target_mhz;
    machine_t *m = be300_create(&cfg);
    if (!m) return NULL;
    m->web_mode = true;
    m->use_builtin_ui = false;
    m->mirror_serial_to_stdout = false;
    return m;
}
