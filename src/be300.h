/*
 *  machine.h — BE-300 machine definitions for GXemul integration.
 *
 *  This replaces the old Unicorn-based machine.h. The GXemul CPU engine
 *  handles CP0, TLB, exceptions, and address translation natively.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Hardware peripheral state */
#include "hw/bcu.h"
#include "hw/cmu.h"
#include "hw/pmu.h"
#include "hw/icu.h"
#include "hw/siu.h"
#include "hw/rtc.h"
#include "hw/gpio.h"
#include "hw/nand.h"
#include "hw/cf.h"

/* Forward declarations for GXemul types */
struct cpu;
struct machine;
struct memory;
struct emul;

/* Physical address map (VR4131 hardware manual §3.1) */
#define PA_SDRAM_BASE    UINT32_C(0x00000000)
#define PA_SDRAM_SIZE    (64u * 1024u * 1024u)
#define PA_IO_BASE       UINT32_C(0x0F000000)
#define PA_IO_SIZE       UINT32_C(0x00001000)
#define PA_ROM_BASE      UINT32_C(0x1E000000)
#define PA_ROM_SIZE      (32u * 1024u * 1024u)
#define PA_RESET_VECTOR  UINT32_C(0x1FC00000)

/* VRC4173 companion chip physical addresses */
#define PA_VRC4173_BASE  UINT32_C(0x0A000000)
#define PA_VRC4173_SIU   UINT32_C(0x0A008680)
#define PA_VRC4173_FB    UINT32_C(0x0A200000)

/* MIPS virtual addresses */
#define VA_RESET_VECTOR  UINT32_C(0xBFC00000)

/* VR4131 CP0 PRId */
#define VR4131_PRID      UINT32_C(0x00000C80)

#define BE300_MAX_CF_SLOTS 2u
#define BE300_PRIMARY_CF_SLOT 0u

/*
 * CLI configuration — parsed from command line, passed to machine setup.
 */
typedef struct {
    bool        trace;
    bool        log_mmio;
    bool        log_nand_legacy;
    bool        enable_ppsh;
    bool        enable_pcconnect_time_sync;
    bool        enable_rtc_host_time;
    bool        enable_stowaway_keyboard;
    bool        enable_ne2000;
    bool        net_mac_set;
    bool        restore;

    /* Default-off coverage instrumentation.
     * mmio_coverage: first-hit-per-(device, offset, op) log + shutdown table.
     * detect_stall:  periodic unique-PC sampler that fires [BE300_STALL] on spin. */
    bool        mmio_coverage;
    bool        detect_stall;
    uint32_t    stall_window;            /* instructions per sampling bucket (default 10000) */
    uint32_t    stall_unique_threshold;  /* fire when unique PCs in window drops below this (default 64) */
    uint32_t    stall_wall_secs;         /* sustained wall-seconds below threshold before firing (default 5) */

    const char *rom_path;
    const char *nand_path;
    const char *cf_paths[BE300_MAX_CF_SLOTS];
    unsigned    cf_count;
    uint8_t     net_mac[6];
    uint32_t    sdram_size;      /* bytes, default 16*1024*1024 */
    uint32_t    target_mhz;      /* throttle target in million guest instructions/sec; 0 = unthrottled */
    double      frame_lcd_scale; /* framed SDL LCD scale; 0 = default/env */

    /* Optional --pcconnect-bridge spec; mutually exclusive with
     * --pcconnect-time-sync. NULL = disabled.
     * Examples: "tcp:127.0.0.1:5555", "tcp-listen:5555",
     *           "unix:/tmp/pcc.sock", "unix-listen:/tmp/pcc.sock", "pty:auto".
     * --pcconnect-tee writes both directions to a host-side log. */
    const char *pcconnect_bridge;
    const char *pcconnect_tee;
    /* Inter-byte throttle on the guest->host bridge direction. 0 = unlimited.
     * Default 115200 to match real-hardware 8N1 timing. */
    uint32_t    pcconnect_baud;
} machine_config_t;

typedef enum {
    BE300_BOOT_NONE = 0,
    BE300_BOOT_NAND,
    BE300_BOOT_RESTORE,
    BE300_BOOT_ROM,
} be300_boot_mode_t;

#define BE300_SERIAL_RING_CAP 65536u

/*
 * BE-300 machine state.
 */
typedef struct be300_state {
    /* GXemul framework objects */
    struct emul    *emul;
    struct machine *gxe_machine;
    struct cpu     *cpu;

    /* CLI configuration */
    machine_config_t cfg;

    /* Peripheral state (VR4131 internal I/O) */
    bcu_state_t  bcu;
    cmu_state_t  cmu;
    icu_state_t  icu;
    siu_state_t  siu;
    rtc_state_t  rtc;
    gpio_state_t gpio;

    /* NAND flash controller (VRC4173) */
    nand_state_t nand;
    cf_state_t   cf[BE300_MAX_CF_SLOTS];

    /* NAND image data (loaded from file) */
    uint8_t     *nand_data;
    size_t       nand_size;       /* emulated chip buffer size */
    size_t       nand_file_size;  /* bytes loaded from the data-only image */

    /* Framebuffer host pointer (from GXemul dev_fb) */
    void        *fb_data;

    /* Framebuffer geometry (fixed BE-300 hardware) */
    uint32_t     fb_width;       /* 240 visible pixels */
    uint32_t     fb_height;      /* 320 visible pixels */
    uint32_t     fb_stride;      /* 256 pixels (allocation width for alignment) */

    /* PIU touch device (for tick callback interrupt generation) */
    void        *touch_device;
    void        *button_device;

    /* Input state (written by SDL event loop, read by be300_input MMIO device) */
    uint8_t      btn_set1;       /* PA 0x0A00A044: bits 0x04=ok 0x08=esc 0x10=up 0x20=down 0x40=right 0x80=left */
    uint8_t      btn_set2;       /* PA 0x0A00A045: bit 0x10=rocket/modifier  0x80=power */
    bool         touch_down;     /* pen-down state */
    uint16_t     touch_x;        /* touch-panel pixel 0..239 */
    uint16_t     touch_y;        /* touch-panel pixel 0..359; 320..359 drives the icon strip below the display */

    /* SDL handles (opaque, cast inside ui.c) */
    void        *sdl_window;
    void        *sdl_renderer;
    void        *sdl_texture;

    /* Boot/runtime host state */
    be300_boot_mode_t boot_mode;
    bool         input_registered;
    bool         runtime_initialized;
    bool         runtime_stopped;
    bool         runtime_finalized;
    bool         web_mode;
    bool         use_builtin_ui;
    bool         save_exit_screenshot;
    bool         mirror_serial_to_stdout;
    uint64_t     throttle_target_ips;
    uint64_t     throttle_wall_origin;
    uint64_t     throttle_instr_origin;
    uint64_t     autostop_after_ns;
    uint64_t     autostop_start_ns;
    int64_t      last_report;
    int          loop_count;

    /* Serial capture ring for non-terminal hosts */
    char         serial_ring[BE300_SERIAL_RING_CAP];
    size_t       serial_head;
    size_t       serial_tail;
    size_t       serial_count;

    /* Optional emulator-only flat NK override for diagnostic payload testing */
    char        *nk_override_path;
    bool         nk_override_applied;
    bool         nk_override_failed;
} be300_state_t;

typedef be300_state_t machine_t;

/*
 * Main API — called from main.c
 */
machine_t *be300_create(const machine_config_t *cfg);
int        be300_step(machine_t *m, uint32_t max_batches);
int        be300_copy_frame_rgba8888(machine_t *m, uint8_t *dst,
                                     size_t dst_len,
                                     uint32_t *width_out,
                                     uint32_t *height_out);
size_t     be300_drain_serial(machine_t *m, char *dst, size_t dst_len);
void       be300_set_touch(machine_t *m, bool down, uint16_t x, uint16_t y);
void       be300_set_buttons(machine_t *m, uint8_t btn_set1, uint8_t btn_set2);
void       be300_buttons_host_update(machine_t *m, uint8_t old_set1,
                                     uint8_t old_set2);
void       be300_touch_tick(machine_t *m);
void       be300_pcconnect_poll(void);
bool       be300_pcconnect_irq_is_asserted(void);
void       be300_vrc4173_update_cf_irq(void);
void       be300_stop(machine_t *m);
void       be300_run(machine_t *m);
void       be300_destroy(machine_t *m);
bool       be300_vrc4173_latch_read_u32(uint32_t pa, uint32_t *out);
