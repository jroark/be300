#include <stdio.h>
#include "pmu.h"

void pmu_init(pmu_state_t *s)
{
    /*
     * Cold boot: PMU registers at hardware-reset defaults (zero).
     * The kernel programs them during initialization.
     */
    s->pmuintreg     = 0;
    s->pmucntreg     = 0;
    s->pmutclkdivreg = 0;
    s->pmuintreg2    = 0;
    s->pmuwaitreg    = 0;
    s->pmudivreg     = 0;
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
