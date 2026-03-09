#include <stdio.h>
#include "bcu.h"

void bcu_init(bcu_state_t *s)
{
    s->bcucntreg1  = 0x0000;
    s->bcucntreg2  = 0x0000;
    s->romsizereg  = 0x0003;   /* 32-bit ROM, 32 MB */
    s->romspeedreg = 0x00FF;
    s->io0sizereg  = 0x0000;
    s->io0speedreg = 0x00FF;
    s->io1speedreg = 0x00FF;
    s->clkspeedreg = 0x0000;
}

uint32_t bcu_read(bcu_state_t *s, uint32_t offset, unsigned size)
{
    (void)size;
    switch (offset) {
    case BCU_BCUCNTREG1:  return s->bcucntreg1;
    case BCU_BCUCNTREG2:  return s->bcucntreg2;
    case BCU_ROMSIZEREG:  return s->romsizereg;
    case BCU_ROMSPEEDREG: return s->romspeedreg;
    case BCU_IO0SIZEREG:  return s->io0sizereg;
    case BCU_IO0SPEEDREG: return s->io0speedreg;
    case BCU_IO1SPEEDREG: return s->io1speedreg;
    case BCU_REVIDREG:    return BCU_REVID_VR4131;
    case BCU_CLKSPEEDREG: return s->clkspeedreg;
    default:
        /* WinCE probes additional BCU registers (SDRAM config, etc.)
         * that we don't model — return 0 silently. */
        return 0;
    }
}

void bcu_write(bcu_state_t *s, uint32_t offset, unsigned size, uint32_t val)
{
    (void)size;
    switch (offset) {
    case BCU_BCUCNTREG1:  s->bcucntreg1  = (uint16_t)val; break;
    case BCU_BCUCNTREG2:  s->bcucntreg2  = (uint16_t)val; break;
    case BCU_ROMSIZEREG:  s->romsizereg  = (uint16_t)val; break;
    case BCU_ROMSPEEDREG: s->romspeedreg = (uint16_t)val; break;
    case BCU_IO0SIZEREG:  s->io0sizereg  = (uint16_t)val; break;
    case BCU_IO0SPEEDREG: s->io0speedreg = (uint16_t)val; break;
    case BCU_IO1SPEEDREG: s->io1speedreg = (uint16_t)val; break;
    case BCU_REVIDREG:    /* read-only */ break;
    case BCU_CLKSPEEDREG: s->clkspeedreg = (uint16_t)val; break;
    default:
        /* Ignore writes to unmodeled BCU registers */
        break;
    }
}
