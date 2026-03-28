#include <stdio.h>
#include "pmu.h"

void pmu_init(pmu_state_t *s)
{
    /*
     * Stable warm-state seed from the hardware surveys:
     * 0x0F0000C0: 10060400 00000000 00000148 00000002
     *
     * In little-endian halfwords this corresponds to:
     *   0x0f0000c0 pmuintreg     = 0x0400
     *   0x0f0000c2 pmucntreg     = 0x1006
     *   0x0f0000c4 pmutclkdivreg = 0x0000
     *   0x0f0000c6 pmuintreg2    = 0x0000
     *   0x0f0000c8 pmuwaitreg    = 0x0148
     *   0x0f0000cc pmudivreg     = 0x0002
     */
    s->pmuintreg     = 0x0400;
    s->pmucntreg     = 0x1006;
    s->pmutclkdivreg = 0x0000;
    s->pmuintreg2    = 0x0000;
    s->pmuwaitreg    = 0x0148;
    s->pmudivreg     = 0x0002;
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
