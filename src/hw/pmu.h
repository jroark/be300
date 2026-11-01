#pragma once
#include <stdint.h>

/*
 * PMU — Power Management Unit (VR4131 hardware manual §12)
 *
 *   - PMUINTREG (0x00, W1C): reset-cause and wake-cause bits (UM §12.2.1,
 *     U15350EJ2V0UM p.225-226). Set by hardware on power-on (RTCRST),
 *     HALTimer fire (TIMOUTRST), reset switch or SOFTRST (RSTSW), a
 *     battery too low to start (BATTINH) and an Elapsed Time alarm wake
 *     (RTCINTR). A power-switch wake from Hibernate sets none of them
 *     (UM §12.1.3(1): "Activation via power switch interrupt never sets
 *     PMUINTREG's POWERSWINTR bit to 1"). DCDST is read-only and shows the
 *     DCD# pin. NK reads this to decide cold vs. warm-restart branching.
 *   - PMUCNTREG (0x02): control. Bit 2 (HALTIMERRST) must be written
 *     1 within ~4 s of power-on (UM §12.2.2) or the HALTimer fires and
 *     cold-resets the CPU with SDRAM preserved.
 *   - PMUCNT2REG (0x06): control 2. Bit 4 (SOFTRST) triggers an
 *     immediate cold reset equivalent to a reset-switch press.
 *
 * This header stays free of any gxemul type. The dev_vr41xx.c caller
 * captures CP0 Count samples and the struct-cpu pointer and passes
 * them as plain integers / void* so pmu.c stays CPU-agnostic.
 */

/* Register offsets from PMU base (0x0F0000C0 on VR4131) */
#define PMU_PMUINTREG      0x00u   /* PMU interrupt status (W1C) */
#define PMU_PMUCNTREG      0x02u   /* PMU control (incl. HALTIMERRST bit 2) */
#define PMU_PMUTCLKDIVREG  0x04u   /* Timer clock divider (legacy alias) */
#define PMU_PMUCNT2REG     0x06u   /* PMU control 2 (incl. SOFTRST bit 4) */
#define PMU_PMUWAITREG     0x08u   /* PMU wait counter */
#define PMU_PMUDIVREG      0x0Cu   /* PMU divide mode */

/* PMUINTREG bits (UM §12.2.1, U15350EJ2V0UM p.225-226). Write 1 to clear,
 * but for DCDST (read-only pin state), RFU bit 2 and the memo bits 7:6,
 * which software reads and writes freely. */
#define PMUINTREG_POWERSWINTR (1u << 0)  /* power switch, while running */
#define PMUINTREG_BATTINTR    (1u << 1)  /* battery low, while running */
#define PMUINTREG_RSTSW       (1u << 3)  /* reset switch / SOFTRST */
#define PMUINTREG_RTCRST      (1u << 4)  /* power-on: the RTC was reset */
#define PMUINTREG_TIMOUTRST   (1u << 5)  /* HALTimer expiry */
#define PMUINTREG_MEMO        (3u << 6)  /* free for software */
#define PMUINTREG_BATTINH     (1u << 8)  /* battery too low at power-on */
#define PMUINTREG_RTCINTR     (1u << 9)  /* woken by the Elapsed Time alarm */
#define PMUINTREG_DCDST       (1u << 10) /* DCD# pin high (read-only) */
#define PMUINTREG_CLKRUNINTR  (1u << 11)
#define PMUINTREG_GPIOINTR(n) (1u << (12 + (n)))  /* GPIO0-3 activation */

/* PMUCNTREG control bits (UM §12.2.2) */
#define PMUCNTREG_HALTIMERRST (1u << 2)  /* write 1: pet HALTimer */

/* PMUCNT2REG control bits (UM §12.2.4) */
#define PMUCNT2REG_SOFTRST    (1u << 4)  /* write 1: trigger cold reset */

typedef struct {
    uint16_t pmuintreg;
    uint16_t pmucntreg;
    uint16_t pmutclkdivreg;
    uint16_t pmucnt2reg;
    uint16_t pmuwaitreg;
    uint16_t pmudivreg;

    /* HALTimer countdown (UM §12.2.2 ~4 s watchdog). haltimer_window is
       the full window, armed again on every wake from Hibernate. */
    uint64_t last_count_sample;
    uint64_t haltimer_remaining_cycles;
    uint64_t haltimer_arm_cycles;
    uint64_t haltimer_window;

    /* DCD# pin level for DCDST. The BE-300 reads it high: PMUINTREG 0x0400
       in docs/hardware/hw_dump_vr4131.txt:35, and 0x0400/0x0600 on a real
       BE-300 after Glaze's Hibernate wakes (2026-10-31). */
    uint8_t  dcd_high;

    /* Cold-reset staging: set by pmu_tick / pmu_write (SOFTRST), applied
       by pmu_apply_pending_reset_state before the cold_reset call. */
    uint32_t pending_reset_cause;
    uint8_t  softrst_pending;

    /* Cold-reset dispatch (wired by dev_vr41xx.c via pmu_arm_initial). */
    void   (*cold_reset)(void *opaque);
    void    *cpu_opaque;
} pmu_state_t;

void     pmu_init (pmu_state_t *s);
uint32_t pmu_read (pmu_state_t *s, uint32_t offset, unsigned size);
void     pmu_write(pmu_state_t *s, uint32_t offset, unsigned size, uint32_t val);

/*
 * Arm the HALTimer on power-on (RTCRST). Called once from
 * dev_vr41xx_init immediately after pmu_init(). Captures the baseline
 * COUNT sample, records the full arm-cycle window (for re-arming on
 * each HALTIMERRST pet and after every reset), and wires the
 * cold-reset thunk that bridges to gxemul CPU code.
 */
void pmu_arm_initial(pmu_state_t *s, uint64_t initial_count,
                     uint64_t arm_cycles,
                     void (*cold_reset)(void *opaque),
                     void *cpu_opaque);

/*
 * DEVICE_TICK hook. Decrements haltimer_remaining_cycles by the CP0
 * Count delta since the last sample; on expiry stages TIMOUTRST and
 * invokes the cold-reset thunk.
 */
void pmu_tick(pmu_state_t *s, uint64_t cur_count);

/*
 * Apply pending post-reset PMU state (cause-bit OR, HALTimer re-arm,
 * last_count_sample resync). Called by pmu_tick expiry and by the
 * dev_vr41xx MMIO post-write hook that handles SOFTRST. PMUCNTREG,
 * PMUCNT2REG, PMUTCLKDIVREG, PMUWAITREG, PMUDIVREG are preserved per
 * UM §12.1.2.
 */
void pmu_apply_pending_reset_state(pmu_state_t *s, uint64_t cur_count);

/*
 * A reset by the RSTSW# pin (the reset switch, or a board part driving it):
 * stages RSTSW like SOFTRST, for pmu_apply_pending_reset_state.
 */
void pmu_reset_switch(pmu_state_t *s);

/*
 * The CPU starts again after HIBERNATE (UM §12.1.3): `cause` is
 * PMUINTREG_RTCINTR for the Elapsed Time alarm and 0 for the power switch,
 * which sets nothing. The HALTimer is armed again, since software must
 * clear it within about 4 s of any activation (UM §12.1.2(1)).
 */
void pmu_hibernate_wake(pmu_state_t *s, uint32_t cause, uint64_t cur_count);
