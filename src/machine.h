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
    uint32_t jiffies_pa;    /* PA of kernel jiffies symbol (resolved from ELF) */
    bool     has_jiffies_pa;
    bool     saw_icu_mmio;  /* true once guest touches ICU MMIO window */
    uint32_t fallback_timer_div;
    uint64_t irq_injected_count;
    uint64_t insn_count;
    bool     running;

    /* Manual MIPS exception-entry state (set by intr_hook / inject_hw_irq_if_pending) */
    uint64_t pending_epc;           /* EPC to return/restore via MFC0/ERET intercepts      */
    uint32_t pending_excode;        /* non-zero = injected exception active (sentinel)      */
    uint32_t pending_cause;         /* full CP0 Cause value to return via MFC0 $13          */
    bool     epc_was_written;       /* MTC0 EPC has been seen; next ERET is the exit        */
    bool     pending_cause_served;  /* first MFC0 Cause read has been intercepted */
    bool     pending_epc_served;    /* first MFC0 EPC read has been intercepted   */
    uint32_t pending_syscall_nr;    /* syscall number captured at SYSCALL inject */
    uint64_t pending_syscall_a0;    /* syscall arg0 captured at SYSCALL inject   */
    char     pending_syscall_a0_str[128]; /* best-effort arg0 string (if pointer) */
    bool     has_saved_exception;   /* nested real exception (e.g., TLBS) saved            */
    uint64_t saved_pending_epc;
    uint32_t saved_pending_excode;
    uint32_t saved_pending_cause;
    bool     saved_epc_was_written;
    bool     saved_pending_cause_served;
    bool     saved_pending_epc_served;

    /* CP0 shadow state for TLB/refill diagnostics (Unicorn API lacks CP0 accessors). */
    uint64_t shadow_cp0_index;
    uint64_t shadow_cp0_entrylo0;
    uint64_t shadow_cp0_entrylo1;
    uint64_t shadow_cp0_context;
    uint64_t shadow_cp0_pagemask;
    uint64_t shadow_cp0_badvaddr;
    uint64_t shadow_cp0_entryhi;
    uint64_t shadow_cp0_epc;

    /* Deferred MFC0 readback (value is observed from $rt at the following insn). */
    bool     cp0_readback_pending;
    uint8_t  cp0_readback_rt;
    uint8_t  cp0_readback_rd;
    uint8_t  cp0_readback_sel;
    uint64_t cp0_readback_next_pc;

    /* Late-boot TLB/refill debug window (armed near run_init_process). */
    bool     tlb_trace_window;
    bool     post_init_trace_window;
    uint32_t post_init_trace_batches;

    /* Runtime instruction patching: temporarily rewrite tlbwi -> tlbwr. */
    bool     tlbwi_patch_pending;
    uint64_t tlbwi_patch_addr;
    uint32_t tlbwi_patch_orig;
};

machine_t *machine_create(const machine_config_t *cfg);
void       machine_destroy(machine_t *m);
void       machine_run(machine_t *m);
void       machine_stop(machine_t *m);
