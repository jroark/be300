#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL2/SDL.h>

#include "be300_probe.h"

#include "cop0.h"
#include "cpu.h"
#include "cpu_mips.h"
#include "machine.h"
#include "memory.h"

typedef struct {
    uint64_t pc;
    const char *label;
    uint32_t hits;
} exec_watch_t;

typedef struct {
    uint64_t start;
    uint64_t end;
    const char *label;
    bool log_reads;
    bool log_writes;
    uint32_t read_hits;
    uint32_t write_hits;
} mem_watch_t;

static struct {
    bool enabled;
    struct machine *machine;
    uint32_t launcher_state_dumps;
    uint32_t launcher_notify_dumps;
    uint32_t loader_logs;
    uint32_t module_walk_done;
    uint32_t t5_dump_done;
    uint32_t gdi_dumped;

    /* W1 — MMIO coverage (default off, enabled by --mmio-coverage). */
    bool mmio_coverage;

    /* W4 — stuck-PC detector (default off, enabled by --detect-stall). */
    bool     detect_stall;
    uint32_t stall_window;            /* instructions per sampling bucket */
    uint32_t stall_unique_threshold;  /* fire when unique PCs drop below this */
    uint32_t stall_wall_secs;         /* sustained wall-seconds before firing */
} g_probe;

/*
 * W1 — MMIO first-hit coverage map. Open-addressing hash table, keyed on
 * (dev-string-pointer-mix, offset, op). Zero-cost when g_probe.mmio_coverage
 * is false. Dumped by be300_probe_detach at shutdown.
 */
#define BE300_MMIO_COV_CAP 4096u   /* power of two; ~3000 usable slots */

typedef struct {
    const char *dev;       /* NULL = empty slot */
    uint32_t    off;
    uint32_t    first_pc;
    uint32_t    hits;
    uint32_t    last_value;
    uint16_t    len;
    uint8_t     op;        /* 'R' or 'W' */
    uint8_t     mclass;    /* enum be300_mmio_class */
} mmio_cov_entry_t;

static mmio_cov_entry_t g_mmio_cov[BE300_MMIO_COV_CAP];
static uint32_t         g_mmio_cov_used;
static uint32_t         g_mmio_cov_overflow;

static const char *be300_mmio_class_name(int mclass)
{
    switch (mclass) {
    case BE300_MMIO_CLASS_KNOWN:   return "known";
    case BE300_MMIO_CLASS_STUBBED: return "stubbed";
    case BE300_MMIO_CLASS_LATCHED: return "latched";
    case BE300_MMIO_CLASS_DEFAULT: return "default";
    default:                       return "?";
    }
}

static uint32_t be300_mmio_hash(const char *dev, uint32_t off, char op)
{
    uint32_t h = 5381u;
    const unsigned char *p;
    if (dev) {
        for (p = (const unsigned char *)dev; *p; p++)
            h = ((h << 5) + h) ^ *p;
    }
    h ^= off * 2654435761u;
    h ^= (uint32_t)(unsigned char)op;
    return h & (BE300_MMIO_COV_CAP - 1u);
}

/*
 * W4 — stuck-PC sampler. Per-bucket hash-set of unique PCs; at bucket end we
 * count unique entries and, if it stays below threshold for stall_wall_secs
 * of wall time, emit one [BE300_STALL] line (throttled to one per 30s).
 * Zero-cost when g_probe.detect_stall is false.
 */
#define BE300_STALL_PC_CAP 4096u   /* power of two */

static struct {
    uint32_t pcs[BE300_STALL_PC_CAP];   /* 0 = empty (PC=0 is not a real guest PC) */
    uint32_t inserted;                  /* distinct PCs in current bucket */
    uint64_t insn_in_bucket;
    uint64_t low_run_start_ns;          /* wall-time when consecutive-low started, 0 = not in low run */
    uint32_t consecutive_high_buckets;  /* hysteresis: only reset low-run after N consecutive highs */
    uint64_t last_fire_ns;
    uint32_t fire_count;
    uint32_t last_fire_pc;
} g_stall;

/*
 * Hysteresis: guest scheduler/timer activity periodically injects "high unique"
 * buckets into an otherwise spin-dominated boot. Require several consecutive
 * high buckets before we decide the stall is actually over. At 200K insn/bucket
 * on a 166MHz target this is ~4.8ms per bucket, so 8 = ~40ms — enough to bridge
 * normal timer/ISR bursts while still recovering quickly from genuine progress.
 */
#define BE300_STALL_HIGH_HYSTERESIS 8u

static uint64_t be300_probe_wall_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t be300_probe_norm_addr(uint64_t addr)
{
    return (uint64_t)(uint32_t)addr;
}

static exec_watch_t g_exec_watches[] = {
    { 0x01F84A5Cu, "coredll_dllmain_entry_user", 0 },
    { 0x80080AA4u, "launcher_wait_loop", 0 },
    { 0x80080CB4u, "launcher_blocking_wait_call", 0 },
    { 0x80080D38u, "launcher_module_ready_notify", 0 },
    { 0x8008130Cu, "event_modify_set_reset_pulse", 0 },
    { 0x8008690Cu, "spawn_module_createprocess_path", 0 },
    { 0x8008FF00u, "dllmain_dispatch", 0 },
    { 0x800927CCu, "loader_entry", 0 },
    { 0x800929D0u, "loader_post_dllmain", 0 },
    { 0x800929E4u, "loader_rollback_done", 0 },
    { 0x80096094u, "boot_callback_registration", 0 },
    { 0x80096620u, "boot_thread_main", 0 },
    { 0x800BFA5Cu, "coredll_dll_process_attach", 0 },
    { 0x80082300u, "boot_trampoline_entry", 0 },
    { 0x80082560u, "boot_trampoline_createevent_for_wfm", 0 },
    { 0x800825C4u, "boot_trampoline_spawn_launcher", 0 },
    { 0x80082600u, "boot_trampoline_yield_to_scheduler", 0 },
    { 0x80082618u, "boot_trampoline_wfm_call", 0 },
    { 0x80082690u, "boot_trampoline_post_wfm", 0 },
    { 0x800826E0u, "boot_trampoline_post_wfm_pulse", 0 },
    { 0x80086884u, "signal_boot_ready_pulser", 0 },
    { 0x80082248u, "spawn_thread_alloc_helper", 0 },

    /*
     * Pass 16 verification: prove or refute the WaitForAPIReady(21)
     * deadlock hypothesis (Pass 15.A). Trace the full pcmcia.dll DllMain
     * call chain to identify where execution actually stops.
     *
     *   pcmcia_dllmain_entry @ 0x01981ac4 — pcmcia.dll DllMain (E32 entryRVA 0x1ac4)
     *   pcmcia_init_entry    @ 0x019817b4 — PCMCIA_init (DllMain calls this on PROCESS_ATTACH)
     *   pcmcia_phase1_entry  @ 0x019890dc — PCMCIA_init_phase1 (suspect)
     *   pcmcia_phase1_wait_api21_call   @ 0x01989364 — jal 0x198d7c4 (the wait)
     *   pcmcia_phase1_wait_api21_return @ 0x0198936c — first instr after the wait returns
     *
     * Outcomes:
     *   If dllmain doesn't fire: pcmcia.dll never loaded (refutes Pass 9).
     *   If dllmain fires but init doesn't: DllMain returns early (DLL_THREAD_ATTACH path).
     *   If init fires but phase1 doesn't: init bails before phase1 (e.g., GetSystemTime fails).
     *   If phase1 fires but wait_api21_call doesn't: phase1's earlier checks bail out.
     *   If wait_api21_call fires but wait_api21_return doesn't: the wait blocks forever (CONFIRMS hypothesis).
     */
    { 0x01981ac4u, "pcmcia_dllmain_entry",            0 },
    { 0x019817b4u, "pcmcia_init_entry",               0 },
    { 0x019890dcu, "pcmcia_phase1_entry",             0 },
    /* PC right after FUNC_A (jal 0x198d7b4) returns — v0 holds FUNC_A's return.
     * If non-zero, phase1 takes the bnez branch to the error exit. */
    { 0x01989120u, "pcmcia_phase1_after_funcA",       0 },
    /* PC after FUNC_C (jal 0x198a9c8 = device enumerator) returns — v0 = device-enumerator ptr.
     * If 0, phase1 takes the beql branch to error exit. */
    { 0x01989144u, "pcmcia_phase1_after_funcC",       0 },
    /* PC right before per-device init loop. v0 holds device count (DAT_0198e0d4 read).
     * If 0, no devices were enumerated → phase1 returns 0x20 without WaitForAPIReady. */
    { 0x019891f8u, "pcmcia_phase1_check_devcount",    0 },
    /* PC of the WaitForAPIReady(21) call site. */
    { 0x01989364u, "pcmcia_phase1_wait_api21_call",   0 },
    { 0x0198936Cu, "pcmcia_phase1_wait_api21_return", 0 },
    /* phase1 error exit (taken on FUNC_A != 0, FUNC_C == 0, etc.) */
    { 0x01989394u, "pcmcia_phase1_exit_error",        0 },

    /*
     * Pass 18 (2026-04-19): nanddisk.dll CheckDMAEnd entry + print site.
     * Body reads *(a1+4) then polls bit 0 of *(that+0x10) for 1000 iters.
     * If always set, prints "CheckDMAEnd: Error timeout" (0x019A1D68).
     */
    { 0x019A55B0u, "nanddisk_check_dma_end_entry",    0 },
    { 0x019A55ECu, "nanddisk_check_dma_end_timeout_print", 0 },

    /*
     * Pass 20 (2026-04-19): NandColdBoot (UM 0x019A3B24..0x019A3DD0)
     * entry + all 3 v0-setting sites. Disassembly shows the function
     * has ONE `jr ra` at 0x019A3DCC and a shared epilogue starting at
     * 0x019A3DA4. First probe run confirmed: function called once,
     * returned v0=0 → caller (ra=0x019A292C) prints "NandColdBoot
     * Error!!!". Two branches set v0=0 and one sets v0=1:
     *
     *   0x019A3D64: move v0,zero (delay slot of b 0x019A3DA4)
     *     reached from a branch around 0x019A3D50 area — retry limit
     *   0x019A3D84: move v0,zero (delay slot of b 0x019A3DA4)
     *     reached via beql path around 0x019A3D70 — scan bailout
     *   0x019A3DA0: li v0,1 (fall through to epilogue) — success
     *
     * The Pass-20 explorer's disassembly report was wrong about
     * 0x019A3ECC and 0x019A4050 being NandColdBoot returns — those
     * addresses belong to separate functions starting at 0x019A3DD4
     * and later. Addresses below are all inside the true function.
     */
    { 0x019A3B24u, "nanddisk_nandcoldboot_entry",        0 },
    /*
     * Pass 20 outcome so far: function runs once, scans blocks via
     * outer loop (533 iterations), all 533 ReadHelper calls return
     * v0=1 (data delivery works). But t2 decremented 21 times, hitting
     * the v0=0 exit via site A. t2 only decrements on the path:
     *   a2<3 (bnez at 0x19A3C88) → a3<3 (bnez at 0x19A3CFC) → t1 retry
     *   (slti 0x19A3D20 beql) → after retries exhaust (t1>=2), t2--.
     * Watches at the two validity-check bnez'es log s8+t0+t1+t2 so
     * we can identify WHICH blocks are failing OOB validation and
     * count retries. 8-hit cap is fine — expect ~21 total failures.
     */
    { 0x019A3C88u, "nanddisk_ncb_bnez_a2_lt_3", 0 }, /* bnez at(a2<3) → 0x19A3CF8 */
    { 0x019A3CFCu, "nanddisk_ncb_bnez_a3_lt_3", 0 }, /* bnez at(a3<3) → 0x19A3D20 */
    { 0x019A3D60u, "nanddisk_ncb_b_to_epi_A",   0 }, /* b→epi (delay: v0=0) */
    { 0x019A3DCCu, "nanddisk_ncb_return",       0 }, /* jr ra */

    /*
     * Pass 21 Objective 2: Exception 003 at UM 0x019A3D14 triage.
     * `sh t0, 0(t9)` where t9 = *(0x019AB34C) + v0. Pass 20 handoff
     * hypothesis: 0x019AB34C's pointer is still zero — benign lazy
     * fault. This watch dumps t9/t0/ra at the faulting instruction.
     */
    { 0x019A3D14u, "nanddisk_blank_bat_store", 0 },

    /*
     * Pass 24 (2026-04-19): clblib.dll (Casio Common Library) is
     * confirmed as the module at 0x019B0000. The thread holding
     * memmgr CS 0x806695A0 has saved user PCs at these offsets —
     * count hits to see which function is active, and whether it is
     * a one-shot init stuck on a blocking syscall or a repeated
     * polling loop.
     */
    { 0x019B1040u, "clblib_user_pc_1040", 0 },
    { 0x019B17CCu, "clblib_user_pc_17cc", 0 },
    { 0x019B2038u, "clblib_user_pc_2038", 0 },

    /* Pass 30: inside Boot.exe's CASIO reboot-wait loop. PC 0x800A8788 is
     * `sltu at, t6, t7` where t6 = current_tick - *PA_0x2644 and t7 = Wait.
     * Special-case dump of t5/t6/t7 at this PC reveals the runtime Wait
     * value and the computed diff; combined with TimeOut at sp+0x48 it
     * pins whether the loop exits via Wait or TimeOut. */
    { 0x800A8788u, "reboot_wait_loop_compare", 0 },
    /* FUN_8007a140 -- post-reboot-wait VRC4173/state stabilisation call
     * that Pass 30 decomp identified as the real hang: disables
     * interrupts, writes 0x0A00A0C4=7 / 0x0A00A0C8=10, then polls
     * _DAT_80660030 in a do-while until stable. If entry fires but
     * return doesn't, the poll loop never terminates. */
    { 0x8007A140u, "post_reboot_vrc_stabilise_entry",  0 },
    { 0x8007A174u, "post_reboot_after_vrc_c8_store",   0 },
    { 0x8007A178u, "post_reboot_suspected_self_jal",   0 },
    { 0x8007A180u, "post_reboot_next_func_prologue",   0 },
    { 0x8007A1A4u, "post_reboot_loop_bottom",          0 },
    { 0x8007A1C4u, "post_reboot_loop_beql_back",       0 },
    { 0x8007A1FCu, "post_reboot_function_jr_ra",       0 },

    /*
     * Pass 32 (2026-04-20): post-Pass-31 gwes stall triage.
     *
     * gwes.exe runs in its process's slot 0 (base 0x00010000). Ghidra
     * analysis (slot-2 relocation at 0x04010000) identified:
     *   - gwes WinMain at 0x00016394 (Ghidra 0x04016394)
     *   - Last init function: FUN_0x00034b68 — creates events A/B/C,
     *     CreateThread(entry 0x000348d4), SetEvent(C), then blocks
     *     on WaitForSingleObject(event_B, INFINITE).
     *   - Worker thread FUN_0x000348d4 — waits on event C, then does
     *     font/bitmap/window init (via FUN_0x0002a4c4 =
     *     CreateDialogFromResource ~300 lines, heavy SendMessage
     *     traffic), then SetEvent(B) to unblock WinMain.
     *
     * Expected hit pattern on a healthy boot (unreachable because the
     * emulator is pre-healthy):
     *   winmain_entry >= 1, last_init_entry >= 1, worker_entry >= 1,
     *   window_create_entry >= 1, message_loop >= 1.
     *
     * Stall hypothesis: worker's window-create blocks, never reaches
     * SetEvent(B), main thread parks in WFM.
     *
     * Diagnosis table after a 60s run:
     *   winmain hits, last_init_entry=0   -> stall is elsewhere upstream
     *   last_init_entry=1, worker_entry=0 -> CreateThread failed
     *   worker_entry=1, window_create=0   -> stall in pre-window worker init
     *   window_create=1, message_loop=0   -> stall inside CreateDialog chain
     *   message_loop>=1                   -> gwes advanced; stall is elsewhere
     *
     * Noise: other processes live at 0x00010000+ too and may have code
     * incidentally at these offsets. Hit counts > 10 for gwes-specific
     * labels indicate the right process. Offsets like 0x00016394 are
     * unlikely to collide because each EXE has different .text layout.
     */
    { 0x00016394u, "gwes_winmain_entry",                0 },
    { 0x00034B68u, "gwes_last_init_entry",              0 },
    { 0x000348D4u, "gwes_worker_thread_entry",          0 },
    { 0x0002A4C4u, "gwes_window_create_entry",          0 },
    { 0x00035928u, "gwes_message_loop_entry",           0 },

    /*
     * Pass 32 addendum 3: does gwes actually load the user-mode display
     * driver? ddi.dll vbase 0x01A50000, vsize 0x01A000 per
     * build-host/modules/index.txt. Exec-watch the first instruction of
     * each DDI-family DLL's .text (all at +0x1000 from vbase — standard
     * WinCE XIP layout). Hit counts on any of these mean the DLL's code
     * got mapped and at least one thread executed it (DllMain or similar).
     *
     * Zero hits on ddi.dll means gwes never loads the display driver →
     * confirms the addendum-3 hypothesis.
     */
    { 0x01A51000u, "ddi_dll_text_entry",           0 },
    { 0x01B91000u, "ddhel_dll_text_entry",         0 },
    { 0x01BC1000u, "ddstub_dll_text_entry",        0 },
    { 0x01BD1000u, "ddcore_dll_text_entry",        0 },
    { 0x01C21000u, "ddraw_dll_text_entry",         0 },

    /* Pass 32 addendum 4: sample DDI.DLL .text at 0x1000 intervals to see
     * if code runs ANYWHERE inside the module (DllMain may not be at byte 0). */
    { 0x01A52000u, "ddi_dll_text_1k",              0 },
    { 0x01A54000u, "ddi_dll_text_4k",              0 },
    { 0x01A58000u, "ddi_dll_text_8k",              0 },
    { 0x01A5C000u, "ddi_dll_text_c000",            0 },
    { 0x01A60000u, "ddi_dll_text_10000",           0 },
    { 0x01A64000u, "ddi_dll_text_14000",           0 },

    /*
     * Pass 32 addendum 7: pinned function-entry probes in ddi.dll.
     * FUN_01A540B8 is the DrvEnableDriver export (1-line tail-call
     * thunk to impl). FUN_01A5C43C is the real impl (verified by
     * WinCE 3.0 signature check: param_1==0x20001 /
     * DDI_DRIVER_VERSION, param_2==0x6C /
     * sizeof(DRVENABLEDATA)). FUN_01A5BF00 is the 1 Hz blit
     * dispatcher — reading its entry args reveals the surface
     * object being drawn to.
     */
    { 0x01A540B8u, "ddi_DrvEnableDriver_export",   0 },
    { 0x01A5C43Cu, "ddi_DrvEnableDriver_impl",     0 },
    { 0x01A5BF00u, "ddi_blit_dispatcher_entry",    0 },

    /*
     * Pass 32 addendum (2026-04-22): gwes worker thread's event-handle
     * traffic. Per Ghidra decompile of gwes_worker_REACHED_OK_Pass32
     * (runtime PC 0x000348d4), the worker loop is:
     *   do {
     *     WFSO(0x000B6834, INFINITE);   // *_wfso_6834_entry
     *     <ret from coredll WFSO>       // *_wfso_6834_ret
     *     ResetEvent(0x000B6834);       // *_reset_6834
     *     ... prep, CreateWindow ...
     *     SetEvent(0x000B6830);         // *_set_6830
     *     WFSO(0x000B6824, INFINITE);   // *_wfso_6824_entry
     *     <ret>                         // *_wfso_6824_ret
     *     ... GetMessage loop ...
     *   } while (true);
     * If the worker is stuck at either WFSO, the "_ret" hit count will be
     * strictly less than the corresponding "_entry" count for that wait.
     */
    { 0x00034928u, "gwes_worker_wfso_6834_entry", 0 },
    { 0x0003492Cu, "gwes_worker_wfso_6834_ret",   0 },
    { 0x00034938u, "gwes_worker_reset_6834",      0 },
    { 0x00034A4Cu, "gwes_worker_set_6830",        0 },
    { 0x00034A68u, "gwes_worker_wfso_6824_entry", 0 },
    { 0x00034A6Cu, "gwes_worker_wfso_6824_ret",   0 },

    /*
     * Pass 32 §11 two-probe set (2026-04-22):
     *
     * Inside gwes_last_init the key EventModify calls are at PCs:
     *   0x00034d5c: EventModify(0x6824, 2)  = RESET 0x6824
     *   0x00034d6c: EventModify(0x6830, 2)  = RESET 0x6830
     *   0x00034d7c: EventModify(0x6834, 3)  = SET   0x6834  ← KEY
     *   0x00034d94: WFSO(0x6830, INFINITE)  ← init blocks here if worker
     *                                         never signals 0x6830
     * And the gate before all of these:
     *   0x00034d48: jal 0x00075530  — if this returns 0, init takes the
     *                                  early-return path and SetEvent never
     *                                  fires.
     *
     * Case A (hits=0 for *setevent_6834*): init early-returns before
     *   setting 0x6834 → worker hangs because nobody signals it.
     * Case B (hits>=2 for *setevent_6834* but worker WFSO still doesn't
     *   return): the SetEvent reaches coredll EventModify but doesn't
     *   wake the worker — scheduler/event-state emulation bug.
     */
    { 0x00034D48u, "gwes_init_jal_75530",         0 },
    { 0x00034D5Cu, "gwes_init_reset_6824",        0 },
    { 0x00034D6Cu, "gwes_init_reset_6830",        0 },
    { 0x00034D7Cu, "gwes_init_setevent_6834",     0 },
    { 0x00034D94u, "gwes_init_wfso_6830",         0 },

    /*
     * Pass 32 §12 (2026-04-22): func_0x00075530 decides whether gwes_last_init
     * reaches SetEvent(0x6834). Probe its 4 return paths to find the
     * specific branch that returns 0:
     *   0x00075530: fn entry
     *   0x00075584/0x000755D4/0x00075624: alloc-failure early returns
     *   0x00075648: just before jal func_0x000b1814
     *   0x00075654: post-return, v0 = func_0x000b1814's return
     *                            (case A: non-zero -> TRUE)
     *   0x00075670: sltu on Owner[0x22a]   (case B)
     *   0x00075684: sltu on Notes[0x182]   (case C)
     *   0x00075688: final return (log v0)
     */
    { 0x00075530u, "fn75530_entry",               0 },
    { 0x00075584u, "fn75530_alloc1_fail",         0 },
    { 0x000755D4u, "fn75530_alloc2_fail",         0 },
    { 0x00075624u, "fn75530_alloc3_fail",         0 },
    { 0x00075640u, "fn75530_jal_75ef4",           0 },
    { 0x00075648u, "fn75530_jal_b1814",           0 },
    { 0x00075654u, "fn75530_after_b1814",         0 },
    { 0x00075670u, "fn75530_sltu_owner_22a",      0 },
    { 0x00075684u, "fn75530_sltu_notes_182",      0 },
    { 0x00075688u, "fn75530_final_return",        0 },

    /*
     * Pass 32 §12: coredll stub at 0x01F8B4D0 is the target of IAT
     * slot 0xB32BC. Trace its entry, the jalr to the kernel sentinel
     * (0xFFFFAB46), and the post-return. If the jalr fires but post-
     * return doesn't, the kernel callback is silently dropping the
     * API call.
     */
    { 0x01F8B4D0u, "cdll_b4d0_entry",             0 },
    { 0x01F8B4DCu, "cdll_b4d0_jalr",              0 },
    { 0x01F8B4E4u, "cdll_b4d0_post_return",       0 },
    { 0x01F8B4E8u, "cdll_b4d0_return_to_caller",  0 },

    /*
     * coredll EventModify entry — every call. We use a hit cap of 256 so
     * we can see all early calls. The dispatcher branch for this PC
     * decodes a0 (handle) and a1 (op) explicitly for readability.
     */
    { 0x000B1054u, "coredll_eventmodify_entry",   0 },

    /*
     * Pass 32 addendum 7 follow-up (Stage 1a): GDI caller of
     * DrvEnableDriver, observed ra=0x00049D50 → this is the
     * instruction after the call. Sweep a window of 13 PCs at
     * 4-byte stride to see how far the caller progresses after
     * DrvEnableDriver returns. If only the first few fire, GDI
     * init bails early (Hypothesis 1). If all fire, GDI is
     * attempting the full DrvEnablePDEV / DrvCompletePDEV /
     * DrvEnableSurface dispatch and the failure is deeper.
     *
     * Gwes is ASID 4 per addendum 7; entryhi in the log line
     * lets us filter noise from other slot-0 code.
     */
    { 0x00049D50u, "gwes_gdi_after_drvenabledriver",  0 },
    { 0x00049D54u, "gwes_gdi_49d54",                  0 },
    { 0x00049D58u, "gwes_gdi_49d58",                  0 },
    { 0x00049D5Cu, "gwes_gdi_49d5c",                  0 },
    { 0x00049D60u, "gwes_gdi_49d60",                  0 },
    { 0x00049D64u, "gwes_gdi_49d64",                  0 },
    { 0x00049D68u, "gwes_gdi_49d68",                  0 },
    { 0x00049D6Cu, "gwes_gdi_49d6c",                  0 },
    { 0x00049D70u, "gwes_gdi_49d70",                  0 },
    { 0x00049D74u, "gwes_gdi_49d74",                  0 },
    { 0x00049D78u, "gwes_gdi_49d78",                  0 },
    { 0x00049D7Cu, "gwes_gdi_49d7c",                  0 },
    { 0x00049D80u, "gwes_gdi_49d80",                  0 },

    /* Wider sweep past the initial 0x30-byte window (32-byte stride, 20
     * probes covering 0x49D80..0x4A080 = 0x300 bytes). Hit counts will
     * pinpoint where GDI init stops or loops. */
    { 0x00049DA0u, "gwes_gdi_49da0",                  0 },
    { 0x00049DC0u, "gwes_gdi_49dc0",                  0 },
    { 0x00049DE0u, "gwes_gdi_49de0",                  0 },
    { 0x00049E00u, "gwes_gdi_49e00",                  0 },
    { 0x00049E20u, "gwes_gdi_49e20",                  0 },
    { 0x00049E40u, "gwes_gdi_49e40",                  0 },
    { 0x00049E60u, "gwes_gdi_49e60",                  0 },
    { 0x00049E80u, "gwes_gdi_49e80",                  0 },
    { 0x00049EA0u, "gwes_gdi_49ea0",                  0 },
    { 0x00049EC0u, "gwes_gdi_49ec0",                  0 },
    { 0x00049EE0u, "gwes_gdi_49ee0",                  0 },
    { 0x00049F00u, "gwes_gdi_49f00",                  0 },
    { 0x00049F20u, "gwes_gdi_49f20",                  0 },
    { 0x00049F40u, "gwes_gdi_49f40",                  0 },
    { 0x00049F60u, "gwes_gdi_49f60",                  0 },
    { 0x00049F80u, "gwes_gdi_49f80",                  0 },
    { 0x00049FA0u, "gwes_gdi_49fa0",                  0 },
    { 0x00049FC0u, "gwes_gdi_49fc0",                  0 },
    { 0x00049FE0u, "gwes_gdi_49fe0",                  0 },
    { 0x0004A000u, "gwes_gdi_4a000",                  0 },

    /* Fine sweep inside ddi.dll around DrvEnableDriver_impl at 0x01A5C43C.
     * DrvEnablePDEV, DrvCompletePDEV, and DrvEnableSurface are likely
     * neighbours (WinCE driver authors typically group them). Stride 0x80
     * covers 0x01A5C000..0x01A5D000 (32 probes worth, 16 here)
     * and 0x01A5A000..0x01A5C000 (backwards). */
    { 0x01A5A000u, "ddi_fine_5a000",                  0 },
    { 0x01A5A400u, "ddi_fine_5a400",                  0 },
    { 0x01A5A800u, "ddi_fine_5a800",                  0 },
    { 0x01A5AC00u, "ddi_fine_5ac00",                  0 },
    { 0x01A5B000u, "ddi_fine_5b000",                  0 },
    { 0x01A5B400u, "ddi_fine_5b400",                  0 },
    { 0x01A5B800u, "ddi_fine_5b800",                  0 },
    { 0x01A5BC00u, "ddi_fine_5bc00",                  0 },
    { 0x01A5C400u, "ddi_fine_5c400",                  0 },
    { 0x01A5C800u, "ddi_fine_5c800",                  0 },
    { 0x01A5CC00u, "ddi_fine_5cc00",                  0 },
    { 0x01A5D000u, "ddi_fine_5d000",                  0 },
    { 0x01A5D400u, "ddi_fine_5d400",                  0 },
    { 0x01A5D800u, "ddi_fine_5d800",                  0 },
    { 0x01A5DC00u, "ddi_fine_5dc00",                  0 },
    { 0x01A5E000u, "ddi_fine_5e000",                  0 },
    { 0x01A5E400u, "ddi_fine_5e400",                  0 },
    { 0x01A5E800u, "ddi_fine_5e800",                  0 },
    { 0x01A5EC00u, "ddi_fine_5ec00",                  0 },
    { 0x01A5F000u, "ddi_fine_5f000",                  0 },

    /*
     * Pass 32 Stage 1 round 3: the 27 iFunc pointers DDI writes into
     * ppdded. Observed GDI reads touch slots 0, 2, 4, 5, 10, 17, 18, 21
     * but NEVER slot 3 (DrvEnableSurface = 0x01A5D228). Probing each
     * iFunc entry tells us which actually execute. Slot 0 (suspected
     * DrvEnablePDEV at 0x01A5D2F0) is the most important — if it
     * returns NULL, GDI would skip EnableSurface and short-circuit
     * to DisablePDEV (slot 2 = 0x01A5D254), which matches observation.
     */
    { 0x01A5D2F0u, "ddi_iFunc0_DrvEnablePDEV_guess",  0 },
    { 0x01A5D2B4u, "ddi_iFunc1",                      0 },
    { 0x01A5D254u, "ddi_iFunc2_DrvDisablePDEV_guess", 0 },
    { 0x01A5D228u, "ddi_iFunc3_DrvEnableSurface_guess", 0 },
    { 0x01A5D18Cu, "ddi_iFunc4",                      0 },
    { 0x01A5D13Cu, "ddi_iFunc5",                      0 },
    { 0x01A5CEFCu, "ddi_iFunc6",                      0 },
    { 0x01A5FA80u, "ddi_iFunc7",                      0 },
    { 0x01A5CB00u, "ddi_iFunc8",                      0 },
    { 0x01A5CA78u, "ddi_iFunc9",                      0 },
    { 0x01A5C9CCu, "ddi_iFunc10",                     0 },
    { 0x01A5ECB4u, "ddi_iFunc11",                     0 },
    { 0x01A5C974u, "ddi_iFunc12",                     0 },
    { 0x01A5C90Cu, "ddi_iFunc13",                     0 },
    { 0x01A5C884u, "ddi_iFunc14",                     0 },
    { 0x01A5C774u, "ddi_iFunc15",                     0 },
    { 0x01A5C744u, "ddi_iFunc16",                     0 },
    { 0x01A5C604u, "ddi_iFunc17",                     0 },
    { 0x01A60690u, "ddi_iFunc18",                     0 },
    { 0x01A540ACu, "ddi_iFunc19",                     0 },
    { 0x01A6054Cu, "ddi_iFunc20",                     0 },
    { 0x01A5C5E0u, "ddi_iFunc21",                     0 },
    { 0x01A5C5BCu, "ddi_iFunc22",                     0 },
    { 0x01A5C580u, "ddi_iFunc26",                     0 },

    /* Fine sweep inside DrvEnablePDEV_guess (0x01A5D2F0) to see how far it
     * executes. If the function body is short (< 0x80 bytes) it's
     * probably returning NULL for a missing resource. */
    { 0x01A5D2F4u, "pdev_4",                          0 },
    { 0x01A5D2FCu, "pdev_c",                          0 },
    { 0x01A5D304u, "pdev_14",                         0 },
    { 0x01A5D310u, "pdev_20",                         0 },
    { 0x01A5D320u, "pdev_30",                         0 },
    { 0x01A5D340u, "pdev_50",                         0 },
    { 0x01A5D360u, "pdev_70",                         0 },
    { 0x01A5D380u, "pdev_90",                         0 },
    { 0x01A5D3A0u, "pdev_b0",                         0 },
    { 0x01A5D3C0u, "pdev_d0",                         0 },

    /* Denser window around the observed last-hit pdev_90 (0x01A5D380) and
     * the pre-hit pdev_b0 (0x01A5D3A0). The return PC is between these. */
    { 0x01A5D384u, "pdev_94",                         0 },
    { 0x01A5D388u, "pdev_98",                         0 },
    { 0x01A5D38Cu, "pdev_9c",                         0 },
    { 0x01A5D390u, "pdev_a0",                         0 },
    { 0x01A5D394u, "pdev_a4",                         0 },
    { 0x01A5D398u, "pdev_a8",                         0 },
    { 0x01A5D39Cu, "pdev_ac",                         0 },

    /* Fine sweep of early body to see how far into the prologue we branch.
     * If we hit a conditional early and branch to a returns-0 tail, that's
     * the failure. */
    { 0x01A5D2F8u, "pdev_8",                          0 },
    { 0x01A5D300u, "pdev_10",                         0 },
    { 0x01A5D308u, "pdev_18",                         0 },
    { 0x01A5D30Cu, "pdev_1c",                         0 },
    { 0x01A5D314u, "pdev_24",                         0 },
    { 0x01A5D318u, "pdev_28",                         0 },
    { 0x01A5D31Cu, "pdev_2c",                         0 },
    { 0x01A5D328u, "pdev_38",                         0 },
    { 0x01A5D330u, "pdev_40",                         0 },
    { 0x01A5D338u, "pdev_48",                         0 },
    { 0x01A5D348u, "pdev_58",                         0 },
    { 0x01A5D350u, "pdev_60",                         0 },
    { 0x01A5D358u, "pdev_68",                         0 },
    { 0x01A5D368u, "pdev_78",                         0 },
    { 0x01A5D370u, "pdev_80",                         0 },
    { 0x01A5D378u, "pdev_88",                         0 },
    /* DrvEnablePDEV return points: 0x01A5D444 is the shared jr ra epilogue
     * for both success and error paths; v0 at this PC is the return value. */
    { 0x01A5D444u, "pdev_return_jr_ra",               0 },
    { 0x01A5D440u, "pdev_return_success_v0_move",     0 },
    /* iFunc 17 = DrvGetModes. vtable[9] returns mode count at 0x01A5C650.
     * Probe at the instruction RIGHT AFTER the jalr to capture v0 = mode
     * count. If 0 → no modes → hdev+188 stays zero → DrvEnablePDEV's
     * vtable[10] palette creation sees missing mode data. */
    { 0x01A5C654u, "drvgetmodes_after_modecount",     0 },
    /* Probe after vtable[8] returns (inside DrvGetModes loop, at
     * 0x01A5C6C4 we're in the jalr delay slot; the continue-after-return
     * target is 0x01A5C6C8). */
    { 0x01A5C6C8u, "drvgetmodes_after_vtable8",       0 },
    /* blez s8 check — if mode count 0, branch taken to 0x01A5C710. */
    { 0x01A5C690u, "drvgetmodes_blez_modecount",      0 },
    { 0x01A5C710u, "drvgetmodes_after_loop",          0 },
    /* vtable[8] and [9] located. Probe their entries to confirm
     * execution and capture return values. */
    { 0x01A54264u, "cached_pdev_vtable8_mode_filler", 0 },
    { 0x01A5425Cu, "cached_pdev_vtable9_mode_count",  0 },
    { 0x01A540D4u, "cached_pdev_vtable10_palette",    0 },
    /* DrvEnablePDEV's two main helpers — likely site of any MMIO reads. */
    { 0x01A54054u, "ddi_drvenablepdev_helper1",       0 },
    { 0x01A616A0u, "ddi_drvenablepdev_helper2",       0 },
    /* Probe the EngCreatePalette return capture at 0x01A54244 (sw v0, 0(t6)
     * where t6 = &hpalDefault). v0 at this PC is the palette handle
     * returned by gwes's EngCreatePalette (FUN_0407BDCC). If 0,
     * palette creation failed. */
    { 0x01A54244u, "vtable10_store_palette_handle",    0 },
    { 0x01A54248u, "vtable10_after_store",              0 },
    /* FUN_04049C00 return points. Successful return via `return 1`
     * happens somewhere inside the deep nested success branch;
     * failure path converges on LAB_0404A1E8 which sets
     * *param_1 = 0 then returns 0. Probe at the END of the function
     * (just before the final return) to see what value we return.
     * We don't yet know the exact PCs; probe the region liberally. */
    { 0x0004a1e8u, "fun49c00_LAB_cleanup",              0 },
    { 0x00049ffcu, "fun49c00_near_return",              0 },
    { 0x00049ff0u, "fun49c00_maybe_success_return",     0 },
    { 0x0004a1ccu, "fun49c00_return1_last_path",        0 },
    { 0x0004a200u, "fun49c00_cleanup_body",              0 },
    { 0x0004a210u, "fun49c00_cleanup_body2",             0 },
    { 0x0004a220u, "fun49c00_cleanup_body3",             0 },
    { 0x0004a230u, "fun49c00_cleanup_body4",             0 },
    /* Code between slot-2 dispatch (pc=0x49F64) and possible return;
     * find which post-slot-2 operation fails. */
    { 0x00049f78u, "fun49c00_after_slot2",               0 },
    { 0x00049fa0u, "fun49c00_after_slot2_2",             0 },
    { 0x0004a000u, "fun49c00_post_slot2_zone",           0 },
    { 0x0004a040u, "fun49c00_post_slot2_zone2",          0 },
    { 0x0004a080u, "fun49c00_post_slot2_zone3",          0 },
    { 0x0004a0c0u, "fun49c00_post_slot2_zone4",          0 },
    { 0x0004a100u, "fun49c00_post_slot2_zone5",          0 },
    { 0x0004a140u, "fun49c00_post_slot2_zone6",          0 },
    { 0x0004a180u, "fun49c00_post_slot2_zone7",          0 },
    { 0x0004a1a0u, "fun49c00_post_slot2_zone8",          0 },
    /* Dense 4-byte-stride probe from 0x4A140 (last observed hit) to
     * 0x4A1E8 (cleanup path) to find exact exit point. */
    { 0x0004a144u, "fun49c00_4a144",                     0 },
    { 0x0004a148u, "fun49c00_4a148",                     0 },
    { 0x0004a14cu, "fun49c00_4a14c",                     0 },
    { 0x0004a150u, "fun49c00_4a150",                     0 },
    { 0x0004a154u, "fun49c00_4a154",                     0 },
    { 0x0004a158u, "fun49c00_4a158",                     0 },
    { 0x0004a15cu, "fun49c00_4a15c",                     0 },
    { 0x0004a160u, "fun49c00_4a160",                     0 },
    { 0x0004a164u, "fun49c00_4a164",                     0 },
    { 0x0004a168u, "fun49c00_4a168",                     0 },
    { 0x0004a16cu, "fun49c00_4a16c",                     0 },
    { 0x0004a170u, "fun49c00_4a170",                     0 },
    { 0x0004a174u, "fun49c00_4a174",                     0 },
    { 0x0004a178u, "fun49c00_4a178",                     0 },
    { 0x0004a17cu, "fun49c00_4a17c",                     0 },
    { 0x0004a184u, "fun49c00_4a184",                     0 },
    { 0x0004a188u, "fun49c00_4a188",                     0 },
    { 0x0004a18cu, "fun49c00_4a18c",                     0 },
    { 0x0004a190u, "fun49c00_4a190",                     0 },
    { 0x0004a194u, "fun49c00_4a194",                     0 },
    { 0x0004a198u, "fun49c00_4a198",                     0 },
    { 0x0004a19cu, "fun49c00_4a19c",                     0 },
    { 0x0004a1a4u, "fun49c00_4a1a4",                     0 },
    { 0x0004a1a8u, "fun49c00_4a1a8",                     0 },
    { 0x0004a1acu, "fun49c00_4a1ac",                     0 },
    { 0x0004a1b0u, "fun49c00_4a1b0",                     0 },
    { 0x0004a1b4u, "fun49c00_4a1b4",                     0 },
    { 0x0004a1b8u, "fun49c00_4a1b8",                     0 },
    { 0x0004a1bcu, "fun49c00_4a1bc",                     0 },
    { 0x0004a1c0u, "fun49c00_4a1c0",                     0 },
    { 0x0004a1c4u, "fun49c00_4a1c4",                     0 },
    { 0x0004a1c8u, "fun49c00_4a1c8",                     0 },
    { 0x0004a1d0u, "fun49c00_4a1d0",                     0 },
    { 0x0004a1d4u, "fun49c00_4a1d4",                     0 },
    { 0x0004a1d8u, "fun49c00_4a1d8",                     0 },
    { 0x0004a1dcu, "fun49c00_4a1dc",                     0 },
    { 0x0004a1e0u, "fun49c00_4a1e0",                     0 },
    { 0x0004a1e4u, "fun49c00_4a1e4",                     0 },
    /* Probes between slot-0 return (0x49E30) and slot-2 call (0x49F68)
     * to find where second FUN_04049C00 invocation diverges. */
    { 0x00049e30u, "fun49c00_after_slot0",               0 },
    { 0x00049e44u, "fun49c00_after_slot0_branch",        0 },
    { 0x00049e48u, "fun49c00_cs_leave",                  0 },
    { 0x00049e50u, "fun49c00_after_cs_leave",            0 },
    { 0x00049e58u, "fun49c00_pdev_nonzero_check",        0 },
    { 0x00049e6cu, "fun49c00_null_branch_1",             0 },
    { 0x00049e74u, "fun49c00_null_branch_2",             0 },
    { 0x00049e94u, "fun49c00_null_b_cleanup_1",          0 },
    { 0x00049eb0u, "fun49c00_null_b_cleanup_2",          0 },
    { 0x00049eb8u, "fun49c00_gate_call",                 0 },
    { 0x00049ec0u, "fun49c00_gate_compare_5",            0 },
    { 0x00049ec4u, "fun49c00_gate_beq",                  0 },
    { 0x00049eccu, "fun49c00_gate_fail_branch",         0 },
    { 0x00049edcu, "fun49c00_gate_fail_b_cleanup",       0 },
    { 0x00049ee4u, "fun49c00_gate_success_branch",       0 },
    { 0x00049ef4u, "fun49c00_in_success_body",           0 },
    { 0x00049f14u, "fun49c00_jal_4a320",                 0 },
    { 0x00049f1cu, "fun49c00_after_4a320",               0 },
    { 0x00049f30u, "fun49c00_b69c8_only_block",          0 },
    { 0x00049f5cu, "fun49c00_before_slot2_prep",         0 },
    { 0x00049f68u, "fun49c00_slot2_jalr",                0 },
    { 0x00049f70u, "fun49c00_after_slot2_jalr",          0 },
    { 0x00049f74u, "fun49c00_slot2_retval_check",        0 },

    /* NK LoadLibrary return-path. loadlib_parse_filename's ra=0x80090A78
     * is the instruction after the call that parsed the filename. Watch
     * this PC AND watch 0x80090A78+N to see what the caller does with
     * the returned module handle. If v0=0 at this probe, LoadLibrary
     * reported failure. */
    { 0x80090A78u, "nk_loadlib_return_path",       0 },

    /*
     * Pass 28 (2026-04-20): Boot.exe (module 0x3B) stall triage.
     * Pass 27 confirmed Boot.exe spawns but never calls
     * SignalStarted(0x3B). These watches hit every direct child-call
     * the "fresh-boot-normal" path of Boot_WinMain (0x00012BD4) makes,
     * in source order. The last watch that fires with a HIGHER hit
     * count than the next one in the chain is the stall site.
     *
     * All VAs are in Boot.exe's slot-0 address space (0x00010000 base).
     * Other EXEs also live at 0x00011000+ but their code differs;
     * occasional noise hits from them are tolerable -- we care about
     * the LAST hit before launcher_blocking_wait_call gets stuck on
     * a2=0x3B.
     */
    { 0x00012BD4u, "bootexe_winmain_entry",           0 },
    { 0x00012F18u, "bootexe_safemode_check_entry",    0 },
    { 0x000124B0u, "bootexe_syncversionfiles_entry",  0 },
    { 0x0001209Cu, "bootexe_step_209c_entry",         0 },
    { 0x00011E18u, "bootexe_alarmdb_init_entry",      0 },
    { 0x000119A0u, "bootexe_step_119a0_entry",        0 },
    { 0x00011B30u, "bootexe_step_11b30_entry",        0 },
    { 0x00012F9Cu, "bootexe_step_12f9c_entry",        0 },
    { 0x00012D20u, "bootexe_write_initmarker_entry",  0 },
    /* Inside FUN_00012D20: after CreateFile returns, after CloseHandle
     * returns. If initmarker_entry fires but createfile_returned does
     * not, the stall is in CreateFile. If createfile_returned fires but
     * function_return doesn't, the stall is in CloseHandle (unlikely).
     */
    { 0x00012D5Cu, "bootexe_initmarker_createfile_returned", 0 },
    { 0x00012D70u, "bootexe_initmarker_function_return",     0 },
    /* Thunk entry at 0x1310C is the 6-arg call immediately following
     * FUN_00012D20 in WinMain's normal-fresh-boot path. The thunk reads
     * IAT slot 0x14084 and jumps into coredll. If this watch fires but
     * thunk_1310c_return does not, the coredll target hangs. */
    { 0x0001310Cu, "bootexe_thunk_1310c_entry",       0 },
    { 0x00012CC0u, "bootexe_thunk_1310c_return",      0 },
    { 0x00012FF0u, "bootexe_step_12ff0_entry",        0 },
    { 0x00012D18u, "bootexe_winmain_return",          0 },
};

/*
 * Pass 9 loader watches. These instrument the LoadLibrary / module-load
 * call chain so we can identify the driver image whose XXX_Init is
 * running on T5 (handoff §5.1). Hit limit is intentionally higher than
 * the generic g_exec_watches cap of 8 because device.exe loads many
 * drivers in succession; we need to see the LAST entry before stall.
 *
 * Reading a0 as UTF-16: FUN_80091c90 takes (LPCWSTR filename, ...)
 * per the LoadLibrary signature, so a0 should point to the image name.
 *
 * TODO: ActivateDeviceEx address not yet known — Ghidra has it as an
 * unnamed FUN_; locate via filesys.exe import / device.exe registry
 * dispatch and add the address here.
 */
static exec_watch_t g_loader_watches[] = {
    { 0x8008FD3Cu, "module_load_core", 0 },
    { 0x80091C90u, "loadlib_parse_filename", 0 },
    { 0x80093040u, "loadlib_acquire_loadercs", 0 },
};

/*
 * Pass 10 splash-render and reset-vector watches. Bumped hit cap (64)
 * so we capture the full early-boot OAL display sequence + any ROM
 * re-entry. All entries here use the same g_exec_watches mechanism
 * but with the alternate cap defined below in be300_probe_note_exec.
 *
 * Splash callers from Ghidra get_xrefs_to(0x80078e10):
 *   - 0x800A60A0 in FUN_800A6090 (single call)
 *   - 0x800776C0 / 0x800777C0 in FUN_8007767C (two calls)
 *   - 0x80077978 in FUN_8007796C
 *
 * The dispatcher itself at 0x80078E10 is logged so we see a0 (10=clear,
 * 0=splash buffer, 6=progress buffer) per CLAUDE.md.
 *
 * The ROM reset vector watch at 0xBFC00000 fires once on cold boot;
 * if it ever fires a SECOND time during a single emulator run, that
 * proves a CPU reset (HW or HALTimer/RSTSW/SOFTRST-triggered) occurred
 * — confirming the user's HW-reset hypothesis. The battery-check
 * function at 0x9FC003EC (PMUINTREG.BATTINH probe per Pass 11/12
 * Ghidra rename rom_battery_check_tests_PMUINTREG_BATTINH; NOT the
 * cold/warm gate — that lives at 0xBFC0042C) is logged for context.
 */
static exec_watch_t g_reset_watches[] = {
    { 0xBFC00000u, "rom_reset_vector", 0 },
    { 0x9FC003ECu, "rom_battery_check", 0 },
    { 0x80078E10u, "oal_display_dispatcher", 0 },
    { 0x800A60A0u, "splash_caller_a060a0", 0 },
    { 0x800776C0u, "splash_caller_776c0", 0 },
    { 0x800777C0u, "splash_caller_777c0", 0 },
    { 0x80077978u, "splash_caller_77978", 0 },
};

static mem_watch_t g_mem_watches[] = {
    /* Pass 29 (2026-04-20): capture the WinCE loader's write that binds
     * Boot.exe's IAT slot at VA 0x00014084 to a real coredll function
     * address. The thunk at Boot.exe VA 0x0001310C reads this slot and
     * jumps to the loaded value; that call is the Pass 28 stall. Read
     * is enabled so we also see every Boot.exe re-read of the slot in
     * case the loader binds lazily. */
    { 0x00014080u, 0x00014090u, "bootexe_iat_1310c_slot", true, true, 0, 0 },

    /* Pass 30 (2026-04-20): the NK dispatcher nk_SystemOpCode_dispatcher_0x1010xxx
     * at VA 0x800A84A8 handles Boot.exe's stalling call with cmd 0x0101003c.
     * Its wait loop reads _DAT_a0002644 (PA 0x2644 = kseg1 VA 0xA0002644)
     * inside a sleep(500)-per-iteration spin. Check whether this kernel tick
     * field advances (dwCurMSec-like) or stays static. Also watch the
     * kseg0 VA alias 0x80002644 for the same physical address. */
    { 0x00002644u, 0x00002648u, "kdata_tick_pa_2644",  true, true, 0, 0 },
    { 0x80002644u, 0x80002648u, "kdata_tick_kseg0",    true, true, 0, 0 },
    { 0xA0002644u, 0xA0002648u, "kdata_tick_kseg1",    true, true, 0, 0 },
    /* Wide watch for the KData page so we catch any writes (kseg0/1/2)
     * that update the tick-counter cluster around 0x2644. */
    { 0x00002640u, 0x00002660u, "kdata_page_pa",       false, true, 0, 0 },
    { 0x80002640u, 0x80002660u, "kdata_page_kseg0",    false, true, 0, 0 },
    { 0xA0002640u, 0xA0002660u, "kdata_page_kseg1",    false, true, 0, 0 },
    { 0xFFFFCE40u, 0xFFFFCE60u, "kdata_page_kseg2",    false, true, 0, 0 },

    { 0x01FE6544u, 0x01FE6548u, "coredll_oal_slot_6544", true, true, 0, 0 },
    { 0x01FE6548u, 0x01FE654Cu, "coredll_oal_slot_6548", true, true, 0, 0 },
    { 0x01FE654Cu, 0x01FE6550u, "coredll_oal_slot_654c", true, true, 0, 0 },
    { 0x01FE6550u, 0x01FE6554u, "coredll_oal_slot_6550", true, true, 0, 0 },
    { 0x01FE6554u, 0x01FE6558u, "coredll_oal_slot_6554", true, true, 0, 0 },
    { 0x8066006Cu, 0x80660088u, "kernel_funcptr_table", false, true, 0, 0 },
    { 0x80662AE0u, 0x80662AE4u, "boot_callback_gate", true, true, 0, 0 },
    { 0x806694E0u, 0x806694E4u, "boot_ready_flag", false, true, 0, 0 },
    { 0x806697E8u, 0x806697ECu, "boot_post_pulse_event_handle", true, true, 0, 0 },
    { 0x806698CCu, 0x806698D0u, "boot_wait_event_handle", true, true, 0, 0 },
    { 0x806695A0u, 0x806695B0u, "cs_memmgr_obj", false, true, 0, 0 },
    { 0x806697A0u, 0x806697B0u, "cs_alloc_obj", false, true, 0, 0 },
    { 0x8066AF00u, 0x8066AF04u, "launcher_event_handle", true, true, 0, 0 },
    { 0x80FFFF2Cu, 0x80FFFF30u, "coredll_desc_plus84", true, true, 0, 0 },

    /*
     * Pass 10 PMU reset-register watches (VR4131 UM §12.2). All are
     * 16-bit registers (we cover 4 bytes for half-word access). Reads
     * AND writes are logged because:
     *   - Reads of PMUINTREG (0xC0) reveal NK checking the reset-cause
     *     status bits (RTCRST, RSTSW, TIMOUTRST, BATTINH, etc.) early
     *     in cold boot, confirming NK expects to be re-entered after
     *     a reset.
     *   - Writes to PMUCNTREG (0xC2) reveal HALTIMERRST petting (bit 2
     *     write 1 = arm/restart watchdog within 4s window).
     *   - Writes to PMUCNT2REG (0xC6) reveal SOFTRST (bit 4 write 1 =
     *     trigger reset identical to RSTSW press) — the smoking gun
     *     for an NK-issued software reset.
     *
     * Both kseg1 (0xBF...) and kseg0 (0x8F...) aliases are watched
     * since NK might use either depending on cache requirements.
     */
    { 0xAF0000C0u, 0xAF0000C2u, "pmu_intreg_kseg1", true, true, 0, 0 },
    { 0xAF0000C2u, 0xAF0000C4u, "pmu_cntreg_kseg1", true, true, 0, 0 },
    { 0xAF0000C4u, 0xAF0000C6u, "pmu_int2reg_kseg1", true, true, 0, 0 },
    { 0xAF0000C6u, 0xAF0000C8u, "pmu_cnt2reg_kseg1", true, true, 0, 0 },
    { 0x8F0000C0u, 0x8F0000C2u, "pmu_intreg_kseg0", true, true, 0, 0 },
    { 0x8F0000C2u, 0x8F0000C4u, "pmu_cntreg_kseg0", true, true, 0, 0 },
    { 0x8F0000C4u, 0x8F0000C6u, "pmu_int2reg_kseg0", true, true, 0, 0 },
    { 0x8F0000C6u, 0x8F0000C8u, "pmu_cnt2reg_kseg0", true, true, 0, 0 },

    /*
     * BCU bus-watchdog (UM §15.2.1 TIMOUTCNTREG, §15.2.2 TIMOUTCOUNTREG).
     * Different timer than the HALTimer; this one detects bus access
     * timeouts (target doesn't ack within configured window).
     */
    { 0xAF001000u, 0xAF001008u, "bcu_timeout_regs_kseg1", true, true, 0, 0 },
    { 0x8F001000u, 0x8F001008u, "bcu_timeout_regs_kseg0", true, true, 0, 0 },

    /*
     * Pass 17 (2026-04-19): BE-300 board-ID register (PA 0x0A00A0C0,
     * hw_survey_v8.txt confirms 0x00007100 on real hw). Read by
     * pcmcia.dll's FUNC_C at UM 0x0198a9c8: if (read(0xAA00A0C0) & 0xFFF0)
     * != 0x7100, return NULL → phase1 bails, DllMain returns failure.
     * Our nand.c seeds strap_regs[0x60]=0x7100 but probe showed v0=0.
     * This watch exposes what value is actually returned to pcmcia and
     * from what PC the read happens.
     */
    { 0xAA00A0C0u, 0xAA00A0C4u, "board_id_kseg1", true, true, 0, 0 },
    { 0x8A00A0C0u, 0x8A00A0C4u, "board_id_kseg0", true, true, 0, 0 },

    /*
     * Pass 18 (2026-04-19): nanddisk.dll's CheckDMAEnd at UM 0x019A55B0
     * polls bit 0 of *(v1+0x10) where v1 = *(ctx+4). Init at UM 0x019A4200
     * maps 0xAA00A400 (size 196) as first MMIO region and stores it at
     * ctx->field_c. Watch the whole XFER range to find which register the
     * driver polls for DMA-done, and log who (if anyone) writes DMA-done.
     */
    { 0xAA00A400u, 0xAA00A4C4u, "nand_xfer_kseg1", true, true, 0, 0 },
    { 0x8A00A400u, 0x8A00A4C4u, "nand_xfer_kseg0", true, true, 0, 0 },

    /*
     * Pass 18: CheckDMAEnd at entry has a1=0x000D51C0 (observed).
     * CheckDMAEnd reads *(a1+4)=*(0x000D51C4) → v1=0x00061CC0 → polls
     * *(0x00061CD0) = PA 0x0A001CD0 (VRC4173 offset 0x1CD0). Real hw
     * reads 0 there; our emulator's latch is returning bit 0 set.
     * Watch both the user-VA poll target and kseg1 alias + the ctx.
     */
    { 0x000D51C0u, 0x000D51D0u, "nanddisk_ctx_head", true, true, 0, 0 },
    { 0x00061CC0u, 0x00061CE0u, "nd_dma_status_uva", true, true, 0, 0 },
    { 0xAA001CC0u, 0xAA001CE0u, "nd_dma_status_k1", true, true, 0, 0 },
    { 0x8A001CC0u, 0x8A001CE0u, "nd_dma_status_k0", true, true, 0, 0 },

    /*
     * Pass 32 addendum 6: DMA-engine hypothesis. The fb_topleft probe
     * only catches CPU stores (dyntrans hook is CPU-side). If DDI.DLL
     * uses memory-to-memory DMA to blit an off-screen back-buffer into
     * the framebuffer at PA 0x0A200000, CPU writes to 0xAA200000 would
     * be zero even though pixels flow. Watch the two DMA-engine ranges
     * available on BE-300:
     *   - VRC4173 DMA engine at PA 0x0A001CXX..0x0A001FXX (nanddisk uses
     *     0x1CD0 for status; control regs typically nearby)
     *   - VR4131 DMAC at PA 0x0F000200+ (per VR4131 UM §14)
     * Watch for writes that aren't attributable to nanddisk.
     */
    { 0xAA001C00u, 0xAA002000u, "vrc4173_dma_range_k1", false, true, 0, 0 },
    { 0xAF000200u, 0xAF000280u, "vr4131_dmac_range_k1", true,  true, 0, 0 },

    /*
     * Pass 32 addendum 6: watch a typical user-space back-buffer location.
     * If DDI.DLL's off-screen buffer is allocated from process heap, it
     * often lands at low VAs in the 0x00100000-0x00800000 range. Watch
     * a few candidate single bytes to see massive write rates — an
     * actively drawing framebuffer would generate millions of writes/sec
     * to SOME VA. Heavy-reading from a specific VA by the LCD scanout
     * (if it's CPU-driven) would also be visible.
     */
    { 0x00200000u, 0x00200004u, "maybe_backbuf_at_2mb",   false, true, 0, 0 },
    { 0x00400000u, 0x00400004u, "maybe_backbuf_at_4mb",   false, true, 0, 0 },
    { 0x00800000u, 0x00800004u, "maybe_backbuf_at_8mb",   false, true, 0, 0 },
    { 0x01000000u, 0x01000004u, "maybe_backbuf_at_16mb",  false, true, 0, 0 },

    /*
     * Pass 32 (2026-04-20): framebuffer write diagnostic.
     * PA 0xAA200000 is the BE-300 framebuffer per Linux4be hardware.txt:193.
     * OAL's splash_caller_a060a0 draws "Starting.bmp" early. After gwes
     * reaches its message loop (Pass 32 addendum 2 confirms), the user-
     * mode display driver is expected to take over and redraw.
     *
     * Watch a 16-byte window at the framebuffer's top-left pixel region.
     * If writes hit this range AFTER the Pass 31 warm reset, the display
     * handover IS happening (and the stall-visible-at-Starting.bmp is
     * merely an SDL screenshot-timing artefact). If writes never hit
     * post-reset, the user-mode display driver has not taken over and
     * the stall is in display-driver loading or display-handover code.
     *
     * Log reads too: display driver may probe the current contents.
     */
    { 0xAA200000u, 0xAA200010u, "fb_topleft_kseg1", true, true, 0, 0 },
    { 0x8A200000u, 0x8A200010u, "fb_topleft_kseg0", true, true, 0, 0 },

    /*
     * Pass 32 addendum (2026-04-22): test the user's hypothesis that
     * user-mode GDI is writing to a framebuffer region OTHER than PA
     * 0x0A200000 (and the emulator's SDL only sees 0x0A200000).
     *
     * Watch writes in a wide band around the official framebuffer +
     * typical backing-store ranges. Exclude the first 16 bytes
     * (already covered by fb_topleft_kseg1) and watch up through the
     * end of a 320x240x16bpp surface (0x25800 bytes = ~150 KB).
     *
     * If this watch accumulates zero writes post-warm-reset but the
     * guest was supposed to be drawing, the writes must be going
     * somewhere else entirely — to a secondary PA range or through
     * an LCD-controller base register our emulator doesn't track.
     */
    { 0xAA200010u, 0xAA226000u, "fb_body_kseg1_writes",   false, true, 0, 0 },
    { 0x8A200010u, 0x8A226000u, "fb_body_kseg0_writes",   false, true, 0, 0 },
    { 0x0A200010u, 0x0A226000u, "fb_body_pa_writes",      false, true, 0, 0 },

    /*
     * Secondary framebuffer candidates — plausible alternate PAs the
     * WinCE display stack might use for a back buffer or after a
     * user-mode LCD base-register rewrite:
     *   - 0x0A280000..0x0A2A6000: just past primary surface
     *   - 0x0A000000..0x0A026000: low-end of VRC4173 range
     *   - 0x00100000..0x00200000: somewhere in the user-mode mapped
     *     shared buffer region (gwes often keeps DIBs there)
     */
    { 0xAA280000u, 0xAA2A6000u, "fb_alt_2a_kseg1",        false, true, 0, 0 },
    { 0xAA400000u, 0xAA426000u, "fb_alt_4_kseg1",         false, true, 0, 0 },
    { 0x00140000u, 0x00170000u, "gdi_surface_0x140000",   false, true, 0, 0 },
    { 0x001E0000u, 0x00206000u, "ddi_mapped_user_va",     true,  true, 0, 0 },

    /*
     * Pass 32 addendum 2 (2026-04-22): cached_pdev[0x6C] at UVA
     * 0x0011047C holds the VirtualCopy-mapped FB user VA (0x001E0000
     * per prior session). ANY read means the primary surface's VA
     * is being looked up — probably by ddi.dll's blit-to-primary
     * path. Watch reads AND writes — writes would identify the
     * setter (only expected during DrvEnablePDEV); reads identify
     * the blit caller.
     */
    { 0x0011047Cu, 0x00110480u, "cached_pdev_6c_primary_va", true, true, 0, 0 },

    /*
     * Pass 21 Objective 2: nanddisk blank-block BAT pointer at
     * UM 0x019AB34C. `lw t8, -19636(at)` at 0x019A3D08 loads from
     * this slot; if the pointer is still zero, the subsequent
     * `sh t0, 0(t9)` faults (Exception 003 at 0x019A3D14). Watch
     * reads + writes to see if the slot is ever initialised.
     */
    { 0x019AB34Cu, 0x019AB350u, "nanddisk_blank_bat_ptr", true, true, 0, 0 },

    /*
     * Pass 32 addendum 7 follow-up (Stage 1b): the DDI function table
     * written by ddi_DrvEnableDriver_impl lives at DAT_01A637F8..01A63828
     * per addendum 7 line 21. Log every write so we can recover the
     * iFunc PCs at runtime (iFunc 0 = DrvEnablePDEV, 1 = DrvCompletePDEV,
     * 3 = DrvEnableSurface). Writes are what we care about; reads will
     * flood the log once blits start.
     */
    { 0x01A637F8u, 0x01A63828u, "ddi_funcptr_table",     false, true, 0, 0 },
    /* Slot 13+ of the GDI Eng* callback table — vtable[10] reads
     * *(0x01A63828) = EngCreatePalette. Watch slot 13 AND a few past. */
    { 0x01A63828u, 0x01A63850u, "ddi_funcptr_table_slot13plus", true, true, 0, 0 },

    /*
     * Pass 32 addendum 7 follow-up (Stage 1b continued): ppdded struct
     * passed by gwes to DrvEnableDriver (a2=0x000B6A00 per addendum 7).
     * Size is 0x6C per a1. Watch all reads AND writes so we capture:
     *   - DDI's writes of iDriverVersion, c, pdrvfn (reveals drvfn array VA)
     *   - GDI's reads after DrvEnableDriver returns (shows iFunc dispatch order)
     */
    { 0x000B6A00u, 0x000B6A6Cu, "gwes_ppdded_struct",   true, true, 0, 0 },

    /*
     * Pass 32 §12: gwes import stub `func_0x000b1814` reads IAT entry
     * at 0x000B32BC and jumps. This function returns 0 in our emulator
     * causing the gwes init chain to early-return. Watch both reads
     * (to see the resolved coredll PC during runtime) and writes
     * (to see the loader populating the slot).
     */
    { 0x000B32BCu, 0x000B32C0u, "gwes_iat_b1814_imp",   true, true, 0, 0 },

    /*
     * Pass 32 §13 (2026-04-22): VR4131 RTC MMIO range (PA 0x0F000100..
     * 0x0F00013F) via kseg1 0xAF0001xx. ETIMEL/M/H + ECMPL/M/H + RTCL1/2
     * + TCLK + RTCINT. Watch every access so we can see how the kernel
     * reads the RTC and what time WinCE computes from ETIME=0 on cold
     * boot. The point is to determine whether our "ETIME starts at 0"
     * matches the real-HW post-battery-pull signal WinCE expects.
     */
    { 0xAF000100u, 0xAF000140u, "vr4131_rtc_range_k1",  true, true, 0, 0 },
};


static bool be300_probe_cpu_is_ours(const struct cpu *cpu)
{
    if (!cpu || !cpu->machine)
        return false;

    return cpu->machine == g_probe.machine &&
        cpu->machine->machine_type == MACHINE_HPCMIPS &&
        cpu->machine->machine_subtype == MACHINE_HPCMIPS_CASIO_BE300;
}

static bool be300_probe_machine_matches(const struct cpu *cpu)
{
    if (!g_probe.enabled)
        return false;
    return be300_probe_cpu_is_ours(cpu);
}

static void be300_probe_reset_counters(void)
{
    size_t i;

    for (i = 0; i < sizeof(g_exec_watches) / sizeof(g_exec_watches[0]); i++)
        g_exec_watches[i].hits = 0;

    for (i = 0; i < sizeof(g_loader_watches) / sizeof(g_loader_watches[0]); i++)
        g_loader_watches[i].hits = 0;

    for (i = 0; i < sizeof(g_reset_watches) / sizeof(g_reset_watches[0]); i++)
        g_reset_watches[i].hits = 0;

    for (i = 0; i < sizeof(g_mem_watches) / sizeof(g_mem_watches[0]); i++) {
        g_mem_watches[i].read_hits = 0;
        g_mem_watches[i].write_hits = 0;
    }

    g_probe.launcher_state_dumps = 0;
    g_probe.launcher_notify_dumps = 0;
    g_probe.loader_logs = 0;
    g_probe.module_walk_done = 0;
    g_probe.t5_dump_done = 0;
    g_probe.gdi_dumped = 0;
}

static bool be300_probe_read_guest(struct cpu *cpu, uint64_t addr,
    unsigned char *buf, size_t len)
{
    if (!be300_probe_machine_matches(cpu) || !buf || len == 0)
        return false;

    memset(buf, 0, len);
    return cpu->memory_rw(cpu, cpu->mem, be300_probe_norm_addr(addr), buf, len,
        MEM_READ, CACHE_NONE | NO_EXCEPTIONS) != 0;
}

/*
 * Diagnostic-only: render a guest memory region as a 240x320 16-bpp RGB565
 * bitmap and save it to the current directory as a .bmp. Used to visually
 * inspect what WinCE's user-mode GDI has drawn into candidate surfaces (the
 * GDI surface heap at 0x00140000, the VirtualCopy-mapped primary surface at
 * 0x001E0000, etc.) since the default SDL display only shows PA 0xAA200000.
 *
 * The read goes through cpu->memory_rw which uses the current process's ASID
 * for user VAs. Caller must ensure this fires when the expected process's
 * VA space is active (e.g., inside a probe hit on a ddi.dll PC means gwes
 * is current). For kseg1 PAs no ASID constraint applies.
 *
 * Reads in 4KB chunks; failed chunks stay zero so partial reads still
 * produce a viewable BMP rather than all-or-nothing.
 */
#define BE300_GDI_DUMP_W 240
#define BE300_GDI_DUMP_H 320
/*
 * True row stride in bytes. The hardware framebuffer at PA 0xAA200000
 * uses 512 bytes per row (240*2 = 480 visible + 32 padding) per
 * docs/hardware/hardware.txt:15. User-mode mappings of the primary
 * surface inherit the same stride. GDI off-screen surfaces (e.g. at
 * VA 0x140000) usually pack tight at 480 bytes per row. Reading with
 * the wrong stride produces a diagonal-shear artifact because each row
 * starts 32 bytes earlier or later than where its pixels live.
 */
#define BE300_FB_ROW_STRIDE     512
#define BE300_TIGHT_ROW_STRIDE  (BE300_GDI_DUMP_W * 2)

static void be300_probe_dump_region_as_bmp(struct cpu *cpu, uint64_t vaddr,
    const char *region_label, int row_stride)
{
    unsigned char *buf;
    size_t off;
    char fname[128];
    time_t t;
    struct tm *tm;
    SDL_Surface *surf;

    if (!be300_probe_machine_matches(cpu) || !region_label)
        return;

    if (row_stride <= 0 || row_stride < BE300_GDI_DUMP_W * 2)
        row_stride = BE300_GDI_DUMP_W * 2;

    size_t dump_bytes = (size_t)row_stride * (size_t)BE300_GDI_DUMP_H;

    buf = (unsigned char *)calloc(1, dump_bytes);
    if (!buf) {
        fprintf(stderr,
            "[BE300_GDI_DUMP] region=%s vaddr=0x%08" PRIx64 " alloc_failed=1\n",
            region_label, vaddr);
        return;
    }

    /* Read in 4KB chunks; if a chunk fails, leave zeros so the BMP still
     * renders the parts that did read. Counts successful chunks for the log. */
    size_t good = 0, total = 0;
    for (off = 0; off < dump_bytes; off += 4096) {
        size_t n = dump_bytes - off;
        if (n > 4096) n = 4096;
        total++;
        if (cpu->memory_rw(cpu, cpu->mem,
                be300_probe_norm_addr(vaddr + off), buf + off, n,
                MEM_READ, CACHE_NONE | NO_EXCEPTIONS) != 0) {
            good++;
        }
    }

    t = time(NULL);
    tm = localtime(&t);
    /* Serial number prevents filename collisions within the same second. */
    static uint32_t s_serial = 0;
    uint32_t serial = ++s_serial;
    snprintf(fname, sizeof(fname),
        "gdi_dump_%04d%02d%02d_%02d%02d%02d_%03u_%s.bmp",
        tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
        tm->tm_hour, tm->tm_min, tm->tm_sec, serial, region_label);

    surf = SDL_CreateRGBSurfaceWithFormatFrom(buf,
        BE300_GDI_DUMP_W, BE300_GDI_DUMP_H, 16,
        row_stride, SDL_PIXELFORMAT_RGB565);
    if (!surf) {
        fprintf(stderr,
            "[BE300_GDI_DUMP] region=%s vaddr=0x%08" PRIx64
            " sdl_surface_failed=%s\n",
            region_label, vaddr, SDL_GetError());
        free(buf);
        return;
    }

    if (SDL_SaveBMP(surf, fname) == 0) {
        fprintf(stderr,
            "[BE300_GDI_DUMP] saved=%s region=%s vaddr=0x%08" PRIx64
            " chunks_ok=%zu/%zu\n",
            fname, region_label, vaddr, good, total);
    } else {
        fprintf(stderr,
            "[BE300_GDI_DUMP] region=%s vaddr=0x%08" PRIx64
            " savebmp_failed=%s\n",
            region_label, vaddr, SDL_GetError());
    }

    SDL_FreeSurface(surf);
    free(buf);
}

static void be300_probe_dump_all_gdi_regions(struct cpu *cpu, const char *why)
{
    if (!be300_probe_machine_matches(cpu))
        return;
    if (g_probe.gdi_dumped)
        return;
    g_probe.gdi_dumped = 1;

    fprintf(stderr, "[BE300_GDI_DUMP] trigger=%s\n", why ? why : "?");
    /*
     * Only dump regions that represent a coherent 240x320 framebuffer:
     *   primary_uva + primary_kseg1 map to/are the HW framebuffer at
     *   PA 0xAA200000 with 512-byte row stride (hardware.txt:15).
     *
     * Deliberately NOT dumped: VA 0x00140000 (the GDI surface heap)
     * is a heap of many small surfaces at varying offsets/strides, not
     * a single 240x320 bitmap; rendering it as one produces garbage
     * that misleads investigation. Its write-count is still tracked via
     * the gdi_surface_0x140000 memory watchpoint — use that for signal.
     * Individual surfaces are dumped by be300_probe_dump_surfobj() when
     * a ddi.dll blit fires, which has correct per-surface dimensions
     * and stride from each SURFOBJ.
     */
    be300_probe_dump_region_as_bmp(cpu, 0x001E0000u, "primary_uva",
        BE300_FB_ROW_STRIDE);
    be300_probe_dump_region_as_bmp(cpu, 0xAA200000u, "primary_kseg1",
        BE300_FB_ROW_STRIDE);
}

/*
 * At an active ddi.dll blit-dispatcher call, the source SURFOBJ is in a0 and
 * the dest SURFOBJ is in a1. A WinCE-3.0 SURFOBJ is laid out roughly:
 *   +0x00 dhsurf    +0x04 hsurf     +0x08 dhpdev    +0x0C hdev
 *   +0x10 sizlBitmap.cx  +0x14 sizlBitmap.cy
 *   +0x18 cjBits
 *   +0x1C pvBits     +0x20 pvScan0   +0x24 lDelta
 *   +0x28 iUniq      +0x2C iBitmapFormat
 *   +0x30 iType      +0x32 fjBitmap
 * Dump a0/a1's struct bytes and, if pvBits + dimensions look reasonable,
 * save the bitmap content as a BMP. This is the clean answer to "what is
 * user-mode actually drawing" because it follows the driver's own size
 * metadata.
 */
static void be300_probe_dump_surfobj(struct cpu *cpu, uint32_t surfobj_va,
    const char *tag)
{
    unsigned char hdr[0x40];
    uint32_t pv_bits, cx, cy;
    int32_t l_delta;
    uint32_t bm_format;

    if (!be300_probe_machine_matches(cpu) || surfobj_va == 0)
        return;
    if (!be300_probe_read_guest(cpu, surfobj_va, hdr, sizeof(hdr))) {
        fprintf(stderr,
            "[BE300_GDI_SURFOBJ] tag=%s va=0x%08x read_failed=1\n",
            tag, surfobj_va);
        return;
    }

    cx        = (uint32_t)(hdr[0x10] | (hdr[0x11] << 8) |
                           (hdr[0x12] << 16) | (hdr[0x13] << 24));
    cy        = (uint32_t)(hdr[0x14] | (hdr[0x15] << 8) |
                           (hdr[0x16] << 16) | (hdr[0x17] << 24));
    pv_bits   = (uint32_t)(hdr[0x1C] | (hdr[0x1D] << 8) |
                           (hdr[0x1E] << 16) | (hdr[0x1F] << 24));
    l_delta   = (int32_t)(hdr[0x24] | (hdr[0x25] << 8) |
                          (hdr[0x26] << 16) | (hdr[0x27] << 24));
    bm_format = (uint32_t)(hdr[0x2C] | (hdr[0x2D] << 8) |
                           (hdr[0x2E] << 16) | (hdr[0x2F] << 24));

    fprintf(stderr,
        "[BE300_GDI_SURFOBJ] tag=%s va=0x%08x cx=%u cy=%u "
        "pvBits=0x%08x lDelta=%d iBitmapFormat=%u\n",
        tag, surfobj_va, cx, cy, pv_bits, l_delta, bm_format);

    if (cx == 0 || cy == 0 || cx > 2048 || cy > 2048 || pv_bits == 0)
        return;

    /*
     * WinCE SURFOBJ.iBitmapFormat → bpp + SDL pixel format. Previous
     * dump code hardcoded RGB565 and cx*2 stride, producing diagonal
     * shear on any surface that wasn't exactly tight 16-bpp (most
     * blits are 1-bpp masks or 8-bpp palette, and the 16-bpp ones
     * can be bottom-up with negative lDelta). See WinCE 3.0 DDI docs
     * "SURFOBJ" and "Bitmap Formats".
     */
    int bpp;
    uint32_t sdl_fmt;
    switch (bm_format) {
        case 1: bpp = 1;  sdl_fmt = SDL_PIXELFORMAT_INDEX1MSB;  break;
        case 2: bpp = 4;  sdl_fmt = SDL_PIXELFORMAT_INDEX4MSB;  break;
        case 3: bpp = 8;  sdl_fmt = SDL_PIXELFORMAT_INDEX8;     break;
        case 4: bpp = 16; sdl_fmt = SDL_PIXELFORMAT_RGB565;     break;
        case 5: bpp = 24; sdl_fmt = SDL_PIXELFORMAT_BGR24;      break;
        case 6: bpp = 32; sdl_fmt = SDL_PIXELFORMAT_ARGB8888;   break;
        default:
            fprintf(stderr,
                "[BE300_GDI_SURFOBJ] tag=%s unknown_iBitmapFormat=%u\n",
                tag, bm_format);
            return;
    }

    /*
     * True row stride is |lDelta|. Negative lDelta means bottom-up
     * (scan 0 = bottommost displayed row; pvBits points at scan 0).
     * Copy into a flat top-first buffer so SDL can render with a
     * positive stride regardless of orientation.
     */
    int abs_stride = l_delta < 0 ? -l_delta : l_delta;
    if (abs_stride == 0) {
        /* Fallback: tight row of cx pixels. */
        abs_stride = ((int)cx * bpp + 7) / 8;
    }
    if (abs_stride <= 0 || abs_stride > 65536) {
        fprintf(stderr,
            "[BE300_GDI_SURFOBJ] tag=%s implausible_stride=%d\n",
            tag, abs_stride);
        return;
    }

    size_t pv_bytes = (size_t)cy * (size_t)abs_stride;
    unsigned char *buf = (unsigned char *)calloc(1, pv_bytes);
    if (!buf)
        return;

    /*
     * For negative lDelta, visual-top row is the LAST scan in memory,
     * so we read from pv_bits + (cy-1)*lDelta (= pv_bits - (cy-1)*|lDelta|)
     * linearly forward. For positive lDelta we read from pv_bits forward.
     * Either way, buf[] ends up holding visual-top-row first.
     */
    uint32_t read_base = (l_delta < 0)
        ? (pv_bits - (uint32_t)((cy - 1) * abs_stride))
        : pv_bits;

    size_t good = 0, total = 0;
    for (size_t off = 0; off < pv_bytes; off += 4096) {
        size_t n = pv_bytes - off;
        if (n > 4096) n = 4096;
        total++;
        if (cpu->memory_rw(cpu, cpu->mem,
                be300_probe_norm_addr(read_base + (uint32_t)off),
                buf + off, n,
                MEM_READ, CACHE_NONE | NO_EXCEPTIONS) != 0) {
            good++;
        }
    }

    /*
     * NOTE: no row flip needed. For BOTH top-down (lDelta > 0) and
     * bottom-up (lDelta < 0), the visual-top row lives at the LOWEST
     * memory address in the surface's bitmap buffer, because:
     *   top-down: scan 0 = visual top at pvBits (low mem), scan N above it
     *   bottom-up: scan 0 = visual BOTTOM at pvBits (high mem), scan N
     *              at pvBits + N*lDelta = pvBits - N*|lDelta| (lower mem),
     *              so scan cy-1 (visual TOP) is at pvBits - (cy-1)*|lDelta|
     * read_base (above) targets that lowest-memory row for either sign,
     * and a linear read to higher memory yields visual-top first — exactly
     * the order SDL expects from a positive-pitch surface.
     */

    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    static uint32_t s_surf_serial = 0;
    uint32_t serial = ++s_surf_serial;
    char fname[128];
    snprintf(fname, sizeof(fname),
        "gdi_surfobj_%04d%02d%02d_%02d%02d%02d_%03u_%s_%ux%u_bpp%d.bmp",
        tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
        tm->tm_hour, tm->tm_min, tm->tm_sec, serial, tag, cx, cy, bpp);

    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(buf,
        (int)cx, (int)cy, bpp, abs_stride, sdl_fmt);
    if (surf) {
        /* Indexed formats need a palette. Install a sane default
         * (grayscale ramp or 0/1 B&W) so SDL_SaveBMP doesn't error
         * out and the saved BMP is human-legible. */
        if (bpp == 1 && surf->format->palette) {
            SDL_Color pal[2] = {
                {0x00, 0x00, 0x00, 0xff},
                {0xff, 0xff, 0xff, 0xff},
            };
            SDL_SetPaletteColors(surf->format->palette, pal, 0, 2);
        } else if ((bpp == 4 || bpp == 8) && surf->format->palette) {
            int n = (bpp == 4) ? 16 : 256;
            SDL_Color pal[256];
            for (int i = 0; i < n; i++) {
                int v = (bpp == 4) ? (i * 255 / 15) : i;
                pal[i].r = pal[i].g = pal[i].b = (Uint8)v;
                pal[i].a = 0xff;
            }
            SDL_SetPaletteColors(surf->format->palette, pal, 0, n);
        }
        /*
         * SDL_SaveBMP only supports 8/24/32 bpp outputs. Convert any
         * indexed surface at <8 bpp into 24 bpp for saving so
         * 1-bpp masks and 4-bpp bitmaps are viewable.
         */
        SDL_Surface *out = surf;
        SDL_Surface *converted = NULL;
        if (bpp < 8) {
            converted = SDL_ConvertSurfaceFormat(surf,
                SDL_PIXELFORMAT_BGR24, 0);
            if (converted)
                out = converted;
        }
        if (SDL_SaveBMP(out, fname) == 0) {
            fprintf(stderr,
                "[BE300_GDI_SURFOBJ] saved=%s tag=%s pvBits=0x%08x "
                "cx=%u cy=%u bpp=%d stride=%d chunks_ok=%zu/%zu\n",
                fname, tag, pv_bits, cx, cy, bpp, abs_stride, good, total);
        } else {
            fprintf(stderr,
                "[BE300_GDI_SURFOBJ] tag=%s savebmp_failed=%s\n",
                tag, SDL_GetError());
        }
        if (converted)
            SDL_FreeSurface(converted);
        SDL_FreeSurface(surf);
    }
    free(buf);
}

static bool be300_probe_read_u16(struct cpu *cpu, uint64_t addr, uint16_t *out)
{
    unsigned char buf[2];

    if (!out || !be300_probe_read_guest(cpu, addr, buf, sizeof(buf)))
        return false;

    *out = (uint16_t)(buf[0] | (buf[1] << 8));
    return true;
}

static bool be300_probe_read_u32(struct cpu *cpu, uint64_t addr, uint32_t *out)
{
    unsigned char buf[4];

    if (!out || !be300_probe_read_guest(cpu, addr, buf, sizeof(buf)))
        return false;

    *out = (uint32_t)buf[0]
        | ((uint32_t)buf[1] << 8)
        | ((uint32_t)buf[2] << 16)
        | ((uint32_t)buf[3] << 24);
    return true;
}

static void be300_probe_read_utf16le_ascii(struct cpu *cpu, uint64_t addr,
    char *dst, size_t dst_len)
{
    size_t used = 0;

    if (!dst || dst_len == 0)
        return;

    dst[0] = '\0';
    while (used + 1 < dst_len) {
        uint16_t wc = 0;
        char ch;

        if (!be300_probe_read_u16(cpu, addr + used * 2, &wc) || wc == 0)
            break;

        ch = (wc >= 0x20 && wc < 0x7f) ? (char)wc : '?';
        dst[used++] = ch;
    }

    dst[used] = '\0';
}

static void be300_probe_dump_launcher_state(struct cpu *cpu, const char *reason)
{
    uint32_t count = 0;
    uint32_t base = 0;
    uint32_t *dump_counter;

    if (!be300_probe_machine_matches(cpu))
        return;

    dump_counter = strcmp(reason, "notify") == 0
        ? &g_probe.launcher_notify_dumps
        : &g_probe.launcher_state_dumps;

    (*dump_counter)++;
    if (*dump_counter > 4)
        return;

    fprintf(stderr,
        "[BE300_LIFECYCLE_LAUNCHER] reason=%s dump=%u entering=1\n",
        reason, *dump_counter);

    if (!be300_probe_read_u32(cpu, 0x8066AEE8u, &count) ||
        !be300_probe_read_u32(cpu, 0x8066AEECu, &base)) {
        fprintf(stderr,
            "[BE300_LIFECYCLE_LAUNCHER] reason=%s count=? base=? read_failed=1\n",
            reason);
        return;
    }

    fprintf(stderr,
        "[BE300_LIFECYCLE_LAUNCHER] reason=%s dump=%u count=%u base=0x%08x "
        "s2=0x%08" PRIx64 " s5=0x%08" PRIx64 "\n",
        reason, *dump_counter, count, base,
        (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S2],
        (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S5]);

    if (base == 0 || count == 0 || count > 32)
        return;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t entry = (uint64_t)base + i * 0x250u;
        uint32_t launch_id = 0;
        uint32_t ready = 0;
        char deps[96];
        char image[96];
        size_t used = 0;

        deps[0] = '\0';
        image[0] = '\0';
        if (!be300_probe_read_u32(cpu, entry + 0x00, &launch_id) ||
            !be300_probe_read_u32(cpu, entry + 0x04, &ready))
            continue;

        for (uint64_t dep_off = entry + 0x08; used + 8 < sizeof(deps); dep_off += 2) {
            uint16_t dep = 0;
            int written;

            if (!be300_probe_read_u16(cpu, dep_off, &dep) || dep == 0)
                break;

            written = snprintf(deps + used, sizeof(deps) - used,
                used == 0 ? "0x%04x" : ",0x%04x", dep);
            if (written < 0 || (size_t)written >= sizeof(deps) - used)
                break;
            used += (size_t)written;
        }

        be300_probe_read_utf16le_ascii(cpu, entry + 0x48, image, sizeof(image));
        fprintf(stderr,
            "[BE300_LIFECYCLE_LAUNCHER_ENTRY] idx=%u id=0x%08x ready=%u "
            "deps=%s image=\"%s\"\n",
            i, launch_id, ready, deps[0] ? deps : "-", image);
    }
}

static void be300_probe_dump_launcher_current_entry(struct cpu *cpu,
    const char *reason)
{
    uint32_t base = 0;
    uint32_t idx = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S2];
    uint64_t entry;
    uint32_t launch_id = 0;
    uint32_t ready = 0;
    char deps[96];
    char image[96];
    size_t used = 0;

    if (!be300_probe_machine_matches(cpu) ||
        !be300_probe_read_u32(cpu, 0x8066AEECu, &base) ||
        base == 0) {
        return;
    }

    entry = (uint64_t)base + idx * 0x250u;
    deps[0] = '\0';
    image[0] = '\0';

    if (!be300_probe_read_u32(cpu, entry + 0x00, &launch_id) ||
        !be300_probe_read_u32(cpu, entry + 0x04, &ready))
        return;

    for (uint64_t dep_off = entry + 0x08; used + 8 < sizeof(deps); dep_off += 2) {
        uint16_t dep = 0;
        int written;

        if (!be300_probe_read_u16(cpu, dep_off, &dep) || dep == 0)
            break;

        written = snprintf(deps + used, sizeof(deps) - used,
            used == 0 ? "0x%04x" : ",0x%04x", dep);
        if (written < 0 || (size_t)written >= sizeof(deps) - used)
            break;
        used += (size_t)written;
    }

    be300_probe_read_utf16le_ascii(cpu, entry + 0x48, image, sizeof(image));
    fprintf(stderr,
        "[BE300_LIFECYCLE_LAUNCHER_BLOCK] reason=%s idx=%u id=0x%08x ready=%u "
        "deps=%s image=\"%s\"\n",
        reason, idx, launch_id, ready, deps[0] ? deps : "-", image);
}

static const uint32_t g_cs_objects[] = {
    0x80669600u, 0x80669800u, 0x806695C0u, 0x806697C0u, 0x806697A0u,
    0x80669760u, 0x80669720u, 0x80669680u, 0x806696A0u, 0x80669820u,
    0x80669660u, 0x806696C0u, 0x806695E0u, 0x806695A0u, 0x80669740u,
    0x80669640u,
};

static void be300_probe_dump_candidate_thread_struct(struct cpu *cpu,
    uint32_t base)
{
    size_t row;
    unsigned char buf[256];

    if (!be300_probe_machine_matches(cpu))
        return;

    if (!be300_probe_read_guest(cpu, (uint64_t)base, buf, sizeof(buf))) {
        fprintf(stderr,
            "[BE300_LIFECYCLE_THREAD_CANDIDATE] base=0x%08x read_failed=1\n",
            base);
        return;
    }

    fprintf(stderr,
        "[BE300_LIFECYCLE_THREAD_CANDIDATE] base=0x%08x dump_bytes=%zu\n",
        base, sizeof(buf));

    for (row = 0; row < sizeof(buf); row += 16) {
        fprintf(stderr,
            "[BE300_LIFECYCLE_THREAD_CANDIDATE] off=0x%02zx "
            "%02x%02x%02x%02x %02x%02x%02x%02x "
            "%02x%02x%02x%02x %02x%02x%02x%02x\n",
            row,
            buf[row+0], buf[row+1], buf[row+2], buf[row+3],
            buf[row+4], buf[row+5], buf[row+6], buf[row+7],
            buf[row+8], buf[row+9], buf[row+10], buf[row+11],
            buf[row+12], buf[row+13], buf[row+14], buf[row+15]);
    }
}

static void be300_probe_dump_all_cs_state(struct cpu *cpu, const char *reason)
{
    size_t i;
    /* Track held-CS owners for post-loop handle-table dumps. */
    uint32_t held_owners[16];
    size_t held_count = 0;

    if (!be300_probe_machine_matches(cpu))
        return;

    fprintf(stderr,
        "[BE300_LIFECYCLE_CS_SNAPSHOT] reason=%s begin count=%zu\n",
        reason, sizeof(g_cs_objects) / sizeof(g_cs_objects[0]));

    for (i = 0; i < sizeof(g_cs_objects) / sizeof(g_cs_objects[0]); i++) {
        uint32_t base = g_cs_objects[i];
        uint32_t count = 0;
        uint32_t owner = 0;
        uint32_t sync = 0;
        uint32_t field_c = 0;

        be300_probe_read_u32(cpu, (uint64_t)base + 0x00, &count);
        be300_probe_read_u32(cpu, (uint64_t)base + 0x04, &owner);
        be300_probe_read_u32(cpu, (uint64_t)base + 0x08, &sync);
        be300_probe_read_u32(cpu, (uint64_t)base + 0x0C, &field_c);

        fprintf(stderr,
            "[BE300_LIFECYCLE_CS] addr=0x%08x count=0x%08x owner=0x%08x "
            "sync=0x%08x field_c=0x%08x\n",
            base, count, owner, sync, field_c);

        /*
         * Pass 23: track held CSes (count > 0 AND owner != 0). field_c
         * is not required — a CS can be held with field_c=0 if the
         * re-entry counter hasn't been incremented beyond the first
         * acquire (the Pass 21 observation at hit=59 for 0x806695A0).
         */
        if (count != 0 && owner != 0 &&
            held_count < sizeof(held_owners) / sizeof(held_owners[0])) {
            held_owners[held_count++] = owner;
        }
    }

    fprintf(stderr, "[BE300_LIFECYCLE_CS_SNAPSHOT] reason=%s end\n", reason);

    /*
     * Pass 23: resolve the handle for each held CS's owner. The owner
     * field is a raw handle (low 2 bits = type tag — see
     * HANDOFF_POST_PASS23 §9.3 for waiter/handle layout). Strip
     * those and apply the kseg0 alias to get the handle-table entry
     * VA (entry +0x14 = classdesc, +0x18 = object VA).
     *
     * For THRD-class owners (classdesc 0x80074C38), also
     * dump the thread struct itself — saved `s0` (primary waiter) and
     * saved EPC live there, and distinguish:
     *   EPC=0x800819A4 → parked in WFM
     *   EPC=0x80088230 → parked in EnterCS_wait
     *   EPC elsewhere → actively running, CS hold is mid-computation
     */
    for (i = 0; i < held_count; i++) {
        uint32_t handle = held_owners[i];
        uint32_t handle_va = (handle & ~0x3u) | 0x80000000u;
        uint32_t classdesc = 0;
        uint32_t object_va = 0;

        fprintf(stderr,
            "[BE300_LIFECYCLE_CS_OWNER] handle=0x%08x handle_va=0x%08x\n",
            handle, handle_va);
        be300_probe_dump_candidate_thread_struct(cpu, handle_va);

        be300_probe_read_u32(cpu, (uint64_t)handle_va + 0x14, &classdesc);
        be300_probe_read_u32(cpu, (uint64_t)handle_va + 0x18, &object_va);

        fprintf(stderr,
            "[BE300_LIFECYCLE_CS_OWNER_RESOLVED] handle=0x%08x "
            "classdesc=0x%08x object_va=0x%08x%s\n",
            handle, classdesc, object_va,
            classdesc == 0x80074C38u ? " (THRD)" : "");

        if (object_va != 0) {
            be300_probe_dump_candidate_thread_struct(cpu, object_va);
        }
    }

    /* Retained historical bases (from the 2026-04-18 investigation)
     * for cross-reference. */
    be300_probe_dump_candidate_thread_struct(cpu, 0x80FFC9C8u);
    be300_probe_dump_candidate_thread_struct(cpu, 0x80FFC9E8u);
}

/*
 * Pass 9 §5.1 — dump T5 (device.exe driver-init thread) state.
 *
 * T5 is parked in EnterCS_wait waiting for memmgr CS that T3 holds. Its
 * saved user-mode EPC is 0x080AF000 in slot-3 ASID per the prior
 * handoff. Goal: recover the user-mode saved RA so we can map it to a
 * driver image. Strategy:
 *
 *   1) Dump T5's KTHREAD head 256 bytes — exposes saved registers.
 *   2) For each plausible kseg0/kseg1 pointer in the head (high bit
 *      set, looks like RAM in 0x80000000-0x82000000), dump 256 bytes
 *      from there. One of those pointers is the saved kernel SP;
 *      walking that stack reveals the call chain.
 *
 * One-shot: only fires once per emulator run.
 */
static void be300_probe_dump_t5_state(struct cpu *cpu)
{
    static const uint32_t T5_BASE = 0x80FD592Cu;
    unsigned char head[256];
    size_t i;

    if (!be300_probe_machine_matches(cpu) || g_probe.t5_dump_done)
        return;
    g_probe.t5_dump_done = 1;

    fprintf(stderr, "[BE300_LIFECYCLE_T5_STACK] tag=t5_thread_struct\n");
    be300_probe_dump_candidate_thread_struct(cpu, T5_BASE);

    if (!be300_probe_read_guest(cpu, (uint64_t)T5_BASE, head, sizeof(head)))
        return;

    /*
     * Scan for plausible kseg0/kseg1 pointers in T5's head. Aligned 4
     * bytes only; high bit set; not the thread struct itself. Each
     * candidate gets a 256-byte dump from its target so post-hoc
     * analysis can identify which one is the kernel SP and walk it.
     */
    for (i = 0; i + 4 <= sizeof(head); i += 4) {
        uint32_t v = (uint32_t)head[i]
            | ((uint32_t)head[i+1] << 8)
            | ((uint32_t)head[i+2] << 16)
            | ((uint32_t)head[i+3] << 24);
        if ((v & 0x80000000u) == 0)
            continue;
        /* Restrict to plausible RAM range and 4-byte alignment. */
        if (v < 0x80000000u || v >= 0x82000000u)
            continue;
        if ((v & 3u) != 0)
            continue;
        if ((v & ~0xffu) == (T5_BASE & ~0xffu))
            continue;  /* points back into T5 head */
        fprintf(stderr,
            "[BE300_LIFECYCLE_T5_STACK] candidate_ptr off=0x%02zx target=0x%08x\n",
            i, v);
        be300_probe_dump_candidate_thread_struct(cpu, v);
    }
}

/*
 * Pass 9 §5.1 / B.4 — walk NK's kernel thread-object head at
 * _DAT_806697DC (per memory/project_post_ppsh_stall.md "Next:" line).
 * This reaches the head pointer, then dumps 256 bytes from the
 * dereferenced target so the linked-list structure can be decoded
 * post-hoc. ALSO dumps a region around 0x806697xx where the
 * module/handle-table heads are likely co-located, so post-hoc analysis
 * can identify the module list head separately.
 *
 * One-shot.
 */
static void be300_probe_dump_loaded_modules(struct cpu *cpu)
{
    uint32_t head_ptr = 0;

    if (!be300_probe_machine_matches(cpu) || g_probe.module_walk_done)
        return;
    g_probe.module_walk_done = 1;

    fprintf(stderr, "[BE300_LIFECYCLE_MODULE] tag=kernel_data_dump\n");
    be300_probe_dump_candidate_thread_struct(cpu, 0x80669700u);
    be300_probe_dump_candidate_thread_struct(cpu, 0x80669800u);

    if (be300_probe_read_u32(cpu, 0x806697DCu, &head_ptr) && head_ptr != 0) {
        fprintf(stderr,
            "[BE300_LIFECYCLE_MODULE] tag=thread_list_head ptr=0x%08x\n",
            head_ptr);
        if ((head_ptr & 0x80000000u) != 0)
            be300_probe_dump_candidate_thread_struct(cpu, head_ptr);
    }
}

/*
 * Pass 9 §5.1 / B.2 — log loader-call entries with UTF-16 a0 decoded.
 *
 * a0 is treated as LPCWSTR for FUN_80091c90 (LoadLibrary parse-
 * filename). For the other two entries (FUN_8008fd3c, FUN_80093040)
 * a0 may be a different argument type; we still attempt UTF-16 decode
 * and emit the raw value either way. Hit cap is per-watch (256) since
 * device.exe issues many loads in succession during boot.
 */
static void be300_probe_log_loader_call(struct cpu *cpu, uint64_t pc,
    exec_watch_t *watch)
{
    uint32_t a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
    uint32_t a1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1];
    uint32_t ra = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA];
    char arg[96];

    arg[0] = '\0';
    if ((a0 & 1u) == 0 && a0 != 0)
        be300_probe_read_utf16le_ascii(cpu, (uint64_t)a0, arg, sizeof(arg));

    fprintf(stderr,
        "[BE300_LIFECYCLE_LOAD] func=%s hit=%u pc=0x%08" PRIx64
        " a0=0x%08x a1=0x%08x ra=0x%08x arg=\"%s\"\n",
        watch->label, watch->hits, pc, a0, a1, ra, arg);
}

static size_t be300_probe_copy_bytes(char *dst, size_t dst_len,
    const unsigned char *data, size_t len)
{
    size_t used = 0;
    size_t n = len > 16 ? 16 : len;

    if (!dst_len)
        return 0;

    dst[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        int written = snprintf(dst + used, dst_len - used, "%02x", data[i]);
        if (written < 0 || (size_t)written >= dst_len - used)
            break;
        used += (size_t)written;
    }

    if (len > n && used + 3 < dst_len) {
        memcpy(dst + used, "...", 4);
        used += 3;
    }

    return used;
}

static void be300_probe_log_mem(struct cpu *cpu, uint64_t pc, uint64_t vaddr,
    const unsigned char *data, size_t len, bool is_write)
{
    size_t i;
    char hexbuf[64];

    if (!be300_probe_machine_matches(cpu) || !data || len == 0)
        return;

    pc = be300_probe_norm_addr(pc);
    vaddr = be300_probe_norm_addr(vaddr);

    for (i = 0; i < sizeof(g_mem_watches) / sizeof(g_mem_watches[0]); i++) {
        mem_watch_t *watch = &g_mem_watches[i];
        uint64_t end = vaddr + len;
        uint32_t *hits;

        if (end <= watch->start || vaddr >= watch->end)
            continue;

        if (is_write && !watch->log_writes)
            continue;
        if (!is_write && !watch->log_reads)
            continue;

        hits = is_write ? &watch->write_hits : &watch->read_hits;
        (*hits)++;
        if (*hits > 4096)
            continue;

        be300_probe_copy_bytes(hexbuf, sizeof(hexbuf), data, len);
        fprintf(stderr,
            "[BE300_LIFECYCLE_%c] label=%s hit=%u pc=0x%08" PRIx64
            " vaddr=0x%08" PRIx64 " len=%zu data=%s sp=0x%08" PRIx64
            " ra=0x%08" PRIx64 "\n",
            is_write ? 'W' : 'R', watch->label, *hits, pc, vaddr, len, hexbuf,
            (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
    }
}

void be300_probe_set_options(bool mmio_coverage,
                             bool detect_stall,
                             uint32_t stall_window,
                             uint32_t stall_unique_threshold,
                             uint32_t stall_wall_secs)
{
    g_probe.mmio_coverage          = mmio_coverage;
    g_probe.detect_stall           = detect_stall;
    g_probe.stall_window           = stall_window  ? stall_window : 200000u;
    g_probe.stall_unique_threshold = stall_unique_threshold ? stall_unique_threshold : 32u;
    g_probe.stall_wall_secs        = stall_wall_secs ? stall_wall_secs : 15u;

    if (mmio_coverage)
        fprintf(stderr, "[BE300_MMIO_COVERAGE] enabled cap=%u\n",
            BE300_MMIO_COV_CAP);
    if (detect_stall)
        fprintf(stderr,
            "[BE300_STALL] sampler enabled window=%u threshold=%u wall_secs=%u\n",
            g_probe.stall_window, g_probe.stall_unique_threshold,
            g_probe.stall_wall_secs);
}

void be300_probe_note_mmio(const char *dev, uint32_t off, char op,
    uint32_t len, uint64_t pc, int mclass)
{
    uint32_t slot;
    uint32_t probes;

    if (!g_probe.mmio_coverage || !dev)
        return;

    slot = be300_mmio_hash(dev, off, op);
    for (probes = 0; probes < BE300_MMIO_COV_CAP; probes++) {
        mmio_cov_entry_t *e = &g_mmio_cov[slot];
        if (e->dev == NULL) {
            if (g_mmio_cov_used + 1u >= BE300_MMIO_COV_CAP) {
                g_mmio_cov_overflow++;
                return;
            }
            e->dev       = dev;
            e->off       = off;
            e->first_pc  = (uint32_t)pc;
            e->hits      = 1;
            e->len       = (uint16_t)len;
            e->op        = (uint8_t)op;
            e->mclass    = (uint8_t)mclass;
            g_mmio_cov_used++;
            fprintf(stderr,
                "[BE300_MMIO_FIRST] dev=%s off=0x%05x op=%c len=%u class=%s pc=0x%08x\n",
                dev, off, op, len, be300_mmio_class_name(mclass), (uint32_t)pc);
            return;
        }
        if (e->op == (uint8_t)op && e->off == off &&
            (e->dev == dev || strcmp(e->dev, dev) == 0)) {
            e->hits++;
            return;
        }
        slot = (slot + 1u) & (BE300_MMIO_COV_CAP - 1u);
    }
    g_mmio_cov_overflow++;
}

static void be300_probe_stall_sample(uint32_t pc)
{
    uint32_t slot;
    uint32_t probes;
    uint64_t now_ns;

    if (!g_probe.detect_stall)
        return;

    /* Insert PC into bucket hash-set (uint32 PCs, 0 = empty). */
    if (pc == 0u) pc = 1u;  /* fold PC=0 so empty sentinel works */
    slot = (pc * 2654435761u) & (BE300_STALL_PC_CAP - 1u);
    for (probes = 0; probes < BE300_STALL_PC_CAP; probes++) {
        if (g_stall.pcs[slot] == 0u) {
            g_stall.pcs[slot] = pc;
            g_stall.inserted++;
            break;
        }
        if (g_stall.pcs[slot] == pc)
            break;
        slot = (slot + 1u) & (BE300_STALL_PC_CAP - 1u);
    }

    g_stall.insn_in_bucket++;
    if (g_stall.insn_in_bucket < g_probe.stall_window)
        return;

    now_ns = be300_probe_wall_ns();
    if (g_stall.inserted < g_probe.stall_unique_threshold) {
        if (g_stall.low_run_start_ns == 0)
            g_stall.low_run_start_ns = now_ns;
        g_stall.consecutive_high_buckets = 0;
        if (now_ns - g_stall.low_run_start_ns >=
                (uint64_t)g_probe.stall_wall_secs * 1000000000ull) {
            if (now_ns - g_stall.last_fire_ns >= 30ull * 1000000000ull) {
                fprintf(stderr,
                    "[BE300_STALL] fire=%u unique=%u window=%u pc=0x%08x "
                    "low_run_secs=%llu\n",
                    g_stall.fire_count + 1u,
                    g_stall.inserted, g_probe.stall_window, pc,
                    (unsigned long long)((now_ns - g_stall.low_run_start_ns) / 1000000000ull));
                g_stall.fire_count++;
                g_stall.last_fire_ns = now_ns;
                g_stall.last_fire_pc = pc;
            }
        }
    } else {
        g_stall.consecutive_high_buckets++;
        if (g_stall.consecutive_high_buckets >= BE300_STALL_HIGH_HYSTERESIS)
            g_stall.low_run_start_ns = 0;
    }

    /* Reset bucket. */
    memset(g_stall.pcs, 0, sizeof(g_stall.pcs));
    g_stall.inserted = 0;
    g_stall.insn_in_bucket = 0;
}

void be300_probe_attach(struct machine *machine)
{
    const char *env = getenv("BE300_LIFECYCLE_PROBE");

    g_probe.enabled = env != NULL && env[0] != '\0' && strcmp(env, "0") != 0;
    g_probe.machine = machine;
    be300_probe_reset_counters();

    /* W1 / W4 state — cleared unconditionally; set_options turns features on. */
    memset(g_mmio_cov, 0, sizeof(g_mmio_cov));
    g_mmio_cov_used = 0;
    g_mmio_cov_overflow = 0;
    memset(&g_stall, 0, sizeof(g_stall));
    g_probe.mmio_coverage = false;
    g_probe.detect_stall  = false;

    if (!g_probe.enabled || !machine)
        return;

    fprintf(stderr,
        "[BE300_LIFECYCLE] enabled=1 machine_type=%d machine_subtype=%d\n",
        machine->machine_type, machine->machine_subtype);
}

static int mmio_cov_cmp_desc(const void *a, const void *b)
{
    const mmio_cov_entry_t *ea = a, *eb = b;
    if (ea->hits == 0 && eb->hits == 0) return 0;
    if (ea->hits == 0) return 1;
    if (eb->hits == 0) return -1;
    if (eb->hits > ea->hits) return 1;
    if (eb->hits < ea->hits) return -1;
    return 0;
}

static void be300_probe_dump_mmio_coverage(void)
{
    mmio_cov_entry_t *sorted;
    uint32_t i;

    if (!g_probe.mmio_coverage || g_mmio_cov_used == 0) {
        if (g_probe.mmio_coverage)
            fprintf(stderr, "[BE300_MMIO_COVERAGE] no hits recorded\n");
        return;
    }

    sorted = malloc(sizeof(mmio_cov_entry_t) * BE300_MMIO_COV_CAP);
    if (!sorted) {
        fprintf(stderr, "[BE300_MMIO_COVERAGE] malloc failed, skipping dump\n");
        return;
    }
    memcpy(sorted, g_mmio_cov, sizeof(mmio_cov_entry_t) * BE300_MMIO_COV_CAP);
    qsort(sorted, BE300_MMIO_COV_CAP, sizeof(mmio_cov_entry_t), mmio_cov_cmp_desc);

    fprintf(stderr,
        "[BE300_MMIO_COVERAGE] unique=%u overflow=%u cap=%u\n",
        g_mmio_cov_used, g_mmio_cov_overflow, BE300_MMIO_COV_CAP);
    for (i = 0; i < BE300_MMIO_COV_CAP; i++) {
        mmio_cov_entry_t *e = &sorted[i];
        if (e->hits == 0 || e->dev == NULL)
            break;
        fprintf(stderr,
            "[BE300_MMIO_COVERAGE] dev=%-20s off=0x%05x op=%c len=%u "
            "class=%-7s hits=%u first_pc=0x%08x\n",
            e->dev, e->off, e->op, e->len,
            be300_mmio_class_name(e->mclass), e->hits, e->first_pc);
    }
    free(sorted);
}

void be300_probe_detach(struct machine *machine)
{
    size_t i;

    /* W1 / W4 summaries run independently of the lifecycle probe gate. */
    if (g_probe.machine == machine) {
        be300_probe_dump_mmio_coverage();
        if (g_probe.detect_stall) {
            fprintf(stderr,
                "[BE300_STALL_SUMMARY] fired=%u last_pc=0x%08x\n",
                g_stall.fire_count, g_stall.last_fire_pc);
        }
    }

    if (!g_probe.enabled || g_probe.machine != machine)
        goto reset;

    for (i = 0; i < sizeof(g_exec_watches) / sizeof(g_exec_watches[0]); i++) {
        if (g_exec_watches[i].hits == 0)
            continue;
        fprintf(stderr,
            "[BE300_LIFECYCLE_SUMMARY] exec label=%s hits=%u pc=0x%08" PRIx64
            "\n",
            g_exec_watches[i].label, g_exec_watches[i].hits,
            g_exec_watches[i].pc);
    }

    for (i = 0; i < sizeof(g_loader_watches) / sizeof(g_loader_watches[0]); i++) {
        if (g_loader_watches[i].hits == 0)
            continue;
        fprintf(stderr,
            "[BE300_LIFECYCLE_SUMMARY] loader label=%s hits=%u pc=0x%08" PRIx64
            "\n",
            g_loader_watches[i].label, g_loader_watches[i].hits,
            g_loader_watches[i].pc);
    }

    for (i = 0; i < sizeof(g_reset_watches) / sizeof(g_reset_watches[0]); i++) {
        if (g_reset_watches[i].hits == 0)
            continue;
        fprintf(stderr,
            "[BE300_LIFECYCLE_SUMMARY] reset label=%s hits=%u pc=0x%08" PRIx64
            "\n",
            g_reset_watches[i].label, g_reset_watches[i].hits,
            g_reset_watches[i].pc);
    }

    for (i = 0; i < sizeof(g_mem_watches) / sizeof(g_mem_watches[0]); i++) {
        if (g_mem_watches[i].read_hits == 0 && g_mem_watches[i].write_hits == 0)
            continue;
        fprintf(stderr,
            "[BE300_LIFECYCLE_SUMMARY] mem label=%s reads=%u writes=%u "
            "range=0x%08" PRIx64 "..0x%08" PRIx64 "\n",
            g_mem_watches[i].label, g_mem_watches[i].read_hits,
            g_mem_watches[i].write_hits, g_mem_watches[i].start,
            g_mem_watches[i].end);
    }

reset:
    g_probe.enabled       = false;
    g_probe.machine       = NULL;
    g_probe.mmio_coverage = false;
    g_probe.detect_stall  = false;
    memset(g_mmio_cov, 0, sizeof(g_mmio_cov));
    g_mmio_cov_used = 0;
    g_mmio_cov_overflow = 0;
    memset(&g_stall, 0, sizeof(g_stall));
    be300_probe_reset_counters();
}

void be300_probe_note_exec(struct cpu *cpu, uint64_t pc)
{
    size_t i;

    /* W4 — stall sampler runs independently of the lifecycle probe; it only
     * requires the CPU to belong to our machine. */
    if (g_probe.detect_stall && be300_probe_cpu_is_ours(cpu))
        be300_probe_stall_sample((uint32_t)be300_probe_norm_addr(pc));

    if (!be300_probe_machine_matches(cpu))
        return;

    pc = be300_probe_norm_addr(pc);

    for (i = 0; i < sizeof(g_loader_watches) / sizeof(g_loader_watches[0]); i++) {
        exec_watch_t *watch = &g_loader_watches[i];
        if (watch->pc != pc)
            continue;
        watch->hits++;
        if (watch->hits > 256)
            return;
        be300_probe_log_loader_call(cpu, pc, watch);
        return;
    }

    for (i = 0; i < sizeof(g_reset_watches) / sizeof(g_reset_watches[0]); i++) {
        exec_watch_t *watch = &g_reset_watches[i];
        if (watch->pc != pc)
            continue;
        watch->hits++;
        if (watch->hits > 64)
            return;
        fprintf(stderr,
            "[BE300_LIFECYCLE_RESET] label=%s hit=%u pc=0x%08" PRIx64
            " sp=0x%08" PRIx64 " ra=0x%08" PRIx64
            " a0=0x%08" PRIx64 " a1=0x%08" PRIx64
            " v0=0x%08" PRIx64 " entryhi=0x%08" PRIx64 "\n",
            watch->label, watch->hits, pc,
            (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
            (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
            (uint64_t)(uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_ENTRYHI]);
        return;
    }

    for (i = 0; i < sizeof(g_exec_watches) / sizeof(g_exec_watches[0]); i++) {
        exec_watch_t *watch = &g_exec_watches[i];

        if (watch->pc != pc)
            continue;

        watch->hits++;

        /*
         * Pass 17: loader_post_dllmain (0x800929d0) fires ~59× in a 60s
         * cold boot post fix. We need a late trigger for CS snapshot, so
         * keep logging beyond 8 for this PC (capped at 256 for safety).
         */
        uint32_t cap = (pc == 0x800929d0u) ? 256u :
                       (pc == 0x019B17CCu) ? 64u :
                       (pc == 0x8008690Cu) ? 64u :
                       (pc == 0x80080CB4u) ? 32u :
                       (pc == 0x80080D38u) ? 32u :
                       (pc == 0x01A5BF00u) ? 32u :
                       (pc == 0x000B1054u) ? 256u :
                       8u;
        if (watch->hits > cap)
            return;

        fprintf(stderr,
            "[BE300_LIFECYCLE_PC] label=%s hit=%u pc=0x%08" PRIx64
            " sp=0x%08" PRIx64 " ra=0x%08" PRIx64
            " a0=0x%08" PRIx64 " a1=0x%08" PRIx64
            " a2=0x%08" PRIx64 " v0=0x%08" PRIx64
            " entryhi=0x%08" PRIx64 "\n",
            watch->label, watch->hits, pc,
            (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
            (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
            (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
            (uint64_t)(uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_ENTRYHI]);

        if (watch->pc == 0x01A5C6C8u && watch->hits == 1) {
            /* Dump the ddi.dll static mode data at 0x01A63088 (24 bytes). */
            unsigned char mode[24];
            if (cpu->memory_rw(cpu, cpu->mem, 0x01A63088u, mode, sizeof(mode),
                    MEM_READ, CACHE_DATA | NO_EXCEPTIONS) != MEMORY_ACCESS_FAILED) {
                size_t i;
                fprintf(stderr, "[BE300_LIFECYCLE_MODEDATA] 0x01A63088:");
                for (i = 0; i < sizeof(mode); i++)
                    fprintf(stderr, " %02x", mode[i]);
                fprintf(stderr, "\n");
            }
            /* Also dump the buffer at 0x001105A4 (a1 at vtable[8]) —
             * this is where vtable[8] wrote to. After return, read it
             * back to see what was actually deposited. */
            unsigned char filled[24];
            if (cpu->memory_rw(cpu, cpu->mem, 0x001105A4u, filled, sizeof(filled),
                    MEM_READ, CACHE_DATA | NO_EXCEPTIONS) != MEMORY_ACCESS_FAILED) {
                size_t i;
                fprintf(stderr, "[BE300_LIFECYCLE_MODEFILLED] 0x001105A4:");
                for (i = 0; i < sizeof(filled); i++)
                    fprintf(stderr, " %02x", filled[i]);
                fprintf(stderr, "\n");
            }
        }
        if (watch->pc == 0x01A5C6C8u && watch->hits == 1) {
            /* Just returned from vtable[8]. a0 at entry was cached_pdev;
             * now read cached_pdev[0] = vtable, then vtable[8] PC.
             * cached_pdev was 0x00110410 per prior run. Dump it + vtable. */
            unsigned char buf[4];
            uint64_t cached_pdev = 0x00110410u;
            uint32_t vtable_ptr = 0;
            if (cpu->memory_rw(cpu, cpu->mem, cached_pdev, buf, 4,
                    MEM_READ, CACHE_DATA | NO_EXCEPTIONS) != MEMORY_ACCESS_FAILED) {
                vtable_ptr = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8)
                           | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
                fprintf(stderr,
                    "[BE300_LIFECYCLE_VTABLE] cached_pdev=0x%08x vtable=0x%08x\n",
                    (uint32_t)cached_pdev, vtable_ptr);
                if (vtable_ptr != 0) {
                    unsigned char vt[0x80];
                    if (cpu->memory_rw(cpu, cpu->mem, vtable_ptr, vt, sizeof(vt),
                            MEM_READ, CACHE_DATA | NO_EXCEPTIONS) != MEMORY_ACCESS_FAILED) {
                        size_t off;
                        for (off = 0; off < sizeof(vt); off += 16) {
                            fprintf(stderr,
                                "[BE300_LIFECYCLE_VTABLE] +0x%02zx: "
                                "%02x%02x%02x%02x %02x%02x%02x%02x "
                                "%02x%02x%02x%02x %02x%02x%02x%02x\n",
                                off,
                                vt[off+0], vt[off+1], vt[off+2], vt[off+3],
                                vt[off+4], vt[off+5], vt[off+6], vt[off+7],
                                vt[off+8], vt[off+9], vt[off+10], vt[off+11],
                                vt[off+12], vt[off+13], vt[off+14], vt[off+15]);
                        }
                    }
                }
            }
        }
        if ((watch->pc == 0x01A5D254u || watch->pc == 0x01A5D228u ||
             watch->pc == 0x01A5C604u || watch->pc == 0x01A54264u ||
             watch->pc == 0x01A540D4u || watch->pc == 0x01A54054u ||
             watch->pc == 0x01A616A0u) && watch->hits == 1) {
            unsigned char buf[0x200];
            uint64_t addr = watch->pc;
            size_t off;
            if (cpu->memory_rw(cpu, cpu->mem, addr, buf, sizeof(buf),
                    MEM_READ, CACHE_INSTRUCTION | NO_EXCEPTIONS)
                != MEMORY_ACCESS_FAILED) {
                fprintf(stderr, "[BE300_LIFECYCLE_CODEDUMP] pc=0x%08x (%s)\n",
                    (uint32_t)addr, watch->label);
                for (off = 0; off < sizeof(buf); off += 16) {
                    fprintf(stderr,
                        "[BE300_LIFECYCLE_CODEDUMP] +0x%02zx: "
                        "%02x%02x%02x%02x %02x%02x%02x%02x "
                        "%02x%02x%02x%02x %02x%02x%02x%02x\n",
                        off,
                        buf[off+0], buf[off+1], buf[off+2], buf[off+3],
                        buf[off+4], buf[off+5], buf[off+6], buf[off+7],
                        buf[off+8], buf[off+9], buf[off+10], buf[off+11],
                        buf[off+12], buf[off+13], buf[off+14], buf[off+15]);
                }
            }
        }
        if (watch->pc == 0x01A5D2F0u && watch->hits == 1) {
            /* Pass 32 Stage 1 round 4: dump 0x100 bytes of DrvEnablePDEV
             * code on first hit so we can disassemble offline. */
            unsigned char buf[0x300];
            uint64_t addr = 0x01A5D2F0u;
            size_t off;
            if (cpu->memory_rw(cpu, cpu->mem, addr, buf, sizeof(buf),
                    MEM_READ, CACHE_INSTRUCTION | NO_EXCEPTIONS)
                != MEMORY_ACCESS_FAILED) {
                fprintf(stderr, "[BE300_LIFECYCLE_CODEDUMP] pc=0x%08x\n",
                    (uint32_t)addr);
                for (off = 0; off < sizeof(buf); off += 16) {
                    fprintf(stderr,
                        "[BE300_LIFECYCLE_CODEDUMP] +0x%02zx: "
                        "%02x%02x%02x%02x %02x%02x%02x%02x "
                        "%02x%02x%02x%02x %02x%02x%02x%02x\n",
                        off,
                        buf[off+0], buf[off+1], buf[off+2], buf[off+3],
                        buf[off+4], buf[off+5], buf[off+6], buf[off+7],
                        buf[off+8], buf[off+9], buf[off+10], buf[off+11],
                        buf[off+12], buf[off+13], buf[off+14], buf[off+15]);
                }
            } else {
                fprintf(stderr,
                    "[BE300_LIFECYCLE_CODEDUMP] read FAILED at 0x%08x\n",
                    (uint32_t)addr);
            }
        }

        if (watch->pc == 0x800A8788u) {
            uint32_t t5 = (uint32_t)cpu->cd.mips.gpr[13];
            uint32_t t6 = (uint32_t)cpu->cd.mips.gpr[14];
            uint32_t t7 = (uint32_t)cpu->cd.mips.gpr[15];
            uint32_t sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
            fprintf(stderr,
                "[BE300_LIFECYCLE_REBOOTWAIT] hit=%u pa_2644=0x%08x diff=0x%08x "
                "Wait=0x%08x (=%u ms)  sp+0x48=TimeOut_slot sp=0x%08x\n",
                watch->hits, t5, t6, t7, t7, sp);
        }
        if (watch->pc == 0x80080AA4u)
            be300_probe_dump_launcher_state(cpu, "wait");
        else if (watch->pc == 0x80080CB4u)
            be300_probe_dump_launcher_current_entry(cpu, "wait_call");
        else if (watch->pc == 0x80080D38u)
            be300_probe_dump_launcher_state(cpu, "notify");
        else if (watch->pc == 0x8008690Cu) {
            /* Diagnostic-only: decode CreateProcess a0 as UTF-16LE image name
             * so we can cross-reference the 7× hits against the launcher table
             * and identify which process each spawn corresponds to. */
            uint32_t a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
            char image[96];
            image[0] = '\0';
            if ((a0 & 1u) == 0 && a0 != 0)
                be300_probe_read_utf16le_ascii(cpu, (uint64_t)a0, image,
                    sizeof(image));
            fprintf(stderr,
                "[BE300_LIFECYCLE_CREATEPROCESS] hit=%u a0=0x%08x image=\"%s\"\n",
                watch->hits, a0, image);
        }
        else if (watch->pc == 0x01F8B4E4u) {
            /* Post-return from kernel callback. v0 holds return value. */
            fprintf(stderr,
                "[BE300_CDLL_B4D0_RET] hit=%u v0=0x%08" PRIx64
                " pc=0x%08" PRIx64 " ra=0x%08" PRIx64 "\n",
                watch->hits,
                (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0], pc,
                (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
        }
        else if (watch->pc == 0x000B1054u && watch->hits <= 256) {
            /* Pass 32 §11 — decode (handle, op) on every coredll
             * EventModify call, so we can see whether op=3 actually
             * fires for handle 0x6834. */
            uint32_t a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
            uint32_t a1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1];
            uint32_t ra = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA];
            const char *opname =
                (a1 == 1) ? "PULSE" :
                (a1 == 2) ? "RESET" :
                (a1 == 3) ? "SET"   : "?";
            /* Flag calls targeting the three gwes worker-synch handles
             * so they are easy to grep in a noisy log. */
            const char *hnote =
                (a0 == 0x000B6834u) ? " [GWES_6834]" :
                (a0 == 0x000B6824u) ? " [GWES_6824]" :
                (a0 == 0x000B6830u) ? " [GWES_6830]" : "";
            fprintf(stderr,
                "[BE300_EVENTMOD] hit=%u handle=0x%08x op=%u(%s) ra=0x%08x%s\n",
                watch->hits, a0, a1, opname, ra, hnote);
        }
        else if (watch->pc == 0x01A5BF00u && watch->hits <= 30) {
            /* Pass 32 §9 follow-up: at EVERY blit-dispatcher hit (up to 30),
             * dump the src/dst SURFOBJ bitmap content. This follows ddi.dll's
             * own size metadata so we see the exact bitmap gwes is drawing
             * into / out of, not a guessed region. */
            uint32_t a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
            uint32_t a1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1];
            char tag_src[32], tag_dst[32];
            snprintf(tag_src, sizeof(tag_src), "blit%u_src", watch->hits);
            snprintf(tag_dst, sizeof(tag_dst), "blit%u_dst", watch->hits);
            be300_probe_dump_surfobj(cpu, a0, tag_src);
            be300_probe_dump_surfobj(cpu, a1, tag_dst);
            /* Also dump the primary-FB regions at hit 25 only (once) for
             * comparison — same as the original fixed dump but with a
             * guaranteed late-stable timepoint. */
            if (watch->hits == 25) {
                g_probe.gdi_dumped = 0;
                be300_probe_dump_all_gdi_regions(cpu, "ddi_blit_hit25_late");
            }
        }
        else if (watch->pc == 0x80082618u && watch->hits == 1) {
            be300_probe_dump_all_cs_state(cpu, "boot_wfm_entry");
            be300_probe_dump_t5_state(cpu);
            be300_probe_dump_loaded_modules(cpu);
        }
        /*
         * Pass 17 Step C: snapshot CS + thread state at a LATE point
         * after PCMCIA fix unblocked the boot. Observed hit count is ~59
         * in a 60s run; dump at hit 59 (expected last) to catch state
         * at the true stall edge. Hit 50 showed LoaderCS held by the
         * loading thread — expected mid-load state, not the stall.
         */
        else if (watch->pc == 0x800929d0u && watch->hits == 59) {
            be300_probe_dump_all_cs_state(cpu, "post_dllmain_hit59");
            be300_probe_dump_t5_state(cpu);
            be300_probe_dump_loaded_modules(cpu);
        }
        /*
         * Pass 20: when NandColdBoot's Phase-1 produces a2<3
         * (0x19A3C88) or a3<3 (0x19A3CFC), log the block index
         * (fp=s8 / NAND_BLOCK_PAGES=32), counters, and a2/a3 so we
         * can identify WHICH block(s) failed OOB validation. Also
         * log at 0x19A3D60 (the actual v0=0 exit) for context.
         */
        else if (watch->pc == 0x019A3D14u) {
            /*
             * Pass 21 Objective 2: Exception 003 faulting store site.
             * t9 = *(0x019AB34C) + v0; sh t0, 0(t9). If t8/t9 reflect a
             * zero base pointer, the fault is a benign lazy check.
             */
            fprintf(stderr,
                "[NCB_BAT] hit=%u pc=0x%08" PRIx64
                " t0=0x%08" PRIx64 " t8=0x%08" PRIx64
                " t9=0x%08" PRIx64 " v0=0x%08" PRIx64
                " ra=0x%08" PRIx64 "\n",
                watch->hits, pc,
                (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T0],
                (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T8],
                (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T9],
                (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
                (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
        }
        else if (watch->pc == 0x019A3C88u ||
                 watch->pc == 0x019A3CFCu ||
                 watch->pc == 0x019A3D60u) {
            uint32_t sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
            unsigned char stkbuf[64];
            size_t si;

            uint32_t s8 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_FP];
            const char *tag =
                (watch->pc == 0x019A3C88u) ? "A2LT3" :
                (watch->pc == 0x019A3CFCu) ? "A3LT3" : "EXIT0";
            /* a2/a3 and t0-t3 come from args in current regs at bnez */
            fprintf(stderr,
                "[NCB_BAD] tag=%s hit=%u s8=0x%08x block=%u"
                " t0=0x%08" PRIx64 " t1=0x%08" PRIx64
                " t2=0x%08" PRIx64 " t3=0x%08" PRIx64
                " a2=0x%08" PRIx64 " a3=0x%08" PRIx64
                " at=0x%08" PRIx64 " ra=0x%08" PRIx64 "\n",
                tag, watch->hits, s8, s8 / 32u,
                (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T0],
                (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T1],
                (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T2],
                (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T3],
                (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
                (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
                (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_AT],
                (uint64_t)(uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
            (void)sp; (void)stkbuf; (void)si;
        }
        return;
    }
}

void be300_probe_note_load(struct cpu *cpu, uint64_t pc, uint64_t vaddr,
    const unsigned char *data, size_t len)
{
    be300_probe_log_mem(cpu, pc, vaddr, data, len, false);
}

void be300_probe_note_store(struct cpu *cpu, uint64_t pc, uint64_t vaddr,
    const unsigned char *data, size_t len)
{
    be300_probe_log_mem(cpu, pc, vaddr, data, len, true);
}
