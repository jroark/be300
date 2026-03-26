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
#include "wince_boot_types.h"

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

/*
 * CLI configuration — parsed from command line, passed to machine setup.
 */
typedef struct {
    bool        trace;
    bool        log_mmio;
    bool        sfb_5bit_green;
    bool        log_nand_legacy;
    bool        log_wince_stall;
    bool        wince_hw_seed;
    bool        wince_resume_replay;

    const char *rom_path;
    const char *kernel_path;
    const char *cmdline;
    const char *ram_path;
    const char *nand_path;
    uint32_t    sdram_size;      /* bytes, default 16*1024*1024 */
    uint32_t    target_mhz;      /* target CPU speed in MHz; 0 = unthrottled (default: 166) */
} machine_config_t;

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
    pmu_state_t  pmu;
    icu_state_t  icu;
    siu_state_t  siu;
    rtc_state_t  rtc;
    gpio_state_t gpio;

    /* NAND flash controller (VRC4173) */
    nand_state_t nand;

    /* NAND image data (loaded from file) */
    uint8_t     *nand_data;
    size_t       nand_size;

    /* WinCE NAND boot instrumentation/state */
    wince_boot_state_t wince;

    /* Framebuffer host pointer (from GXemul dev_fb) */
    void        *fb_data;

    /* Framebuffer geometry (fixed BE-300 hardware) */
    uint32_t     fb_width;       /* 240 visible pixels */
    uint32_t     fb_height;      /* 320 visible pixels */
    uint32_t     fb_stride;      /* 256 pixels (allocation width for alignment) */

    /* Input state (written by SDL event loop, read by be300_input MMIO device) */
    uint8_t      btn_set1;       /* PA 0x0A00A042: bits 0x04=ok 0x08=esc 0x10=up 0x20=down 0x40=right 0x80=left */
    uint8_t      btn_set2;       /* PA 0x0A00A043: bit 0x10=rocket/modifier  0x80=power */
    bool         touch_down;     /* pen-down state */
    uint16_t     touch_x;        /* screen pixel 0..239 */
    uint16_t     touch_y;        /* screen pixel 0..319 */

    /* SDL handles (opaque, cast inside ui.c) */
    void        *sdl_window;
    void        *sdl_renderer;
    void        *sdl_texture;
} be300_state_t;

typedef be300_state_t machine_t;

/*
 * Main API — called from main.c
 */
machine_t *be300_create(const machine_config_t *cfg);
void       be300_run(machine_t *m);
void       be300_destroy(machine_t *m);
