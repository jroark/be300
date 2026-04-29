/*
 * web/main_web.c — Emscripten-only glue for the BE-300 emulator.
 *
 * Provides the `EMSCRIPTEN_KEEPALIVE` entry points used by `web/worker.js`:
 *
 *   - `be300_web_load_nand(data, len)` writes the user-supplied NAND image
 *     bytes into the MEMFS path that `be300_create()` later opens via
 *     `cfg.nand_path`. Returns 0 on full write, -1 on error.
 *
 *   - `be300_web_load_cf(slot, data, len)` writes optional CF images into
 *     MEMFS paths consumed through `cfg.cf_paths[]`.
 *
 *   - `be300_web_create_ex(sdram_mb, target_mhz, flags, mac)` builds a
 *     `machine_config_t`, calls the existing public `be300_create()` API,
 *     then flips `web_mode`/`use_builtin_ui`/`mirror_serial_to_stdout`.
 *
 *   - `be300_web_stowaway_key`, `be300_web_net_rx`, and
 *     `be300_web_net_tx_pop` expose the browser-only keyboard and Ethernet
 *     bridge edges. `be300_web_set_net_bridge_enabled` lets the worker
 *     switch the bridge path on only after the WebSocket is connected. The
 *     worker still drives stepping/touch/serial through the existing public
 *     API in `src/be300.h`.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Use __has_include so IDE/LSP indexers without the emcc sysroot don't choke
 * on the missing header. Real BUILD_WEB compiles always go through emcmake
 * and pick up the real <emscripten.h>. */
#if defined(__has_include)
#  if __has_include(<emscripten.h>)
#    include <emscripten.h>
#  endif
#endif
#ifndef EMSCRIPTEN_KEEPALIVE
#  define EMSCRIPTEN_KEEPALIVE
#endif

/* Relative path so IDE/LSP indexers without `-Isrc` still resolve the header.
 * The emcmake build adds `src/` via include_directories() in CMakeLists.txt
 * so a bare "be300.h" would also work, but the relative form is unambiguous. */
#include "../src/be300.h"
#include "../src/stowaway.h"
#include "emul.h"

static const char *NAND_PATH = "/All_nand_300.bin";
static const char *CF_PATHS[BE300_MAX_CF_SLOTS] = {
    "/cf0.img",
    "/cf1.img",
};

#define WEB_FLAG_NE2000          UINT32_C(0x00000001)
#define WEB_FLAG_TARGUS_KEYBOARD UINT32_C(0x00000002)
#define WEB_FLAG_CF0             UINT32_C(0x00000004)
#define WEB_FLAG_CF1             UINT32_C(0x00000008)
#define WEB_FLAG_NET_MAC         UINT32_C(0x00000010)
#define WEB_FLAG_NET_BRIDGE      UINT32_C(0x00000020)

#define WEB_NET_FRAME_MAX 2048u
#define WEB_NET_TX_QUEUE_CAP 64u

typedef struct {
    uint16_t len;
    uint8_t  data[WEB_NET_FRAME_MAX];
} web_net_frame_t;

static web_net_frame_t g_net_tx_queue[WEB_NET_TX_QUEUE_CAP];
static unsigned g_net_tx_head;
static unsigned g_net_tx_tail;
static unsigned g_net_tx_count;
static bool g_net_bridge_enabled;
static machine_t *g_current_machine;

static void web_net_tx_reset(void)
{
    g_net_tx_head = 0;
    g_net_tx_tail = 0;
    g_net_tx_count = 0;
}

static int web_write_file(const char *path, const uint8_t *data, uint32_t len)
{
    FILE *f;
    size_t w;

    if (!path || !data || len == 0)
        return -1;

    f = fopen(path, "wb");
    if (!f)
        return -1;
    w = fwrite(data, 1, len, f);
    fclose(f);
    return (w == (size_t)len) ? 0 : -1;
}

EMSCRIPTEN_KEEPALIVE
int be300_web_load_nand(const uint8_t *data, uint32_t len) {
    return web_write_file(NAND_PATH, data, len);
}

EMSCRIPTEN_KEEPALIVE
int be300_web_load_cf(uint32_t slot, const uint8_t *data, uint32_t len) {
    if (slot >= BE300_MAX_CF_SLOTS)
        return -1;
    return web_write_file(CF_PATHS[slot], data, len);
}

EMSCRIPTEN_KEEPALIVE
machine_t *be300_web_create_ex(uint32_t sdram_mb, uint32_t target_mhz,
                               uint32_t flags, const uint8_t *mac) {
    machine_config_t cfg = {0};
    machine_t *m;

    if ((flags & WEB_FLAG_NE2000) && (flags & WEB_FLAG_CF0)) {
        fprintf(stderr,
            "[WEB] NE2000 and primary CF both use the primary PCMCIA socket\n");
        return NULL;
    }

    cfg.nand_path = NAND_PATH;
    cfg.sdram_size = sdram_mb * 1024u * 1024u;
    cfg.target_mhz = target_mhz;
    cfg.enable_rtc_host_time = true;
    cfg.enable_ne2000 = (flags & WEB_FLAG_NE2000) != 0;
    cfg.enable_stowaway_keyboard =
        (flags & WEB_FLAG_TARGUS_KEYBOARD) != 0;
    if ((flags & WEB_FLAG_NET_MAC) && mac) {
        memcpy(cfg.net_mac, mac, sizeof(cfg.net_mac));
        cfg.net_mac_set = true;
    }
    if (flags & WEB_FLAG_CF0)
        cfg.cf_paths[0] = CF_PATHS[0];
    if (flags & WEB_FLAG_CF1)
        cfg.cf_paths[1] = CF_PATHS[1];
    for (unsigned i = 0; i < BE300_MAX_CF_SLOTS; i++) {
        if (cfg.cf_paths[i])
            cfg.cf_count = i + 1u;
    }

    web_net_tx_reset();
    g_net_bridge_enabled =
        cfg.enable_ne2000 && ((flags & WEB_FLAG_NET_BRIDGE) != 0);
    g_current_machine = NULL;

    m = be300_create(&cfg);
    if (!m) {
        g_net_bridge_enabled = false;
        return NULL;
    }
    m->web_mode = true;
    m->use_builtin_ui = false;
    m->mirror_serial_to_stdout = false;
    g_current_machine = m;
    return m;
}

EMSCRIPTEN_KEEPALIVE
machine_t *be300_web_create(uint32_t sdram_mb, uint32_t target_mhz) {
    return be300_web_create_ex(sdram_mb, target_mhz, 0, NULL);
}

EMSCRIPTEN_KEEPALIVE
int be300_web_stowaway_key(uint32_t scancode, uint32_t release) {
    if (!g_current_machine ||
        !g_current_machine->cfg.enable_stowaway_keyboard)
        return -1;
    return stowaway_queue_key(scancode, release != 0) ? 0 : -1;
}

int be300_web_net_bridge_enabled(void) {
    return g_net_bridge_enabled ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
void be300_web_set_net_bridge_enabled(uint32_t enabled) {
    g_net_bridge_enabled = enabled != 0 &&
        g_current_machine &&
        g_current_machine->cfg.enable_ne2000;
    if (!g_net_bridge_enabled)
        web_net_tx_reset();
}

void be300_web_net_tx_enqueue(const uint8_t *packet, uint32_t len) {
    web_net_frame_t *frame;

    if (!g_net_bridge_enabled || !packet || len < 14 ||
        len > WEB_NET_FRAME_MAX)
        return;

    if (g_net_tx_count == WEB_NET_TX_QUEUE_CAP) {
        g_net_tx_tail = (g_net_tx_tail + 1u) % WEB_NET_TX_QUEUE_CAP;
        g_net_tx_count--;
    }

    frame = &g_net_tx_queue[g_net_tx_head];
    frame->len = (uint16_t)len;
    memcpy(frame->data, packet, len);
    g_net_tx_head = (g_net_tx_head + 1u) % WEB_NET_TX_QUEUE_CAP;
    g_net_tx_count++;
}

EMSCRIPTEN_KEEPALIVE
uint32_t be300_web_net_tx_pending(void) {
    return g_net_tx_count;
}

EMSCRIPTEN_KEEPALIVE
int be300_web_net_tx_pop(uint8_t *dst, uint32_t dst_len) {
    web_net_frame_t *frame;
    uint16_t len;

    if (g_net_tx_count == 0)
        return 0;
    frame = &g_net_tx_queue[g_net_tx_tail];
    len = frame->len;
    if (!dst || dst_len < len)
        return -1;

    memcpy(dst, frame->data, len);
    g_net_tx_tail = (g_net_tx_tail + 1u) % WEB_NET_TX_QUEUE_CAP;
    g_net_tx_count--;
    return (int)len;
}

EMSCRIPTEN_KEEPALIVE
int be300_web_net_rx(machine_t *m, const uint8_t *packet, uint32_t len) {
    struct ethernet_packet_link *lp;

    if (!m)
        m = g_current_machine;
    if (!m || !packet || len < 14 || len > WEB_NET_FRAME_MAX)
        return -1;
    if (!cf_is_ne2000(&m->cf[BE300_PRIMARY_CF_SLOT]) ||
        !m->emul || !m->emul->net)
        return -1;

    lp = net_allocate_ethernet_packet_link(
        m->emul->net,
        &m->cf[BE300_PRIMARY_CF_SLOT].ne2000.nic,
        len);
    memcpy(lp->data, packet, len);
    return 0;
}
