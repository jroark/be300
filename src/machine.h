#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <unicorn/unicorn.h>

#include "hw/bcu.h"
#include "hw/cmu.h"
#include "hw/pmu.h"
#include "hw/icu.h"
#include "hw/siu.h"
#include "hw/rtc.h"
#include "hw/gpio.h"

/* Physical address map (VR4131 hardware manual §3.1) */
#define PA_SDRAM_BASE    UINT32_C(0x00000000)
#define PA_SDRAM_SIZE    (64u * 1024u * 1024u)   /* 64 MB max */
#define PA_IO_BASE       UINT32_C(0x0F000000)
#define PA_IO_SIZE       UINT32_C(0x00001000)    /* 4 KB internal I/O */
#define PA_ROM_BASE      UINT32_C(0x1E000000)
#define PA_ROM_SIZE      (32u * 1024u * 1024u)   /* 32 MB ROM/Flash */
#define PA_RESET_VECTOR  UINT32_C(0x1FC00000)    /* physical offset of reset vector */

/* MIPS kseg1 virtual reset vector -> PA_RESET_VECTOR */
#define VA_RESET_VECTOR  UINT32_C(0xBFC00000)

/* VR4131 CP0 PRId (NEC, processor ID 0x0C, rev 0x80) */
#define VR4131_PRID      UINT32_C(0x00000C80)

typedef struct machine_s machine_t;

typedef struct {
    bool        trace;        /* log each instruction to stderr */
    bool        log_mmio;     /* log all MMIO register accesses */
    const char *rom_path;     /* path to flat ROM image, loaded at PA_RESET_VECTOR */
    const char *kernel_path;  /* path to ELF kernel (vmlinux); if set, skip ROM boot */
    const char *cmdline;      /* kernel command line (passed in $a1 as argv[0]) */
    const char *ram_path;     /* optional: preload a raw RAM image at PA_SDRAM_BASE */
    uint32_t    sdram_size;   /* SDRAM size in bytes (default 16 MB) */
} machine_config_t;

struct machine_s {
    uc_engine       *uc;
    machine_config_t cfg;

    /* Peripherals */
    bcu_state_t  bcu;
    cmu_state_t  cmu;
    pmu_state_t  pmu;
    icu_state_t  icu;
    siu_state_t  siu;
    rtc_state_t  rtc;
    gpio_state_t gpio;

    uint64_t kernel_entry;  /* VA to start execution from, sign-extended for MIPS64 */
    uint64_t insn_count;
    bool     running;
};

machine_t *machine_create(const machine_config_t *cfg);
void       machine_destroy(machine_t *m);
void       machine_run(machine_t *m);
void       machine_stop(machine_t *m);
