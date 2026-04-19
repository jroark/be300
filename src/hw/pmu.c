#include "pmu.h"

#include <stdio.h>

void pmu_init(pmu_state_t *s)
{
    /*
     * Cold boot: PMU registers at hardware-reset defaults (zero),
     * except PMUINTREG which latches RTCRST so NK can read the
     * reset cause (UM §12.2.1).
     *
     * HALTimer is not armed here — pmu_arm_initial() is called by
     * dev_vr41xx_init with the cpu-derived arm-cycle count and the
     * cold-reset thunk.
     */
    s->pmuintreg     = PMUINTREG_RTCRST;
    s->pmucntreg     = 0;
    s->pmutclkdivreg = 0;
    s->pmucnt2reg    = 0;
    s->pmuwaitreg    = 0;
    s->pmudivreg     = 0;

    s->last_count_sample        = 0;
    s->haltimer_remaining_cycles = 0;
    s->haltimer_arm_cycles       = 0;

    s->pending_reset_cause = 0;
    s->softrst_pending     = 0;

    s->cold_reset = NULL;
    s->cpu_opaque = NULL;
}

void pmu_arm_initial(pmu_state_t *s, uint64_t initial_count,
                     uint64_t arm_cycles,
                     void (*cold_reset)(void *opaque),
                     void *cpu_opaque)
{
    s->haltimer_arm_cycles       = arm_cycles;
    s->haltimer_remaining_cycles = arm_cycles;
    s->last_count_sample         = initial_count;
    s->cold_reset                = cold_reset;
    s->cpu_opaque                = cpu_opaque;
}

void pmu_apply_pending_reset_state(pmu_state_t *s, uint64_t cur_count)
{
    s->pmuintreg |= (uint16_t)s->pending_reset_cause;
    s->pending_reset_cause = 0;

    /*
     * Do NOT re-arm on reset. Per UM §12.2.2 the HALTIMERRST watchdog
     * is a one-time "program is running normally" acknowledgement, and
     * pmu_write(PMUCNTREG) with bit 2 set permanently disarms it (see
     * there). On the code path that reaches this function — HALTimer
     * expiry or SOFTRST — the watchdog must not spring back to life:
     * software has already taken over scheduling its own reset, and
     * re-arming would produce a perpetual 4 s-interval loop through
     * the warm path.
     */
    s->haltimer_remaining_cycles = 0;
    s->last_count_sample         = cur_count;
}

uint32_t pmu_read(pmu_state_t *s, uint32_t offset, unsigned size)
{
    (void)size;
    switch (offset) {
    case PMU_PMUINTREG:     return s->pmuintreg;
    case PMU_PMUCNTREG:     return s->pmucntreg;
    case PMU_PMUTCLKDIVREG: return s->pmutclkdivreg;
    case PMU_PMUCNT2REG:    return s->pmucnt2reg;
    case PMU_PMUWAITREG:    return s->pmuwaitreg;
    case PMU_PMUDIVREG:     return s->pmudivreg;
    default:
        return 0;
    }
}

void pmu_write(pmu_state_t *s, uint32_t offset, unsigned size, uint32_t val)
{
    (void)size;
    switch (offset) {
    case PMU_PMUINTREG:
        /* W1C: bits written as 1 clear the corresponding status bit. */
        s->pmuintreg &= (uint16_t)~val;
        break;

    case PMU_PMUCNTREG:
        s->pmucntreg = (uint16_t)val;
        if (val & PMUCNTREG_HALTIMERRST) {
            /*
             * UM §12.2.2: "The HALTIMERRST bit must be reset (write 1)
             * within about four seconds after power-on. Resetting the
             * HALTIMERRST bit enables the VR4131 to recognize that it
             * has been normally activated."
             *
             * This is a one-shot "program is running normally"
             * acknowledgement, not a periodic pet. The ROM / SPL / NK
             * boot sequence writes bit 2 several times in the first
             * ~seconds of boot (observed 0x0004 from ROM at 0xBFC00744,
             * 0x0006 from SPL at 0xA0F02524, 0x0006 and 0x1006 from
             * NK at several sites); any one of them disarms the
             * watchdog permanently. Real-HW behaviour: HALTimer never
             * actually fires during normal boot.
             */
            s->haltimer_remaining_cycles = 0;
            s->haltimer_arm_cycles       = 0;
        }
        break;

    case PMU_PMUTCLKDIVREG:
        s->pmutclkdivreg = (uint16_t)val;
        break;

    case PMU_PMUCNT2REG:
        s->pmucnt2reg = (uint16_t)val;
        if (val & PMUCNT2REG_SOFTRST) {
            /* Stage the RSTSW cause and let the MMIO dispatcher in
               dev_vr41xx.c supply cur_count and fire the thunk. This
               keeps pmu.c independent of gxemul CPU state. */
            s->pending_reset_cause = PMUINTREG_RSTSW;
            s->softrst_pending     = 1;
        }
        break;

    case PMU_PMUWAITREG:
        s->pmuwaitreg = (uint16_t)val;
        break;

    case PMU_PMUDIVREG:
        s->pmudivreg = (uint16_t)val;
        break;

    default:
        break;
    }
}

void pmu_tick(pmu_state_t *s, uint64_t cur_count)
{
    uint32_t delta32;
    uint64_t delta;

    if (s->haltimer_remaining_cycles == 0)
        return;

    /*
     * Compute the delta in 32-bit wrap-preserving arithmetic: in the
     * gxemul tree COP0_COUNT is stored sign-extended (dyntrans writes
     * `(int32_t)(old + n_instrs)`), and wraps at 0x100000000. Masking
     * to the low 32 bits and taking a uint32_t delta handles the
     * wrap correctly.
     *
     * Guests routinely write COP0_COUNT via mtc0 during timer init
     * (WinCE NK does this as part of OEMInit). When the guest resets
     * Count to a smaller value than last_count_sample, the uint32_t
     * subtraction wraps to a near-2^32 delta — which, if not filtered,
     * would clamp up to arm_cycles and fire the HALTimer spuriously.
     *
     * Filter: any delta larger than half the arm window is treated as
     * a Count-write resync, not elapsed cycles. We store the new
     * cur_count, reset last_count_sample, and count zero elapsed
     * time for this tick. The next tick will see a normal delta.
     */
    delta32 = (uint32_t)cur_count - (uint32_t)s->last_count_sample;
    s->last_count_sample = cur_count;

    if ((uint64_t)delta32 > s->haltimer_arm_cycles / 2)
        return;    /* Count-write resync; no elapsed-time charge */

    delta = (uint64_t)delta32;

    if (delta >= s->haltimer_remaining_cycles) {
        s->haltimer_remaining_cycles = 0;
        s->pending_reset_cause = PMUINTREG_TIMOUTRST;
        fprintf(stderr,
            "[PMU] HALTimer fired, issuing cold reset (cause=TIMOUTRST)\n");
        pmu_apply_pending_reset_state(s, cur_count);
        if (s->cold_reset != NULL)
            s->cold_reset(s->cpu_opaque);
        return;
    }

    s->haltimer_remaining_cycles -= delta;
}
