#pragma once
#include <stdint.h>

/*
 * PMU — Power Management Unit (VR4131 hardware manual §12)
 *
 * Controls power modes (IDLE, SLEEP, HIBERNATE).
 * Stub implementation: accept writes, return stored values.
 * Writing PMUCNTREG.SLEEP or .HIBERNATE in a real system halts
 * the CPU; we ignore those bits for functional emulation.
 */

/* Register offsets from PMU base (0x0F000030) */
#define PMU_PMUINTREG      0x00u   /* PMU interrupt */
#define PMU_PMUCNTREG      0x02u   /* PMU control */
#define PMU_PMUTCLKDIVREG  0x04u   /* Timer clock divider */
#define PMU_PMUINTREG2     0x06u   /* PMU interrupt 2 (VR4131) */

typedef struct {
    uint16_t pmuintreg;
    uint16_t pmucntreg;
    uint16_t pmutclkdivreg;
    uint16_t pmuintreg2;
} pmu_state_t;

void     pmu_init (pmu_state_t *s);
uint32_t pmu_read (pmu_state_t *s, uint32_t offset, unsigned size);
void     pmu_write(pmu_state_t *s, uint32_t offset, unsigned size, uint32_t val);
