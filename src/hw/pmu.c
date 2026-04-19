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
    s->pmucnt2reg    = 0;
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
    case PMU_PMUINTREG:     s->pmuintreg     = (uint16_t)val; break;
    case PMU_PMUCNTREG:     s->pmucntreg     = (uint16_t)val; break;
    case PMU_PMUTCLKDIVREG: s->pmutclkdivreg = (uint16_t)val; break;
    case PMU_PMUCNT2REG:    s->pmucnt2reg    = (uint16_t)val; break;
    case PMU_PMUWAITREG:    s->pmuwaitreg    = (uint16_t)val; break;
    case PMU_PMUDIVREG:     s->pmudivreg     = (uint16_t)val; break;
    default:
        break;
    }
}
