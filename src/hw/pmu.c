#include <stdio.h>
#include "pmu.h"

void pmu_init(pmu_state_t *s)
{
    /*
     * WORKAROUND: Seed PMU registers from warm-state hardware survey.
     *
     * On a truly cold boot (battery removed), these registers would
     * hold their hardware-reset defaults (likely all zeros or
     * chip-specific POR values from the VR4131 datasheet).  We seed
     * them with values captured from a running device
     * (hardware_survey/HardwareDump6.txt) because the exact POR
     * defaults are not documented.
     *
     * Dump at VR4131 PA 0x0F0000C0 (LE 32-bit words):
     *   10060400 00000000 00000148 00000002
     *
     * Halfword register mapping:
     *   0xC0 PMUINTREG     = 0x0400  (interrupt status)
     *   0xC2 PMUCNTREG     = 0x1006  (power control)
     *   0xC4 PMUTCLKDIVREG = 0x0000  (TCLK divider)
     *   0xC6 PMUINTREG2    = 0x0000  (interrupt status 2)
     *   0xC8 PMUWAITREG    = 0x0148  (wait control)
     *   0xCC PMUDIVREG     = 0x0002  (clock divider)
     */
    s->pmuintreg     = 0x0400;  /* WORKAROUND: warm-state seed */
    s->pmucntreg     = 0x1006;  /* WORKAROUND: warm-state seed */
    s->pmutclkdivreg = 0x0000;
    s->pmuintreg2    = 0x0000;
    s->pmuwaitreg    = 0x0148;  /* WORKAROUND: warm-state seed */
    s->pmudivreg     = 0x0002;  /* WORKAROUND: warm-state seed */
}

uint32_t pmu_read(pmu_state_t *s, uint32_t offset, unsigned size)
{
    (void)size;
    switch (offset) {
    case PMU_PMUINTREG:     return s->pmuintreg;
    case PMU_PMUCNTREG:     return s->pmucntreg;
    case PMU_PMUTCLKDIVREG: return s->pmutclkdivreg;
    case PMU_PMUINTREG2:    return s->pmuintreg2;
    case PMU_PMUWAITREG:    return s->pmuwaitreg;
    case PMU_PMUDIVREG:     return s->pmudivreg;
    default:
        fprintf(stderr, "[PMU] Unhandled read offset 0x%02X\n", offset);
        return 0;
    }
}

void pmu_write(pmu_state_t *s, uint32_t offset, unsigned size, uint32_t val)
{
    (void)size;
    switch (offset) {
    case PMU_PMUINTREG:     s->pmuintreg     = (uint16_t)val; break;
    case PMU_PMUCNTREG:     s->pmucntreg     = (uint16_t)val; break;
    case PMU_PMUTCLKDIVREG: s->pmutclkdivreg = (uint16_t)val; break;
    case PMU_PMUINTREG2:    s->pmuintreg2    = (uint16_t)val; break;
    case PMU_PMUWAITREG:    s->pmuwaitreg    = (uint16_t)val; break;
    case PMU_PMUDIVREG:     s->pmudivreg     = (uint16_t)val; break;
    default:
        fprintf(stderr, "[PMU] Unhandled write offset 0x%02X = 0x%04X\n",
                offset, val);
        break;
    }
}
