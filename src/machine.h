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
#include "hw/nand.h"

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
    machine_t *m;
    uint32_t   region_base_pa;
} vrc4173_cb_ctx_t;

/*
 * Recent MMIO access ring used by WinCE stall diagnostics.
 * Records are ordered by insertion; head points to next write slot.
 */
#define WINCE_MMIO_HISTORY_LEN 64u
typedef struct {
    uint32_t pa;
    uint32_t pc;
    uint16_t size_bits;
    uint8_t  is_write;
    uint8_t  reserved;
    uint64_t value;
} mmio_hist_entry_t;

typedef struct {
    bool        trace;        /* log each instruction to stderr */
    bool        log_mmio;     /* log all MMIO register accesses */
    bool        sfb_5bit_green; /* true = apply 5-bit green expansion (2.6 sfb.c non-standard)
                                 * false (default) = pass pixels through as-is (true RGB565) */
    bool        kuseg_hotpath_populate; /* experimental: try shadow-TLB-backed kuseg population in LOAD_EMU/STORE_EMU */
    bool        log_nand_legacy; /* log D7F8/D7FC indexed register traffic */
    bool        log_wince_stall; /* log WinCE post-NAND stall diagnostics */
    bool        trace_user_handoff; /* debug: first-fault and handoff VA->PA trace */
    const char *rom_path;     /* path to flat ROM image, loaded at PA_RESET_VECTOR */
    const char *kernel_path;  /* path to ELF kernel (vmlinux); if set, skip ROM boot */
    const char *cmdline;      /* kernel command line (passed in $a1 as argv[0]) */
    const char *ram_path;     /* optional: preload a raw RAM image at PA_SDRAM_BASE */
    const char *nand_path;    /* optional: NAND image for WinCE boot (B000FF SPL) */
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
    nand_state_t nand;
    vrc4173_cb_ctx_t vrc4173_region1_ctx;
    vrc4173_cb_ctx_t vrc4173_region3_ctx;

    /* NAND image backing store (WinCE boot) */
    uint8_t *nand_data;     /* host-side copy of the NAND image */
    size_t   nand_size;     /* size in bytes */

    /* Optional host-backed SDRAM mapping used for PA/kseg alias coherence. */
    uint8_t *sdram_backing;
    size_t   sdram_backing_size;
    bool     shared_alias_active;       /* true when PA+kseg aliases share backing */
    bool     alias_fallback_sync_active;/* write-sync fallback when shared aliasing fails */
    bool     alias_sync_reentrant;      /* recursion guard for write-sync hook */

    uint64_t kernel_entry;  /* VA to start execution from, sign-extended for MIPS64 */
    uint32_t jiffies_pa;    /* PA of kernel jiffies symbol (resolved from ELF) */
    bool     has_jiffies_pa;
    bool     saw_icu_mmio;  /* true once guest touches ICU MMIO window */
    uint32_t fallback_timer_div;
    uint64_t irq_injected_count;
    uint64_t insn_count;
    bool     running;
    mmio_hist_entry_t mmio_hist[WINCE_MMIO_HISTORY_LEN];
    uint32_t mmio_hist_head;
    uint32_t mmio_hist_count;
    uint32_t wince_null_last_ra;
    uint32_t wince_null_last_intno;
    uint32_t wince_null_consecutive;

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
    uint64_t pending_syscall_epc;   /* ORIGINAL syscall VA; not overwritten by MTC0 EPC */
    bool     execve_user_handoff_active; /* redirected to userspace entry from execve */
    uint64_t execve_user_handoff_pc;     /* last user entry PC for stale trap redirect */
    uint64_t execve_user_handoff_sp;     /* last user stack for stale trap redirect     */
    uint8_t  execve_user_handoff_state;  /* 0=DONE, 1=ARMED, 2=USER_FETCH_SEEN */
    uint32_t execve_user_handoff_done_keep_count; /* stale DONE redirects since last progress */
    bool     user_handoff_fault_traced;  /* trace first user fault details once per handoff */
    bool     do_no_page_watch_active;    /* trace do_no_page return for handoff target */
    uint64_t do_no_page_watch_ra;
    uint32_t do_no_page_watch_addr;
    uint64_t do_no_page_watch_pte_ptr;
    bool     filemap_nopage_watch_active;/* trace filemap_nopage return for handoff target */
    uint64_t filemap_nopage_watch_ra;
    uint32_t filemap_nopage_watch_addr;
    bool     execve_watch_active;   /* waiting to log do_execve return value      */
    uint64_t execve_watch_ret_pc;   /* caller PC to observe on do_execve return   */
    uint64_t execve_watch_a0;       /* do_execve filename pointer (entry arg0)    */
    uint64_t execve_saved_a1;       /* argv pointer saved at do_execve entry      */
    uint64_t execve_saved_a2;       /* envp pointer saved at do_execve entry      */
    uint64_t execve_saved_sp;       /* $sp saved at do_execve entry (restored on DEFER_SKIP) */
    uint64_t execve_entry_pc;       /* VA of do_execve entry (for stale-TLB retry)*/
    uint64_t execve_last_pc;        /* most recent in-flight do_execve PC         */
    uint64_t execve_last_gpr[32];   /* GPR snapshot at execve_last_pc              */
    bool     execve_last_ctx_valid; /* true when execve_last_gpr contains snapshot */
    char     execve_watch_path[128];/* do_execve filename snapshot                 */
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
    uint64_t shadow_cp0_entryhi_live; /* latest MFC0 EntryHi readback (current ASID hint) */
    uint64_t shadow_cp0_epc;
    bool     shadow_cp0_entryhi_live_valid;

    /* Minimal shadow TLB cache captured from tlbwi/tlbwr instructions.
     * Used to resolve kuseg VA->PA at ERET/handoff time when the single
     * CP0 snapshot no longer matches the target user VA. */
    bool     shadow_tlb_valid[64];
    uint32_t shadow_tlb_entryhi[64];
    uint32_t shadow_tlb_lo0[64];
    uint32_t shadow_tlb_lo1[64];
    uint32_t shadow_tlb_pagemask[64];
    uint32_t shadow_tlb_seq[64];
    uint32_t shadow_tlb_seq_next;
    uint32_t shadow_tlb_wr_cursor;

    /* Framebuffer / UI */
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_stride;
    uint8_t *fb_ptr;    /* pointer to host memory if mapped, or NULL */
    void    *sdl_window;
    void    *sdl_renderer;
    void    *sdl_texture;
    uint64_t last_unmapped_addr;     /* fault VA reported by mem_unmapped_hook */
    uc_mem_type last_unmapped_type;  /* corresponding UC_MEM_* access type      */
    bool     last_unmapped_valid;

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
    uint32_t refill_insn_count;
    uint32_t general_insn_count;

    /* Runtime instruction patching: temporarily rewrite tlbwi -> tlbwr. */
    bool     tlbwi_patch_pending;
    uint64_t tlbwi_patch_addr;
    uint32_t tlbwi_patch_orig;

    /* After TLBWI/TLBWR executes, flush Unicorn's softmmu TLB at the next
     * instruction so stale TLB entries (e.g. D=0 → D=1 upgrade) don't
     * prevent the retry store from succeeding. */
    bool     pending_tlb_flush;

    /* VR41xx→MIPS32 PFN fixup: after patching a GPR for MTC0 EntryLo,
     * restore the original value on the next instruction. */
    bool     pending_gpr_restore;
    int      pending_gpr_reg;
    uint64_t pending_gpr_val;

    /* TLB notification de-duplication for execve DEFER.
     * Unicorn re-delivers the same notification-only intno=26 at SYSCALL+4
     * after ERET_TLB_PASSTHROUGH returns to the faulting instruction (before
     * any instruction there executes). After the first DEFER, subsequent
     * notifications at SYSCALL+4 while execve_watch_active are spurious:
     * just restore PC and return so execution can continue. */
    uint32_t tlb_defer_count;
    bool     tlb_defer_active;       /* true after first DEFER for current syscall */
    uint32_t tlb_defer_owner_epc;    /* syscall EPC that owns current DEFER state  */
    uint64_t tlb_defer_fault_pc;     /* real fault PC saved at DEFER entry for ERET return */
    uint64_t last_exec_pc;           /* last PC seen by prid_hook (code hook) */
    uint32_t tlb_exl_drop_defer_count; /* IRQ gate deferrals while EXL unexpectedly cleared */

    /* open_exec fd-leak compensation.
     * open_exec (JAL at 0x8004A9F8) allocates a struct file (nr_files++) and
     * returns a file pointer in $v0.  When do_execve is restarted by
     * ERET_TLB_PASSTHROUGH or TLB_DEFER_SKIP the allocated struct file is
     * abandoned without calling fput(), leaking the nr_files count.  After
     * enough restarts nr_files >= max_files → ENFILE.
     *
     * execve_open_exec_ran is set when execution reaches the open_exec return
     * point (0x8004AA00) and cleared at each do_execve entry.  When it is
     * set at the time of a do_execve restart, we decrement nr_files directly
     * in guest memory to undo the orphaned increment. */
    bool     execve_open_exec_ran;
};

machine_t *machine_create(const machine_config_t *cfg);
void       machine_destroy(machine_t *m);
void       machine_run(machine_t *m);
void       machine_stop(machine_t *m);
void       machine_mmio_history_record(machine_t *m, bool is_write, uint32_t pa,
                                       unsigned size, uint64_t value, uint32_t pc);
