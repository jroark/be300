#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "machine.h"
#include "bus.h"
#include "loader.h"
#include "macc.h"

/*
 * In MIPS64 mode, kseg0/kseg1 virtual addresses (0x80000000–0xBFFFFFFF)
 * must be sign-extended to 64 bits for Unicorn.  Plain 0x80000000 is kuseg
 * in 64-bit address space; 0xFFFFFFFF80000000 is the correct kseg0 base.
 */
static inline uint64_t mips_sext(uint32_t va32) {
    return (uint64_t)(int32_t)va32;
}

static void update_irq_lines(machine_t *m)
{
    /*
     * VR41xx timer path:
     * RTC elapsed-time compare -> RTCINTREG.ELAPSEDTIME -> ICU SYSINT1 bit3.
     */
    if (m->rtc.rtcint & RTCINT_ELAPSEDTIME_INT)
        icu_assert(&m->icu, ICU_SRC1_ETIME);
    else
        icu_deassert(&m->icu, ICU_SRC1_ETIME);

    (void)m;
}

/*
 * Temporary timer shim:
 * linux4.be spins in calibrate_delay waiting for jiffies to change.
 * Until CP0 timer/interrupt delivery is fully emulated, bump jiffies
 * in RAM once per execution batch to keep early boot moving.
 */
static void tick_jiffies_hack(machine_t *m)
{
    static const uint32_t jiffies_pa = 0x001CD9E0u;
    uint32_t j = 0;
    if (uc_mem_read(m->uc, jiffies_pa, &j, sizeof(j)) == UC_ERR_OK) {
        j += 1;
        uc_mem_write(m->uc, jiffies_pa, &j, sizeof(j));
    }
}

/* Convert kseg0/kseg1 VA to physical; return false for non-direct-mapped VA. */
static bool va_to_pa_kseg(uint64_t va, uint64_t *pa_out)
{
    uint32_t va32 = (uint32_t)va;
    if ((va32 >= 0x80000000u && va32 <= 0xBFFFFFFFu) ||
        ((uint64_t)(int64_t)(int32_t)va32) == va) {
        *pa_out = (uint64_t)(va32 & 0x1FFFFFFFu);
        return true;
    }
    return false;
}

/*
 * Best-effort instruction read for hooks.
 * Unicorn sometimes reports virtual PC while uc_mem_read expects mapped PA.
 */
static bool read_insn_best_effort(uc_engine *uc, uint64_t address, uint32_t *insn)
{
    if (uc_mem_read(uc, address, insn, 4) == UC_ERR_OK)
        return true;

    uint64_t pa = 0;
    if (va_to_pa_kseg(address, &pa) && uc_mem_read(uc, pa, insn, 4) == UC_ERR_OK)
        return true;

    return false;
}

/* ------------------------------------------------------------------ */
/* Unicorn hooks                                                         */
/* ------------------------------------------------------------------ */

/*
 * Per-instruction trace hook.
 * Only installed when cfg.trace == true (significant performance impact).
 */
static void trace_hook(uc_engine *uc, uint64_t address,
                        uint32_t size, void *user_data)
{
    (void)size;
    machine_t *m = user_data;
    m->insn_count++;

    uint32_t insn = 0;
    if (!read_insn_best_effort(uc, address, &insn))
        insn = 0xFFFFFFFFu;

    /* Read a few key registers for context */
    uint32_t pc = (uint32_t)address;
    uint64_t at=0, v0=0, a0=0, sp=0, ra=0;
    uc_reg_read(uc, UC_MIPS_REG_AT, &at);
    uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
    uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
    uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra);

    fprintf(stderr, "[T] %08X: %08X  at=%016" PRIX64 " v0=%016" PRIX64
                    " a0=%016" PRIX64 " sp=%016" PRIX64 " ra=%016" PRIX64 "\n",
            pc, insn, at, v0, a0, sp, ra);
}

/*
 * PRId intercept hook.
 *
 * Unicorn runs with the NEC VR5432 CPU model (PRId = 0x00000400), but the
 * linux4.be kernel identifies the platform by checking CP0 PRId against the
 * VR4131 value (0x00000C80).  Unicorn does not expose CP0_PRID as writable,
 * so we intercept every `mfc0 $rt, $15` instruction (CP0 reg 15 = PRId):
 *
 *   Encoding: 0x4000_7800 | (rt << 16)
 *   Mask:     0xFFE0_FFFF  (fix all bits except the 5-bit rt field)
 *
 * When detected:
 *   1. Write VR4131_PRID to the destination GPR.
 *   2. Advance PC by 4 to skip the instruction (Unicorn respects PC changes
 *      made inside UC_HOOK_CODE before the instruction executes).
 */
static void prid_hook(uc_engine *uc, uint64_t address,
                      uint32_t size, void *user_data)
{
    (void)size;
    machine_t *m = user_data;
    static uint32_t mfc0_cause_seen_log = 0;
    static uint32_t mfc0_epc_seen_log = 0;
    static uint32_t mfc0_cause_inject_log = 0;
    static uint32_t mfc0_epc_inject_log = 0;

    uint32_t insn = 0;
    if (!read_insn_best_effort(uc, address, &insn))
        return;

    /* MFC0 $rt, $15, 0  — PRId (CP0 register 15, sel 0)
     * Encoding: 000100 00000 rt 01111 00000 000000
     *           op=0x10 rs=0  rt    rd=15  sel=0  */
    if ((insn & 0xFFE0FFFFu) == 0x40007800u) {
        uint32_t rt = (insn >> 16) & 0x1F;
        uint64_t prid = VR4131_PRID;
        uc_reg_write(uc, UC_MIPS_REG_0 + (int)rt, &prid);
        uint64_t next_pc = address + 4;
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
        return;
    }

    /*
     * MFC0 $rt, $13, 0  — Cause register (CP0 register 13, sel 0)
     * Encoding: 000100 00000 rt 01101 00000 000000
     *           op=0x10 rs=0  rt    rd=13  sel=0
     * Mask:     0xFFE0FFFF
     * Match:    0x40006800
     *
     * Unicorn 2.1.4 does not set CP0_Cause.ExcCode when routing a SYSCALL
     * exception.  The except_vec3_r4000 exception vector reads Cause to
     * dispatch to the right handler.  When a SYSCALL exception is pending
     * (m->pending_excode != 0), return a synthesised Cause value with the
     * correct ExcCode in bits[6:2].
     */
    if ((insn & 0xFFE0FFFFu) == 0x40006800u) {
        if (mfc0_cause_seen_log < 24) {
            fprintf(stderr,
                    "[CAUSE_MFC0] seen PC=0x%08" PRIX64 " pending_excode=%u served=%u cause=0x%08X\n",
                    (uint64_t)(uint32_t)address, m->pending_excode,
                    m->pending_cause_served ? 1u : 0u, m->pending_cause);
            mfc0_cause_seen_log++;
        }
    }
    bool irq_cause_stage_pc = ((uint32_t)address == 0x80000180u ||
                               (uint32_t)address == 0x80007700u ||
                               (uint32_t)address == 0x800077A8u);
    bool should_inject_cause = false;
    if (m->pending_excode == 1u) {
        /* External interrupt injection: serve at vector entry and
         * once more in vr41xx_handle_interrupt (which re-reads Cause). */
        should_inject_cause = irq_cause_stage_pc;
    } else if (m->pending_excode != 0) {
        /* SYSCALL and other synthetic exceptions: one-shot Cause inject. */
        should_inject_cause = !m->pending_cause_served;
    }
    if ((insn & 0xFFE0FFFFu) == 0x40006800u && should_inject_cause) {
        uint32_t rt = (insn >> 16) & 0x1F;
        /* Serve the injected Cause once (exception dispatch).  After this, let
         * QEMU's real Cause register pass through so that synchronous nested
         * exceptions (TLB invalid → page fault, ExcCode=3) are dispatched to
         * do_page_fault rather than a wrong syscall handler.
         * Asynchronous interrupts (CP0 timer, intno=27) reset pending_cause_served
         * in intr_hook before arriving here, so they still get the injected value. */
        uint64_t cause = m->pending_cause;
        uc_reg_write(uc, UC_MIPS_REG_0 + (int)rt, &cause);
        if (m->pending_excode == 1u) {
            /*
             * vr41xx_handle_interrupt re-reads Cause later in the path
             * (0x800077A8) while selecting the IRQ source. Keep IP2 visible
             * until that read has been serviced.
             */
            m->pending_cause_served = ((uint32_t)address == 0x800077A8u);
        } else {
            m->pending_cause_served = true;
        }
        if (mfc0_cause_inject_log < 24) {
            fprintf(stderr,
                    "[CAUSE_MFC0] inject PC=0x%08" PRIX64 " cause=0x%08" PRIX64 " rt=%u\n",
                    (uint64_t)(uint32_t)address, (uint64_t)(uint32_t)cause, rt);
            mfc0_cause_inject_log++;
        }
        uint64_t next_pc = address + 4;
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
        return;
    }

    /*
     * MFC0 $rt, $14, 0  — EPC register (CP0 register 14, sel 0)
     * Encoding: 000100 00000 rt 01110 00000 000000
     *           op=0x10 rs=0  rt    rd=14  sel=0
     * Mask:     0xFFE0FFFF
     * Match:    0x40007000
     *
     * Return the EPC saved during intr_hook so the exception handler
     * can correctly restore the return address before ERET.
     * Only intercept when we are inside our injected exception context
     * (pending_excode != 0).
     */
    if ((insn & 0xFFE0FFFFu) == 0x40007000u) {
        if (mfc0_epc_seen_log < 24) {
            fprintf(stderr,
                    "[EPC_MFC0] seen PC=0x%08" PRIX64 " pending_excode=%u served=%u epc=0x%08" PRIX64 "\n",
                    (uint64_t)(uint32_t)address, m->pending_excode,
                    m->pending_epc_served ? 1u : 0u, (uint64_t)(uint32_t)m->pending_epc);
            mfc0_epc_seen_log++;
        }
    }
    if ((insn & 0xFFE0FFFFu) == 0x40007000u &&
        m->pending_excode != 0 && !m->pending_epc_served) {
        uint32_t rt = (insn >> 16) & 0x1F;
        /* Serve the injected EPC once (SAVE_SOME frame build).  Subsequent MFC0
         * EPC reads (e.g. from nested exception handlers) get QEMU's real EPC so
         * that the correct return address is saved for each nested exception. */
        uint64_t epc = m->pending_epc;
        uc_reg_write(uc, UC_MIPS_REG_0 + (int)rt, &epc);
        m->pending_epc_served = true;
        if (mfc0_epc_inject_log < 24) {
            fprintf(stderr,
                    "[EPC_MFC0] inject PC=0x%08" PRIX64 " epc=0x%08" PRIX64 " rt=%u\n",
                    (uint64_t)(uint32_t)address, (uint64_t)(uint32_t)epc, rt);
            mfc0_epc_inject_log++;
        }
        uint64_t next_pc = address + 4;
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
        return;
    }

    /*
     * MTC0 $rt, $14, 0  — write EPC (CP0 register 14, sel 0)
     * Encoding: 000100 00100 rt 01110 00000 000000
     *           op=0x10 rs=4  rt    rd=14  sel=0
     * Match:    0x40807000 (rs=4 = MTC0)
     *
     * The syscall exit path writes the adjusted return address back to EPC
     * via MTC0 before ERET.  We capture that write so ERET returns correctly.
     * Only intercept when we are inside our injected exception context.
     * Outside that context, Unicorn's QEMU executes the real MTC0 EPC
     * instruction, keeping the CPU's internal CP0_EPC register consistent.
     */
    if ((insn & 0xFFE0FFFFu) == 0x40807000u && m->pending_excode != 0) {
        uint32_t rt = (insn >> 16) & 0x1F;
        uint64_t val = 0;
        uc_reg_read(uc, UC_MIPS_REG_0 + (int)rt, &val);
        m->pending_epc      = val;   /* capture return address written by exit path */
        m->epc_was_written  = true;  /* arm the ERET intercept for the next ERET    */
        uint64_t next_pc = address + 4;
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
        return;
    }

    /*
     * ERET  — return from exception.
     * Encoding: 0x42000018
     *
     * In normal MIPS SOFTMMU, ERET clears Status.EXL and jumps to EPC.
     * When we are managing the exception state manually (pending_epc != 0),
     * we perform the ERET ourselves:
     *   1. Clear EXL in Status.
     *   2. Set PC = pending_epc.
     *   3. Clear pending state.
     */
    /*
     * ERET intercept: only fire when the kernel's exit path has already
     * written the return address to EPC via MTC0 EPC (epc_was_written=true).
     *
     * Without this guard, the intercept would also fire for nested ERET
     * instructions from TLB refill handlers (0x80000000) that run during
     * user-space access faults inside the syscall/IRQ handler body.  Those
     * intermediate ERETs must be left to Unicorn's native ERET logic, which
     * reads the real CP0 EPC (set by the hardware TLB-miss exception entry)
     * and returns to the faulting instruction correctly.
     */
    if (insn == 0x42000018u && m->pending_excode != 0) {
        static uint32_t eret_pending_log = 0;
        if (eret_pending_log < 64) {
            fprintf(stderr,
                    "[ERET] pending_excode=%u epc_written=%u pending_epc=0x%08" PRIX64 " PC=0x%08" PRIX64 "\n",
                    m->pending_excode, m->epc_was_written ? 1u : 0u,
                    (uint64_t)(uint32_t)m->pending_epc, (uint64_t)(uint32_t)address);
            eret_pending_log++;
        }
        if (m->epc_was_written) {
            uint64_t status = 0;
            uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &status);
            status &= ~(uint64_t)0x2u;   /* clear EXL */
            uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &status);

            uint64_t epc = m->pending_epc;
            uc_reg_write(uc, UC_MIPS_REG_PC, &epc);

            /* (debug) fprintf(stderr, "[ERET] returning to EPC=0x%016" PRIX64 "\n", epc); */

            m->pending_epc          = 0;
            m->pending_excode       = 0;
            m->pending_cause        = 0;
            m->epc_was_written      = false;
            m->pending_cause_served = false;
            m->pending_epc_served   = false;
            return;
        }

        /*
         * If no explicit MTC0 EPC was observed, let Unicorn execute the
         * native ERET, but still clear synthetic exception bookkeeping so
         * we don't permanently block future injected interrupts.
         */
        m->pending_epc          = 0;
        m->pending_excode       = 0;
        m->pending_cause        = 0;
        m->epc_was_written      = false;
        m->pending_cause_served = false;
        m->pending_epc_served   = false;
    }
}

/*
 * Interrupt / CPU-exception hook.
 *
 * UC_HOOK_INTR fires for all Unicorn-level interrupts and CPU exceptions.
 * For MIPS SOFTMMU this includes hardware IRQ delivery AND CPU exceptions
 * such as SYSCALL (exception code 8 / EXCP_SYSCALL in QEMU).
 *
 * Strategy: on MIPS SYSCALL (intno == 8) Unicorn raises UC_ERR_EXCEPTION
 * instead of transparently routing to the guest exception vector.  We handle
 * it here by performing the MIPS exception-entry sequence ourselves:
 *   1. Capture the current PC (EPC — the address of the syscall instruction).
 *   2. Save EPC in a machine-local field so downstream MFC0/EPC intercepts
 *      can return it to the guest.
 *   3. Set CP0 Status.EXL = 1.
 *   4. Redirect PC to the general exception vector (0x80000180, BEV=0).
 *
 * Cause.ExcCode cannot be written via the Unicorn 2.1.4 API.  The
 * except_vec3_r4000 handler at 0x80000180 reads Cause with `mfc0 $k1,Cause`
 * (the very first instruction) and uses ExcCode to index exception_handlers[].
 * We intercept that specific read in prid_hook() below and inject ExcCode=8
 * (SYSCALL) when we are in an active SYSCALL entry.
 */

/* CP0 Cause ExcCode for SYSCALL (MIPS spec table 5-1) */
#define MIPS_EXCCODE_SYS  8u

static void intr_hook(uc_engine *uc, uint32_t intno, void *user_data)
{
    machine_t *m = user_data;
    static uint32_t intr_log_count[64];
    static uint32_t syscall_entry_log_count = 0;

    uint64_t pc = 0, status = 0;
    uc_reg_read(uc, UC_MIPS_REG_PC,         &pc);
    uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &status);

    /*
     * Unicorn 2.1.4 MIPS fires intno=17 for the SYSCALL instruction.
     * When the hook fires, Unicorn has already advanced PC to SYSCALL+4;
     * the real EPC (address of the syscall instruction) is PC-4.
     *
     * Log early interrupt events during development; only act when the event
     * looks like a SYSCALL (intno 17 on Unicorn 2.1.x, intno 26 on some
     * older Unicorn builds).
     */
    bool likely_syscall = false;
    if (intno == 17u || intno == 26u) {
        uint32_t maybe_sys = 0;
        if (pc >= 4u && read_insn_best_effort(uc, pc - 4u, &maybe_sys) &&
            maybe_sys == 0x0000000Cu)
            likely_syscall = true;
    }

    if (!likely_syscall) {
        uint32_t idx = intno < 64u ? intno : 63u;
        if (intr_log_count[idx] < 8u) {
            fprintf(stderr, "[INTR] intno=%u PC=0x%016" PRIX64
                            " STATUS=0x%016" PRIX64 "\n", intno, pc, status);
            intr_log_count[idx]++;
            if (intr_log_count[idx] == 8u)
                fprintf(stderr, "[INTR] intno=%u further logs suppressed\n", intno);
        }
        /*
         * Async interrupt (CP0 timer, external IRQ, …).  QEMU has already set
         * EXL=1, Cause, EPC and routed PC to the exception vector.
         *
         * Only reset one-shot serve flags when we are in our injected IRQ
         * context (pending_excode==1).  Doing this during an in-flight
         * SYSCALL (pending_excode==8) can re-arm syscall Cause injection and
         * mis-dispatch nested exceptions (e.g. page faults in execve).
         */
        if (m->pending_excode == 1u) {
            m->pending_cause_served = false;
            m->pending_epc_served   = false;
        }
        return;
    }

    /*
     * SYSCALL: perform manual MIPS exception entry.
     *
     *   EPC  = PC - 4  (address of the syscall instruction itself)
     *   EXL  = 1       (Status bit 1 — enter exception mode)
     *   PC   = 0x80000180  (general exception vector, BEV=0)
     *
     * Cause.ExcCode and EPC are not writable via Unicorn's 2.1.4 API.
     * They are emulated by intercepting the matching MFC0 / MTC0 / ERET
     * instructions inside prid_hook() below.
     */
    uint64_t epc = pc - 4u;   /* undo the PC advance Unicorn already applied */
    m->pending_epc          = epc;
    m->pending_excode       = MIPS_EXCCODE_SYS;
    m->pending_cause        = (uint32_t)(MIPS_EXCCODE_SYS << 2);  /* Cause.ExcCode = 8 */
    m->epc_was_written      = false;  /* reset; set by MTC0 EPC intercept on exit path */
    m->pending_cause_served = false;
    m->pending_epc_served   = false;

    if (syscall_entry_log_count < 32) {
        fprintf(stderr,
                "[SYSCALL_INJECT] EPC=0x%08" PRIX64 " cause=0x%08X STATUS=0x%08" PRIX64 "\n",
                (uint64_t)(uint32_t)epc, m->pending_cause, (uint64_t)(uint32_t)status);
        syscall_entry_log_count++;
    }

    uint64_t new_status = status | 0x2u;   /* set EXL */
    uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &new_status);

    uint64_t vec = mips_sext(0x80000180u);
    uc_reg_write(uc, UC_MIPS_REG_PC, &vec);

    /* (debug) fprintf(stderr, "[INTR] SYSCALL at EPC=0x%016" PRIX64
                    ", routing to exception vector\n", epc); */
}

/*
 * Inject a hardware interrupt (ExcCode=0 / INTR) into the guest CPU when
 * the VR41xx ICU has a pending, enabled interrupt source.
 *
 * Called once per execution batch (before reading PC) so the injected
 * exception vector is picked up as the start address of the next batch.
 *
 * Conditions for injection:
 *   1. No other injected exception is already in flight (pending_excode==0).
 *   2. ICU has at least one unmasked pending source (icu_pending()).
 *   3. CPU is receptive: Status.IE=1, Status.EXL=0, Status.ERL=0.
 *   4. Status.IM2 (bit 10) is set — HW0/INT0 interrupt line is unmasked.
 *
 * When all conditions are met, we perform the manual MIPS exception-entry
 * sequence (same pattern as intr_hook for SYSCALL):
 *   EPC  = current PC (instruction that would have run next)
 *   pending_cause = IP2 set (bit 10), ExcCode=0 (Interrupt)
 *   Status.EXL = 1
 *   PC = 0x80000180 (general exception vector, BEV=0)
 *
 * The exception is then handled by the guest: except_vec3_r4000 reads
 * Cause (returns pending_cause via prid_hook MFC0 intercept), finds
 * ExcCode=0, dispatches to handle_int, which reads the VR41xx ICU to
 * identify the source and calls the appropriate handler (e.g. do_timer
 * for the ETIME tick, which advances jiffies and wakes sleeping tasks).
 */
static void inject_hw_irq_if_pending(machine_t *m)
{
    static uint32_t log_block_pending_excode = 0;
    static uint32_t log_block_status = 0;
    static uint32_t log_block_im2 = 0;
    static uint32_t log_injected = 0;
    static uint32_t log_stale_pending = 0;
    bool pending = icu_pending(&m->icu);

    if (m->pending_excode != 0) {
        /*
         * Synthetic exception state should track EXL=1 while in-flight.
         * If EXL is already clear, the guest has effectively returned from
         * exception but we missed retirement bookkeeping; clear stale state.
         */
        uint64_t status64 = 0;
        uc_reg_read(m->uc, UC_MIPS_REG_CP0_STATUS, &status64);
        uint32_t status = (uint32_t)status64;
        if ((status & 0x2u) == 0) {
            if (log_stale_pending < 16) {
                uint64_t pc = 0;
                uc_reg_read(m->uc, UC_MIPS_REG_PC, &pc);
                fprintf(stderr,
                        "[IRQ_GATE] stale pending_excode=%u cleared (EXL=0) PC=0x%08" PRIX64
                        " STATUS=0x%08X pending_epc=0x%08" PRIX64 "\n",
                        m->pending_excode, (uint64_t)(uint32_t)pc, status,
                        (uint64_t)(uint32_t)m->pending_epc);
                log_stale_pending++;
            }
            m->pending_epc          = 0;
            m->pending_excode       = 0;
            m->pending_cause        = 0;
            m->epc_was_written      = false;
            m->pending_cause_served = false;
            m->pending_epc_served   = false;
        }
    }

    /* Only inject when no exception is already in flight */
    if (m->pending_excode != 0) {
        if (pending && log_block_pending_excode < 16) {
            uint64_t pc = 0;
            uc_reg_read(m->uc, UC_MIPS_REG_PC, &pc);
            fprintf(stderr,
                    "[IRQ_GATE] blocked: pending_excode=%u pending_cause=0x%08X pending_epc=0x%08" PRIX64
                    " PC=0x%08" PRIX64 " SYSINT1=0x%04X MSYSINT1=0x%04X RTCINT=0x%04X\n",
                    m->pending_excode, m->pending_cause,
                    (uint64_t)(uint32_t)m->pending_epc,
                    (uint64_t)(uint32_t)pc,
                    m->icu.sysint1, m->icu.msysint1, m->rtc.rtcint);
            log_block_pending_excode++;
        }
        return;
    }

    if (!pending)
        return;

    uint64_t status = 0;
    uc_reg_read(m->uc, UC_MIPS_REG_CP0_STATUS, &status);
    uint32_t s32 = (uint32_t)status;

    /* CPU must be receptive: IE=1, EXL=0, ERL=0 (bits [2:0] == 0b001) */
    if ((s32 & 0x7u) != 0x1u) {
        if (log_block_status < 16) {
            uint64_t pc = 0;
            uc_reg_read(m->uc, UC_MIPS_REG_PC, &pc);
            fprintf(stderr,
                    "[IRQ_GATE] blocked: status gate STATUS=0x%08X PC=0x%08" PRIX64
                    " SYSINT1=0x%04X MSYSINT1=0x%04X RTCINT=0x%04X\n",
                    s32, (uint64_t)(uint32_t)pc,
                    m->icu.sysint1, m->icu.msysint1, m->rtc.rtcint);
            log_block_status++;
        }
        return;
    }

    /* INT0/HW0 mapped to IP2 = Status bit 10; check it is not masked */
    if (!(s32 & (1u << 10))) {
        if (log_block_im2 < 16) {
            uint64_t pc = 0;
            uc_reg_read(m->uc, UC_MIPS_REG_PC, &pc);
            fprintf(stderr,
                    "[IRQ_GATE] blocked: IM2 masked STATUS=0x%08X PC=0x%08" PRIX64
                    " SYSINT1=0x%04X MSYSINT1=0x%04X RTCINT=0x%04X\n",
                    s32, (uint64_t)(uint32_t)pc,
                    m->icu.sysint1, m->icu.msysint1, m->rtc.rtcint);
            log_block_im2++;
        }
        return;
    }

    /* Inject: save return address, build Cause, redirect to exception vector */
    uint64_t pc = 0;
    uc_reg_read(m->uc, UC_MIPS_REG_PC, &pc);
    m->pending_epc          = pc;
    m->pending_excode       = 1u;           /* INTR sentinel — any non-zero value */
    m->pending_cause        = (1u << 10);   /* IP2 = HW0 = INT0, ExcCode = 0     */
    m->epc_was_written      = false;        /* reset; set by MTC0 EPC intercept   */
    m->pending_cause_served = false;
    m->pending_epc_served   = false;

    uint64_t new_status = status | 0x2u;  /* set EXL */
    uc_reg_write(m->uc, UC_MIPS_REG_CP0_STATUS, &new_status);

    uint64_t vec = mips_sext(0x80000180u);
    uc_reg_write(m->uc, UC_MIPS_REG_PC, &vec);

    if (log_injected < 16) {
        fprintf(stderr,
                "[IRQ_GATE] injected: EPC=0x%08" PRIX64 " STATUS=0x%08X"
                " pending_cause=0x%08X SYSINT1=0x%04X MSYSINT1=0x%04X RTCINT=0x%04X\n",
                (uint64_t)(uint32_t)pc, s32, m->pending_cause,
                m->icu.sysint1, m->icu.msysint1, m->rtc.rtcint);
        log_injected++;
    }
}

/*
 * Invalid instruction hook — handles VR4131 MACC instructions
 * (opcode 0x1C / SPECIAL2) which are not part of standard MIPS32.
 */
static bool insn_invalid_hook(uc_engine *uc, void *user_data)
{
    (void)user_data;
    uint64_t pc = 0;
    uc_reg_read(uc, UC_MIPS_REG_PC, &pc);

    uint32_t insn = 0;
    if (!read_insn_best_effort(uc, pc, &insn)) {
        fprintf(stderr, "[CPU] Cannot read insn at PC=0x%016" PRIX64 "\n", pc);
        return false;
    }

    /* Try MACC decode first */
    if (macc_execute(uc, insn))
        return true;

    /* Unknown instruction — log and stop */
    fprintf(stderr, "[CPU] Illegal instruction PC=0x%016" PRIX64 " insn=0x%08X\n",
            pc, insn);
    return false;
}

/* ------------------------------------------------------------------ */
/* One-shot checkpoint logging                                           */
/* ------------------------------------------------------------------ */

/*
 * Checkpoint table: VA of well-known kernel functions / branch sites.
 * These addresses are for linux4be20040908/vmlinux (confirmed via nm).
 * Each hook fires once and prints to stderr without perturbing execution.
 *
 * Flags:
 *   print_a0_str: read $a0 as a VA→string and print alongside the name.
 */
static const struct {
    uint32_t va;
    const char *name;
    bool print_a0_str;
} checkpoint_table[] = {
    /* ICU / timer init */
    { 0x80275b20u, "vr41xx_icu_init",                 false },
    /* High-level function entries */
    { 0x80001558u, "rest_init",                       false },
    { 0x80001580u, "do_pre_smp_initcalls",            false },
    { 0x800015d0u, "init (kernel thread)",            false },
    { 0x80272918u, "do_basic_setup",                  false },
    { 0x802727d0u, "do_initcalls",                    false },
    /* Fine-grained probes inside init() around the sys_access branch */
    { 0x80001614u, "init: JAL sys_access(/init?)",    false },
    { 0x8000161cu, "init: BNE (sys_access result)",   false },
    { 0x80001624u, "init: B   (skip prepare_ns)",     false },
    { 0x80001630u, "init: JAL prepare_namespace",     false },
    { 0x80273470u, "prepare_namespace (entry)",       false },
    { 0x80001638u, "init: JAL free_initmem",          false },
    /* exec path */
    { 0x80001690u, "init: JAL run_init_process [execute_command]", false },
    { 0x8000169cu, "init: JAL run_init_process [/sbin/init]",      false },
    { 0x80001598u, "run_init_process (entry)",        true  },
    { 0x80080cb0u, "do_execve (entry)",               true  },
    /* Post-inet_init initcalls (last two in table) */
    { 0x80286440u, "af_unix_init",                    false },
    { 0x802864d8u, "packet_init",                     false },
    /* Inside inet_init — call sites (JAL instructions) */
    { 0x80285f58u, "inet_init: JAL sock_register",    false },
    { 0x80286038u, "inet_init: JAL arp_init",         false },
    { 0x80286040u, "inet_init: JAL ip_init",          false },
    { 0x80286050u, "inet_init: JAL tcp_init",         false },
    /* Sub-function entries */
    { 0x80285458u, "ip_init",                         false },
    { 0x802854c0u, "tcp_init",                        false },
    { 0x80285a10u, "arp_init",                        false },
    { 0x80284fb8u, "ip_rt_init",                      false },
    { 0x80162dd0u, "ipfrag_init",                     false },
    { 0x80150378u, "neigh_table_init",                false },
    { 0x8027fd98u, "alloc_large_system_hash",         false },
    { 0x80286208u, "fib_hash_init",                   false },
    /* inet_register_protosw loop site */
    { 0x80286020u, "inet_init: JAL inet_register_protosw (loop)", false },
};
#define CHECKPOINT_COUNT ((int)(sizeof(checkpoint_table)/sizeof(checkpoint_table[0])))

static bool checkpoint_fired[CHECKPOINT_COUNT];

/*
 * Read a NUL-terminated string from guest VA into a host buffer.
 * Returns the number of bytes read (excluding NUL), or 0 on failure.
 */
static int read_guest_string(uc_engine *uc, uint64_t va, char *buf, int bufsz)
{
    /* Try direct PA (kseg0/kseg1) */
    uint64_t pa = 0;
    if (va_to_pa_kseg(va, &pa)) {
        int i;
        for (i = 0; i < bufsz - 1; i++) {
            uint8_t c = 0;
            if (uc_mem_read(uc, pa + i, &c, 1) != UC_ERR_OK) break;
            buf[i] = (char)c;
            if (c == 0) { buf[i] = 0; return i; }
        }
        buf[i] = 0;
        return i;
    }
    buf[0] = 0;
    return 0;
}

static void checkpoint_hook(uc_engine *uc, uint64_t address,
                            uint32_t size, void *user_data)
{
    (void)size; (void)address;
    int idx = (int)(uintptr_t)user_data;
    if (checkpoint_fired[idx]) return;
    checkpoint_fired[idx] = true;

    if (checkpoint_table[idx].print_a0_str) {
        uint64_t a0 = 0;
        uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
        char str[128] = "<unreadable>";
        read_guest_string(uc, a0, str, sizeof(str));
        fprintf(stderr, "[CHECKPOINT] >>> %s  a0=0x%08" PRIX64 " \"%s\" <<<\n",
                checkpoint_table[idx].name, a0, str);
    } else {
        fprintf(stderr, "[CHECKPOINT] >>> %s <<<\n", checkpoint_table[idx].name);
    }
}

/*
 * do_initcalls tracer: fires at the JALR site (0x80272874) inside
 * do_initcalls for every initcall invocation.  Reads $v0 ($2) which
 * holds the function pointer just before the JALR executes.
 * Limited to 64 fires to avoid log flooding.
 */
static int initcall_trace_count = 0;
static void initcall_trace_hook(uc_engine *uc, uint64_t address,
                                uint32_t size, void *user_data)
{
    (void)size; (void)address; (void)user_data;
    if (initcall_trace_count >= 64) return;
    initcall_trace_count++;
    uint64_t fn = 0;
    uc_reg_read(uc, UC_MIPS_REG_V0, &fn);
    fprintf(stderr, "[INITCALL] #%02d  fn=0x%08" PRIX64 "\n",
            initcall_trace_count, (uint64_t)(uint32_t)fn);
}

/* ------------------------------------------------------------------ */
/* RCU / wait_for_completion diagnostic probes                          */
/* ------------------------------------------------------------------ */

/*
 * Multi-fire probes for the RCU grace-period path.
 * Each fires up to RCU_PROBE_LIMIT times, printing a short line so we
 * can see whether these functions are reached while init is blocked in
 * wait_for_completion.
 *
 * Addresses for linux4be20040908/vmlinux (confirmed via nm):
 *   0x801ab590  wait_for_completion   — init should block here in synchronize_kernel
 *   0x800379a8  do_timer              — called from timer interrupt handler
 *   0x800428b8  rcu_check_callbacks   — called from scheduler_tick (via do_timer)
 *   0x80042780  rcu_process_callbacks — RCU tasklet; fires the call_rcu callbacks
 *   0x8000eb30  timer_interrupt       — top-level timer interrupt handler
 */
#define RCU_PROBE_LIMIT 8

static int rcu_probe_wait_count    = 0;
static int rcu_probe_dotimer_count = 0;
static int rcu_probe_rcucheck_count= 0;
static int rcu_probe_rcuproc_count = 0;
static int rcu_probe_timerint_count= 0;

static void rcu_probe_hook(uc_engine *uc, uint64_t address,
                           uint32_t size, void *user_data)
{
    (void)uc; (void)size;
    int idx = (int)(uintptr_t)user_data;
    int *cnt;
    const char *tag;
    switch (idx) {
        case 0: cnt = &rcu_probe_wait_count;     tag = "wait_for_completion";   break;
        case 1: cnt = &rcu_probe_dotimer_count;  tag = "do_timer";              break;
        case 2: cnt = &rcu_probe_rcucheck_count; tag = "rcu_check_callbacks";   break;
        case 3: cnt = &rcu_probe_rcuproc_count;  tag = "rcu_process_callbacks"; break;
        case 4: cnt = &rcu_probe_timerint_count; tag = "timer_interrupt";       break;
        default: return;
    }
    if (*cnt >= RCU_PROBE_LIMIT) return;
    (*cnt)++;
    fprintf(stderr, "[RCU_PROBE] #%d %s  PC=0x%08" PRIX64 "\n",
            *cnt, tag, (uint64_t)(uint32_t)address);
}

/* ------------------------------------------------------------------ */
/* IRQ path probes                                                      */
/* ------------------------------------------------------------------ */

#define IRQ_PROBE_LIMIT 24

static int irq_probe_counts[8];

static void irq_probe_hook(uc_engine *uc, uint64_t address,
                           uint32_t size, void *user_data)
{
    (void)size;
    int idx = (int)(uintptr_t)user_data;
    const char *tag = NULL;
    switch (idx) {
        case 0: tag = "vr41xx_handle_interrupt"; break;
        case 1: tag = "irq_dispatch";            break;
        case 2: tag = "do_IRQ";                  break;
        case 3: tag = "timer_interrupt";         break;
        case 4: tag = "vr41xx_timer_ack";        break;
        case 5: tag = "ll_timer_interrupt";      break;
        case 6: tag = "local_timer_interrupt";   break;
        case 7: tag = "ll_local_timer_interrupt"; break;
        default: return;
    }
    if (irq_probe_counts[idx] >= IRQ_PROBE_LIMIT)
        return;
    irq_probe_counts[idx]++;

    uint64_t a0 = 0;
    uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
    fprintf(stderr, "[IRQ_PROBE] #%d %s  PC=0x%08" PRIX64 " a0=0x%08" PRIX64 "\n",
            irq_probe_counts[idx], tag, (uint64_t)(uint32_t)address,
            (uint64_t)(uint32_t)a0);
}

/* ------------------------------------------------------------------ */
/* ICU MSYSINT1 ETIME fixup                                              */
/* ------------------------------------------------------------------ */

/*
 * vr41xx_icu_init (called from arch_init_irq → vr41xx_init_IRQ) correctly
 * sets icu1_base = 0xAF000080 in kernel data, but then writes 0x0000 to
 * MSYSINT1REG — disabling ALL SYSINT1 interrupt sources, including ETIME
 * (bit 3).
 *
 * The timer was registered via setup_irq(11, …) during time_init(), but
 * at that point icu1_base may have been 0, so the enable_irq write was
 * silently lost.  Subsequent read-modify-write sequences on MSYSINT1REG
 * do not re-enable ETIME because it was never in the initial mask.
 *
 * Fix: intercept the `jr $ra` return instruction of vr41xx_icu_init at
 * 0x80275bec (confirmed by objdump).  At that point the function body has
 * already run (msysint1 = 0), so we force bit 3 (ETIME) back on before
 * control returns to the caller.  The one-shot guard prevents repeated
 * fires (both the normal and error paths share this return site).
 */
static bool icu_etime_fixup_fired = false;

static void icu_etime_fixup_hook(uc_engine *uc, uint64_t address,
                                  uint32_t size, void *user_data)
{
    (void)uc; (void)address; (void)size;
    if (icu_etime_fixup_fired) return;
    icu_etime_fixup_fired = true;
    machine_t *m = user_data;
    m->icu.msysint1 |= ICU_SRC1_ETIME;
    fprintf(stderr,
            "[ICU_FIXUP] vr41xx_icu_init returning; forced ETIME in MSYSINT1=0x%04X\n",
            m->icu.msysint1);
}

/* ------------------------------------------------------------------ */
/* Machine lifecycle                                                     */
/* ------------------------------------------------------------------ */

machine_t *machine_create(const machine_config_t *cfg)
{
    machine_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;

    m->cfg = *cfg;
    if (m->cfg.sdram_size == 0)
        m->cfg.sdram_size = 16u * 1024u * 1024u;   /* 16 MB default */

    /*
     * Open the Unicorn engine: MIPS64 little-endian.
     *
     * The linux4.be vmlinux uses 133k+ MIPS-III 64-bit instructions (LD, SD,
     * DADDIU, LDL, LDR, BEQL, BNEL, …).  MIPS32 mode raises Reserved Instruction
     * exceptions for all of these.  MIPS64 with the NEC VR5432 CPU model supports
     * the full MIPS-III 64-bit ISA.
     *
     * The kernel runs in 32-bit address mode (CP0 Status KX/SX/UX = 0), which
     * keeps kseg0/kseg1 segment translation identical to MIPS32.
     */
    uc_err err = uc_open(UC_ARCH_MIPS,
                         UC_MODE_MIPS64 | UC_MODE_LITTLE_ENDIAN,
                         &m->uc);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "[MACHINE] uc_open failed: %s\n", uc_strerror(err));
        free(m);
        return NULL;
    }

    /*
     * Map physical memory BEFORE setting the CPU model.
     * uc_ctl_set_cpu_model() for MIPS32_4KC creates internal QEMU machine
     * regions (including a boot-ROM stub at/near PA 0) that can overlap with
     * our SDRAM range.  By calling bus_init first we win the region claim.
     */
    bus_init(m);

    /*
     * Select the NEC VR5432 CPU model (MIPS IV, NEC VR series).
     * This is the closest available Unicorn MIPS64 model to the VR4131 and
     * supports the full MIPS-III 64-bit instruction set the kernel relies on.
     */
    uc_ctl_set_cpu_model(m->uc, UC_CPU_MIPS64_VR5432);

    /* Set CP0 Status for reset state: BEV=1 (boot vectors), ERL=1 */
    uint64_t status = 0x00400004u;
    uc_reg_write(m->uc, UC_MIPS_REG_CP0_STATUS, &status);
    bcu_init(&m->bcu);
    cmu_init(&m->cmu);
    pmu_init(&m->pmu);
    icu_init(&m->icu);
    siu_init(&m->siu);
    rtc_init(&m->rtc);
    gpio_init(&m->gpio);

    /* Always install the invalid-instruction hook (for MACC) */
    uc_hook hk;
    uc_hook_add(m->uc, &hk, UC_HOOK_INSN_INVALID,
                insn_invalid_hook, m, 1, 0);

    /*
     * Interrupt / CPU-exception hook.
     * Handles MIPS SYSCALL exceptions that Unicorn 2.1.4 does not
     * transparently route to the guest exception vector.
     */
    uc_hook_add(m->uc, &hk, UC_HOOK_INTR, intr_hook, m, 1, 0);

    /*
     * PRId intercept hook — fires for every instruction, checks for
     * MFC0 $rt, $15 and substitutes VR4131_PRID (0x00000C80).
     * This hook is always installed (low per-instruction cost: one read +
     * one compare; only acts on the rare MFC0 PRId instruction).
     */
    uc_hook_add(m->uc, &hk, UC_HOOK_CODE, prid_hook, m, 1, 0);

    /* Trace hook (only when requested — high overhead) */
    if (cfg->trace)
        uc_hook_add(m->uc, &hk, UC_HOOK_CODE, trace_hook, m, 1, 0);

    /* One-shot checkpoint hooks: fire once when each named function is entered */
    memset(checkpoint_fired, 0, sizeof(checkpoint_fired));
    for (int i = 0; i < CHECKPOINT_COUNT; i++) {
        uint64_t va = mips_sext(checkpoint_table[i].va);
        uc_hook_add(m->uc, &hk, UC_HOOK_CODE, checkpoint_hook,
                    (void *)(uintptr_t)i, va, va);
    }

    /* do_initcalls tracer: logs which function pointer is called at JALR site */
    initcall_trace_count = 0;
    {
        uint64_t jalr_va = mips_sext(0x80272874u);
        uc_hook_add(m->uc, &hk, UC_HOOK_CODE, initcall_trace_hook, NULL,
                    jalr_va, jalr_va);
    }

    /* RCU / wait_for_completion diagnostic probes (multi-fire, up to 8 each) */
    rcu_probe_wait_count     = 0;
    rcu_probe_dotimer_count  = 0;
    rcu_probe_rcucheck_count = 0;
    rcu_probe_rcuproc_count  = 0;
    rcu_probe_timerint_count = 0;
    {
        static const struct { uint32_t va; int idx; } rcu_probes[] = {
            { 0x801ab590u, 0 },  /* wait_for_completion   */
            { 0x800379a8u, 1 },  /* do_timer               */
            { 0x800428b8u, 2 },  /* rcu_check_callbacks    */
            { 0x80042780u, 3 },  /* rcu_process_callbacks  */
            { 0x8000eb30u, 4 },  /* timer_interrupt        */
        };
        for (int i = 0; i < 5; i++) {
            uint64_t va = mips_sext(rcu_probes[i].va);
            uc_hook_add(m->uc, &hk, UC_HOOK_CODE, rcu_probe_hook,
                        (void *)(uintptr_t)rcu_probes[i].idx, va, va);
        }
    }

    /* IRQ dispatch path probes (multi-fire, up to 24 each) */
    memset(irq_probe_counts, 0, sizeof(irq_probe_counts));
    {
        static const struct { uint32_t va; int idx; } irq_probes[] = {
            { 0x800076c0u, 0 },  /* vr41xx_handle_interrupt */
            { 0x80007508u, 1 },  /* irq_dispatch            */
            { 0x80009c30u, 2 },  /* do_IRQ                  */
            { 0x8000eb30u, 3 },  /* timer_interrupt         */
            { 0x80007a98u, 4 },  /* vr41xx_timer_ack        */
            { 0x8000ed88u, 5 },  /* ll_timer_interrupt      */
            { 0x8000ea38u, 6 },  /* local_timer_interrupt   */
            { 0x8000ee18u, 7 },  /* ll_local_timer_interrupt*/
        };
        for (int i = 0; i < 8; i++) {
            uint64_t va = mips_sext(irq_probes[i].va);
            uc_hook_add(m->uc, &hk, UC_HOOK_CODE, irq_probe_hook,
                        (void *)(uintptr_t)irq_probes[i].idx, va, va);
        }
    }

    /* ICU ETIME fixup: force-enable ETIME bit in MSYSINT1 after
     * vr41xx_icu_init clears it.  Fires at the jr $ra (0x80275bec). */
    icu_etime_fixup_fired = false;
    {
        uint64_t va = mips_sext(0x80275becu);
        uc_hook_add(m->uc, &hk, UC_HOOK_CODE, icu_etime_fixup_hook, m, va, va);
    }

    /* Kernel direct-boot mode: load ELF and set entry point */
    if (cfg->kernel_path) {
        uint32_t entry_va = 0;
        if (loader_load_elf(m, cfg->kernel_path, &entry_va) != 0) {
            fprintf(stderr, "[MACHINE] Kernel ELF load failed\n");
            machine_destroy(m);
            return NULL;
        }
        m->kernel_entry = mips_sext(entry_va);

        /*
         * MIPS Linux prom_init() calling convention (linux4.be / arch/mips/vr41xx):
         *   $a0 = argc  (0 = no args, or 1 if cmdline provided)
         *   $a1 = argv  (pointer to cmdline string in low SDRAM, or 0)
         *   $a2 = 0     (envp, unused by prom_init)
         *   $a3 = 0     (reserved / memory descriptor ptr — stub 0)
         *
         * The cmdline string (if any) is placed at a well-known low-SDRAM
         * address (0x00000200) so prom_init can find it.
         */
        uint64_t a0 = 0, a1 = 0;
        if (cfg->cmdline && cfg->cmdline[0]) {
            const char *cl = cfg->cmdline;
            uint32_t   cl_pa = 0x00000200u;   /* physical backing in low SDRAM */
            uint32_t   cl_va = 0xA0000200u;   /* kseg1 virtual pointer seen by kernel */
            const char *arg0 = "be300";
            uint32_t arg0_pa = 0x00000240u;
            uint32_t arg0_va = 0xA0000240u;
            uc_mem_write(m->uc, cl_pa, cl, strlen(cl) + 1);
            uc_mem_write(m->uc, arg0_pa, arg0, strlen(arg0) + 1);
            /* Build argv[] in RAM and pass virtual address in $a1. */
            uint32_t argv_pa = 0x000001F0u;
            uint32_t argv_va = 0xA00001F0u;
            uint32_t argv_words[3] = { arg0_va, cl_va, 0 };
            uc_mem_write(m->uc, argv_pa, argv_words, sizeof(argv_words));
            a0 = 2;
            a1 = mips_sext(argv_va);
        }
        uint64_t a2 = 0, a3 = 0;
        uc_reg_write(m->uc, UC_MIPS_REG_A0, &a0);
        uc_reg_write(m->uc, UC_MIPS_REG_A1, &a1);
        uc_reg_write(m->uc, UC_MIPS_REG_A2, &a2);
        uc_reg_write(m->uc, UC_MIPS_REG_A3, &a3);

        /*
         * For direct kernel boot, relax CP0 Status to kernel mode:
         * BEV=0 (normal exception vectors), EXL=0, ERL=0, KSU=00 (kernel).
         * IM bits all enabled so the kernel can configure them itself.
         */
        uint64_t kstatus = 0x0000FF00u; /* IM7-IM0 all set, no EXL/ERL/BEV */
        uc_reg_write(m->uc, UC_MIPS_REG_CP0_STATUS, &kstatus);

        /*
         * Pre-fill BCU CLKSPEEDREG for VR4131-style encoding:
         *   - CLKSP   bits [4:0]  : core clock step
         *   - VTDIV   bits [10:8] : VT clock divisor selector (must be non-zero)
         *   - TDIVMODE bit  [12]  : 0=/2, 1=/4
         *
         * The previous value (0x004A) left VTDIV=0 and linux4.be executed a
         * guarded DIVU with divisor 0, triggering BREAK.
         */
        m->bcu.clkspeedreg = 0x1108u; /* TDIV=/4, VTDIV=1, CLKSP=8 */

        /* Verify entry-point bytes are correctly loaded */
        {
            uint32_t pa_entry = entry_va & 0x1FFFFFFFu;
            uint32_t insns[4] = {0};
            uc_mem_read(m->uc, pa_entry, insns, sizeof(insns));
            fprintf(stderr, "[MACHINE] Entry PA=0x%08X  insns: %08X %08X %08X %08X\n",
                    pa_entry, insns[0], insns[1], insns[2], insns[3]);
        }

    } else {
        /* ROM boot mode: load flat binary at reset vector */
        m->kernel_entry = mips_sext(VA_RESET_VECTOR);

        if (cfg->rom_path) {
            if (loader_load_rom(m, cfg->rom_path) != 0) {
                fprintf(stderr, "[MACHINE] ROM load failed\n");
                machine_destroy(m);
                return NULL;
            }
        }
    }

    /* Optionally preload RAM (both modes) */
    if (cfg->ram_path)
        loader_load_ram(m, cfg->ram_path);

    return m;
}

void machine_destroy(machine_t *m)
{
    if (!m) return;
    if (m->uc) uc_close(m->uc);
    free(m);
}

/*
 * Main execution loop.
 *
 * Run in batches of BATCH_SIZE instructions.  Between batches we check
 * for ICU interrupt state and advance the RTC — this is the point where
 * future interrupt injection will be added.
 *
 * Unicorn with MIPS softmmu handles TLB misses, CP0 exceptions, ERET, etc.
 * internally; they are transparent to this loop.
 */
#define BATCH_SIZE 100000u

void machine_run(machine_t *m)
{
    m->running    = true;
    m->insn_count = 0;
    int write_unmapped_recoveries = 0;

    fprintf(stderr, "[MACHINE] Starting execution at VA 0x%016" PRIX64 "\n",
            m->kernel_entry);

    /* Set PC to the entry point (sign-extended 64-bit VA for MIPS64 mode) */
    uc_reg_write(m->uc, UC_MIPS_REG_PC, &m->kernel_entry);

    while (m->running) {
        /* Advance simulated time and update peripheral interrupt state.
         * The VR4131 RTC runs at ~32.768 kHz; 33 ticks ≈ 1 ms per batch. */
        rtc_tick(&m->rtc, 33);
        tick_jiffies_hack(m);
        update_irq_lines(m);

        /* Inject a hardware interrupt (INTR, ExcCode=0) if the ICU has a
         * pending, unmasked source and the CPU is receptive.  The injected
         * PC redirect is picked up by the uc_reg_read(PC) below. */
        inject_hw_irq_if_pending(m);

        uint64_t pc = 0;
        uc_reg_read(m->uc, UC_MIPS_REG_PC, &pc);

        /* Periodic PC sample: log PC every 100 batches (~10M insns) to
         * show where execution is spending time when silent. */
        if ((m->insn_count / BATCH_SIZE) % 100 == 0) {
            fprintf(stderr, "[PROGRESS] insns=%" PRIu64 "M  PC=0x%08" PRIX64 "\n",
                    m->insn_count / 1000000, (uint64_t)(uint32_t)pc);
        }

        uc_err err = uc_emu_start(m->uc, pc, 0, 0, BATCH_SIZE);

        if (err != UC_ERR_OK) {
            uint64_t bad_pc = 0;
            uc_reg_read(m->uc, UC_MIPS_REG_PC, &bad_pc);
            if (err == UC_ERR_WRITE_UNMAPPED && write_unmapped_recoveries < 8) {
                /*
                 * Temporary recovery probe for first-userspace transition:
                 * create_elf_tables() writes initial user-stack data and we
                 * currently hit UC_ERR_WRITE_UNMAPPED.  Try mapping nearby
                 * candidate blocks from key registers and retry.
                 */
                uint64_t v0 = 0, a0 = 0, t2 = 0;
                uc_reg_read(m->uc, UC_MIPS_REG_V0, &v0);
                uc_reg_read(m->uc, UC_MIPS_REG_A0, &a0);
                uc_reg_read(m->uc, UC_MIPS_REG_T2, &t2);
                uint64_t candidates[3] = { v0, a0, t2 };
                const char *names[3] = { "v0", "a0", "t2" };
                bool mapped_any = false;

                for (int i = 0; i < 3; i++) {
                    uint64_t va = candidates[i];
                    if (va < 0x1000u || va >= 0x80000000u)
                        continue;
                    uint64_t block = va & ~((uint64_t)0xFFFFF);
                    uc_err me = uc_mem_map(m->uc, block, 0x100000, UC_PROT_ALL);
                    if (me == UC_ERR_OK || me == UC_ERR_MAP) {
                        if (!mapped_any) {
                            fprintf(stderr,
                                    "[MACHINE] write-unmapped recovery #%d at PC=0x%08" PRIX64 "\n",
                                    write_unmapped_recoveries + 1,
                                    (uint64_t)(uint32_t)bad_pc);
                        }
                        fprintf(stderr,
                                "[MACHINE]   mapped block 0x%08" PRIX64 " via $%s=0x%08" PRIX64 "\n",
                                (uint64_t)(uint32_t)block, names[i],
                                (uint64_t)(uint32_t)va);
                        mapped_any = true;
                    }
                }

                if (mapped_any) {
                    write_unmapped_recoveries++;
                    continue;
                }
            }

            uint64_t status = 0;
            uint32_t insn = 0;
            uc_reg_read(m->uc, UC_MIPS_REG_CP0_STATUS, &status);
            if (!read_insn_best_effort(m->uc, bad_pc, &insn))
                insn = 0xFFFFFFFFu;
            fprintf(stderr,
                    "[MACHINE] uc_emu_start error at PC=0x%016" PRIX64 ": %s\n",
                    bad_pc, uc_strerror(err));
            fprintf(stderr,
                    "[MACHINE] pending_excode=%u pending_cause=0x%08X pending_epc=0x%016" PRIX64 "\n",
                    m->pending_excode, m->pending_cause, m->pending_epc);
            /* Dump all 32 GPRs to help identify the faulting address */
            static const char *gpr_names[] = {
                "zero","at","v0","v1","a0","a1","a2","a3",
                "t0","t1","t2","t3","t4","t5","t6","t7",
                "s0","s1","s2","s3","s4","s5","s6","s7",
                "t8","t9","k0","k1","gp","sp","fp","ra"
            };
            for (int r = 0; r < 32; r++) {
                uint64_t val = 0;
                uc_reg_read(m->uc, UC_MIPS_REG_0 + r, &val);
                fprintf(stderr, "  $%-4s = 0x%016" PRIX64 "\n", gpr_names[r], val);
            }
            fprintf(stderr,
                    "[MACHINE] STATUS=0x%016" PRIX64 " INSN=0x%08X\n",
                    status, insn);
            m->running = false;
            break;
        }

        if (!m->cfg.trace)
            m->insn_count += BATCH_SIZE;
    }

    fprintf(stderr, "[MACHINE] Stopped after %" PRIu64 " instructions\n",
            m->insn_count);
}

void machine_stop(machine_t *m)
{
    m->running = false;
    uc_emu_stop(m->uc);
}
