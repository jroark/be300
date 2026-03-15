#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unicorn/unicorn.h>

#include "hw/bcu.h"
#include "hw/cmu.h"
#include "hw/pmu.h"
#include "hw/icu.h"
#include "hw/siu.h"
#include "hw/rtc.h"
#include "hw/gpio.h"
#include "hw/nand.h"

/* Kernel symbol addresses (vmlinux-pgui-demo 2.4.18, vmlinux-2.6) */
#define RUN_INIT_SYSCALL_EPC      0x8000181Cu
#define RUN_INIT_SYSCALL_RET_PC   0x80001820u
#define RUN_INIT_SYSCALL_EPC_26   0x800015B0u
#define RUN_INIT_SYSCALL_RET_PC_26 0x800015B4u
#define INIT_ARGV_KPTR            0x80171244u
#define INIT_ENVP_KPTR            0x8017126Cu
#define EXECVE_SCRATCH_BASE       0x01020000u
#define EXECVE_SCRATCH_END        0x01021000u
#define DO_EXECVE_START_PC        0x8004A9D0u
#define DO_EXECVE_END_PC          0x8004ACA4u
#define TLB_DEFER_RETRY_LIMIT     32u
#define TLB_EXL_DROP_DEFER_LIMIT  64u
#define EXECVE_HANDOFF_STATE_DONE          0u
#define EXECVE_HANDOFF_STATE_ARMED         1u
#define EXECVE_HANDOFF_STATE_USER_FETCH_SEEN 2u
#define USER_HANDOFF_ENTRY_WINDOW_START    0x2AAA8A00u
#define USER_HANDOFF_ENTRY_WINDOW_END      0x2AAA8A40u

#define K24_DO_PAGE_FAULT         0x8000AFA8u
#define K24_HANDLE_TLBL           0x800133E0u
#define K24_NOPAGE_TLBL           0x80013464u
#define K24_HANDLE_TLBL_PTE_LOAD  0x80013410u
#define K24_HANDLE_TLBL_PTE_STORE 0x80013430u
#define K24_HANDLE_TLBS           0x80013560u
#define K24_DO_NO_PAGE            0x800283A4u
#define K24_FILEMAP_NOPAGE        0x8002DAB8u
#define K24_FILEMAP_FIND_GET_PAGE_RET   0x8002DBB4u
#define K24_FILEMAP_PAGE_CACHE_READ_RET 0x8002DBECu
#define K24_FILEMAP_READ_CLUSTER_RET    0x8002DC40u
#define K24_FILEMAP_UPTODATE_CHECK      0x8002DC48u
#define K24_FILEMAP_UPTODATE_BRANCH     0x8002DC54u
#define K24_FILEMAP_READPAGE_RET        0x8002DC94u
#define K24_FILEMAP_LOCKPAGE_CALL       0x8002DC5Cu
#define K24_FILEMAP_MAPPING_CHECK       0x8002DC64u
#define K24_FILEMAP_UPTODATE_RECHECK    0x8002DC7Cu
#define K24_FILEMAP_READPAGE_CALL       0x8002DC8Cu
#define K24_FILEMAP_SKIP_READPAGE       0x8002DCC4u
#define K24_BLOCK_READ_SUBMIT_BH_CALL   0x80042ED8u
#define K24_BLOCK_READ_SUBMIT_BH_RET    0x80042EE0u
#define K24_BLOCK_READ_GET_BLOCK_CALL   0x80042FB0u
#define K24_BLOCK_READ_GET_BLOCK_RET    0x80042FB8u
#define K24_BLOCK_READ_MEMSET_HOLE      0x80042FE8u
#define K24_BLOCK_READ_RET              0x80042EECu
#define K24_EXT2_GET_BLOCK_RET          0x8006E730u

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
#define VR4131_CONFIG    UINT32_C(0x10135923)
#define VR4131_WIRED     UINT32_C(0x00000002)
#define VR4131_STATUS_WARM UINT32_C(0x00008401)
#define VR4131_COUNT_WARM  UINT32_C(0x6FA5AFBF)
#define VR4131_COMPARE_WARM UINT32_C(0x6FA5F0B5)

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

/* WinCE diagnostic write tracker for key low-PA regions. */
typedef struct {
    uint32_t writes;
    uint32_t zero_writes;
    bool     first_valid;
    bool     saw_nonzero;
    uint32_t first_pa;
    uint32_t first_pc;
    uint8_t  first_size;
    uint64_t first_value;
    uint32_t last_pa;
    uint32_t last_pc;
    uint8_t  last_size;
    uint64_t last_value;
} wince_pa_watch_t;

#define WINCE_REGION_TRACK_COUNT 4u
typedef struct {
    uint32_t start_pa;
    uint32_t end_pa; /* exclusive */
    uint32_t writes;
    bool     first_valid;
    uint32_t first_pa;
    uint32_t first_pc;
    uint8_t  first_size;
    uint64_t first_value;
    bool     last_valid;
    uint32_t last_pa;
    uint32_t last_pc;
    uint8_t  last_size;
    uint64_t last_value;
    uint32_t nz_to_zero_count;
    bool     first_nz_to_zero_valid;
    uint32_t first_nz_to_zero_pa;
    uint32_t first_nz_to_zero_pc;
    uint8_t  first_nz_to_zero_size;
    uint64_t first_nz_to_zero_old;
    uint64_t first_nz_to_zero_new;
    bool     last_nz_to_zero_valid;
    uint32_t last_nz_to_zero_pa;
    uint32_t last_nz_to_zero_pc;
    uint8_t  last_nz_to_zero_size;
    uint64_t last_nz_to_zero_old;
    uint64_t last_nz_to_zero_new;
} wince_region_track_t;

#define WINCE_CTRL_HISTORY_LEN 128u
typedef struct {
    uint32_t pc;
    uint32_t target;
    uint32_t ra;
    uint32_t sp;
    uint32_t a0;
    uint32_t value;      /* status value for MTC0_STATUS, else 0 */
    uint8_t  kind;       /* 1=JAL, 2=JALR, 3=JR_RA, 4=MTC0_STATUS */
    uint8_t  reg_idx;    /* source GPR index for MTC0_STATUS, else 0 */
    uint16_t reserved;
} wince_ctrl_hist_entry_t;

#define WINCE_DIV_HISTORY_LEN 128u
typedef struct {
    uint32_t pc;
    uint32_t insn;
    uint32_t ra;
    uint32_t sp;
    uint32_t v0;
    uint32_t t9;
    uint32_t s0;
    uint32_t status;
    uint32_t stack20;
    uint32_t stack24;
    uint8_t  stack20_ok;
    uint8_t  stack24_ok;
    uint16_t reserved;
} wince_div_hist_entry_t;

typedef struct {
    bool     active;
    bool     completed;
    bool     seen_target;
    uint32_t call_pc;
    uint32_t target_pc;
    uint32_t return_pc;
    uint32_t sp_before_call;
    uint32_t sp_at_target;
    uint32_t sp_at_return;
    uint32_t sp_min;
    uint32_t sp_max;
    uint32_t events;
    uint32_t target_hits;
    uint32_t nested_calls;
    uint32_t repeat_hits;
    uint32_t step_logs;
    uint32_t last_pc;
    uint32_t last_sp;
    bool     last_valid;
    int32_t  sp_drift;
} wince_div_call_trace_t;

#define WINCE_DIV_STACK_WINDOW_COUNT 2u
#define WINCE_DIV_STACK_PHASE_COUNT 4u
#define WINCE_DIV_STACK_SLOT_COUNT 2u
typedef struct {
    uint32_t addr;
    uint32_t reads;
    uint32_t writes;
    bool     phase_valid[WINCE_DIV_STACK_PHASE_COUNT];
    uint32_t phase_value[WINCE_DIV_STACK_PHASE_COUNT];
    bool     first_read_valid;
    uint32_t first_read_pc;
    uint32_t first_read_addr;
    uint8_t  first_read_size;
    uint8_t  first_read_reg;
    uint64_t first_read_value;
    bool     last_read_valid;
    uint32_t last_read_pc;
    uint32_t last_read_addr;
    uint8_t  last_read_size;
    uint8_t  last_read_reg;
    uint64_t last_read_value;
    bool     first_valid;
    bool     first_old_valid;
    uint32_t first_pc;
    uint32_t first_addr;
    uint8_t  first_size;
    uint64_t first_old;
    uint64_t first_new;
    bool     last_valid;
    bool     last_old_valid;
    uint32_t last_pc;
    uint32_t last_addr;
    uint8_t  last_size;
    uint64_t last_old;
    uint64_t last_new;
} wince_div_stack_slot_t;

typedef struct {
    uint32_t base;
    uint32_t end; /* exclusive */
    uint32_t reads;
    uint32_t writes;
    bool     first_read_valid;
    uint32_t first_read_pc;
    uint32_t first_read_addr;
    uint8_t  first_read_size;
    uint8_t  first_read_reg;
    uint64_t first_read_value;
    bool     last_read_valid;
    uint32_t last_read_pc;
    uint32_t last_read_addr;
    uint8_t  last_read_size;
    uint8_t  last_read_reg;
    uint64_t last_read_value;
    bool     first_write_valid;
    bool     first_write_old_valid;
    uint32_t first_write_pc;
    uint32_t first_write_addr;
    uint8_t  first_write_size;
    uint64_t first_write_old;
    uint64_t first_write_new;
    bool     last_write_valid;
    bool     last_write_old_valid;
    uint32_t last_write_pc;
    uint32_t last_write_addr;
    uint8_t  last_write_size;
    uint64_t last_write_old;
    uint64_t last_write_new;
} wince_div_stack_window_trace_t;

typedef struct {
    bool     valid;
    uint32_t base;
    uint32_t size;
    uint32_t hash;
    uint32_t words;
    uint32_t valid_words;
    uint32_t nonzero_words;
} wince_div_frame_snapshot_t;

typedef struct {
    bool     active;
    uint32_t pc;
    uint32_t addr;
    uint8_t  size;
    uint8_t  reg_idx;
    uint8_t  slot_idx;
    uint8_t  window_idx;
} wince_div_pending_load_t;

typedef struct {
    bool     active;
    bool     completed;
    uint32_t arm_pc;
    uint32_t arm_sp;
    uint32_t caller_window_start;
    uint32_t caller_window_end; /* exclusive */
    uint32_t callee_window_start;
    uint32_t callee_window_end; /* exclusive */
    bool     callee_window_valid;
    uint32_t slot_logs;
    uint32_t window_logs;
    bool     phase_captured[WINCE_DIV_STACK_PHASE_COUNT];
    wince_div_frame_snapshot_t snapshots[WINCE_DIV_STACK_PHASE_COUNT][WINCE_DIV_STACK_WINDOW_COUNT];
    wince_div_stack_window_trace_t windows[WINCE_DIV_STACK_WINDOW_COUNT];
    wince_div_pending_load_t pending_load;
    wince_div_stack_slot_t slots[WINCE_DIV_STACK_SLOT_COUNT];
} wince_div_stack_watch_t;

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
    uint64_t cp0_count_ticks; /* per-instruction tick source for CP0 Count emulation */
    uint32_t cp0_count_base;
    uint32_t cp0_compare_shadow;
    bool     cp0_compare_shadow_valid;
    uint64_t rtc_last_count_tick; /* previous CP0 Count value used for RTC coupling */
    uint64_t rtc_tick_frac_num;   /* RTC fractional accumulator numerator */
    bool     running;
    /* Byte-addressable latch for unimplemented internal I/O offsets. */
    uint8_t  io_fallback_bytes[PA_IO_SIZE];
    uint8_t  io_fallback_valid[PA_IO_SIZE];
    mmio_hist_entry_t mmio_hist[WINCE_MMIO_HISTORY_LEN];
    uint32_t mmio_hist_head;
    uint32_t mmio_hist_count;
    bool     wince_pa_watch_active;
    uint32_t wince_pa_watch_logs;
    wince_pa_watch_t wince_vec_watch;
    wince_pa_watch_t wince_ctx_watch;
    uint32_t wince_cb_writes;
    bool     wince_cb_nonzero;
    uint32_t wince_objptr_writes;
    bool     wince_objptr_nonzero;
    uint32_t wince_obj_writes;
    bool     wince_obj_nonzero;
    wince_region_track_t wince_region_tracks[WINCE_REGION_TRACK_COUNT];
    uint32_t wince_region_nz2z_logs;
    wince_ctrl_hist_entry_t wince_ctrl_hist[WINCE_CTRL_HISTORY_LEN];
    uint32_t wince_ctrl_hist_head;
    uint32_t wince_ctrl_hist_count;
    wince_div_hist_entry_t wince_div_hist[WINCE_DIV_HISTORY_LEN];
    uint32_t wince_div_hist_head;
    uint32_t wince_div_hist_count;
    uint32_t wince_div_logs;
    wince_div_call_trace_t wince_div_call_trace;
    wince_div_stack_watch_t wince_div_stack_watch;
    uint32_t wince_ctx_probe_logs;
    uint32_t wince_null_last_ra;
    uint32_t wince_null_last_intno;
    uint32_t wince_null_consecutive;
    bool     wince_nk_epoch_reset_done;
    bool     wince_deferred_seed_done;
    uint32_t wince_fptbl_read_logs;
    bool     wince_fptbl_context_dumped;

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

/* Shared utilities: read/write a 32-bit word trying all PA/kseg alias addresses. */
void write_pa_u32_all_aliases(machine_t *m, uint32_t pa, uint32_t val);
bool read_pa_u32_all_aliases(machine_t *m, uint32_t pa, uint32_t *out);

/* Shared utility: read a 32-bit word from guest VA (tries both 32-bit and sign-extended). */
bool read_guest_u32(uc_engine *uc, uint64_t va, uint32_t *out);

/* Shared utility: read an instruction with PA fallback. */
bool read_insn_best_effort(uc_engine *uc, uint64_t address, uint32_t *insn);

/* Shared utility: read a NUL-terminated string from guest memory. */
int read_guest_string(uc_engine *uc, uint64_t va, char *buf, int bufsz);

/* ------------------------------------------------------------------ */
/* Shared inline helpers (used by machine.c and extracted modules)      */
/* ------------------------------------------------------------------ */

static inline uint64_t mips_sext(uint32_t va32) {
    return (uint64_t)(int32_t)va32;
}

static inline bool is_kseg_va32(uint32_t va32)
{
    uint32_t seg = va32 & 0xE0000000u;
    return seg == 0x80000000u || seg == 0xA0000000u;
}

static inline uint64_t tlb_pair_bytes_from_pagemask(uint32_t pagemask)
{
    uint64_t pair_bytes = ((uint64_t)(pagemask | 0x1FFFu) + 1u);
    if (pair_bytes < 0x2000u)
        pair_bytes = 0x2000u;
    if ((pair_bytes & 0xFFFu) != 0u)
        pair_bytes = (pair_bytes + 0xFFFu) & ~(uint64_t)0xFFFu;
    if ((pair_bytes & (pair_bytes - 1u)) != 0u) {
        uint64_t p2 = 0x2000u;
        while (p2 < pair_bytes && p2 < (1ull << 31))
            p2 <<= 1;
        pair_bytes = p2;
    }
    return pair_bytes;
}

static inline uint64_t tlb_leaf_bytes_from_pagemask(uint32_t pagemask)
{
    uint64_t pair_bytes = tlb_pair_bytes_from_pagemask(pagemask);
    if (pair_bytes < 0x2000u)
        return 0x1000u;
    return pair_bytes >> 1;
}

static inline bool tlb_entry_matches_va(uint32_t va, uint32_t entryhi, uint32_t pagemask)
{
    uint64_t pair_bytes = tlb_pair_bytes_from_pagemask(pagemask);
    uint32_t mask = ~((uint32_t)(pair_bytes - 1u));
    return ((va & mask) == (entryhi & mask));
}

static inline bool tlb_trace_window_active(const machine_t *m)
{
    return m->tlb_trace_window;
}

static inline bool va_to_pa_kseg(uint64_t va, uint64_t *pa_out)
{
    uint32_t va32 = (uint32_t)va;
    if ((va32 >= 0x80000000u && va32 <= 0xBFFFFFFFu) ||
        ((uint64_t)(int64_t)(int32_t)va32) == va) {
        *pa_out = (uint64_t)(va32 & 0x1FFFFFFFu);
        return true;
    }
    return false;
}

static inline void format_hex_bytes(const uint8_t *buf, size_t len,
                                    char *out, size_t out_sz)
{
    size_t pos = 0;
    if (out_sz == 0)
        return;
    out[0] = '\0';
    for (size_t i = 0; i < len; i++) {
        if (pos + 4 >= out_sz)
            break;
        int n = snprintf(out + pos, out_sz - pos, "%02X", buf[i]);
        if (n <= 0)
            break;
        pos += (size_t)n;
        if (i + 1 < len && pos + 2 < out_sz)
            out[pos++] = ' ';
    }
    if (pos < out_sz)
        out[pos] = '\0';
}

static inline bool is_wince_boot_cfg(const machine_config_t *cfg)
{
    return cfg != NULL && cfg->nand_path != NULL;
}

static inline bool is_wince_boot_machine(const machine_t *m)
{
    return m != NULL && is_wince_boot_cfg(&m->cfg);
}
