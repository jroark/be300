#include <stdio.h>
#include "icu.h"

void icu_init(icu_state_t *s)
{
    /* Match vr41xx Linux init: all SYSINT lines disabled at reset. */
    s->sysint1  = 0x0000;
    s->msysint1 = 0x0000;
    s->sysint2  = 0x0000;
    s->msysint2 = 0x0000;
    s->piuint   = 0; s->mpiuint  = 0x0000;
    s->kiuint   = 0; s->mkiuint  = 0x0000;
    s->giuintl  = 0; s->mgiuintl = 0xFFFF;
    s->giuinth  = 0; s->mgiuinth = 0xFFFF;
    s->dsiuint  = 0; s->mdsiuint = 0x0000;
    s->firint   = 0; s->mfirint  = 0x0000;
    s->softint  = 0;
    s->nmi      = 0;
    s->intassign2 = 0;
    s->intassign3 = 0;
    s->pciint   = 0; s->mpciint = 0x0000;
    s->scuint   = 0; s->mscuint = 0x0000;
    s->csiint   = 0; s->mcsiint = 0x0000;
    s->bcuint   = 0; s->mbcuint = 0x0000;
}

uint32_t icu_read(icu_state_t *s, uint32_t offset, unsigned size)
{
    (void)size;
    switch (offset) {
    case ICU_SYSINT1REG:  return s->sysint1;
    case ICU_PIUINTREG:   return s->piuint;
    case ICU_ATCINT:      return 0;
    case ICU_KIUINTREG:   return s->kiuint;
    case ICU_GIUINTLREG:  return s->giuintl;
    case ICU_DSIUINTREG:  return s->dsiuint;
    case ICU_MSYSINT1REG: return s->msysint1;
    case ICU_MPIUINTREG:  return s->mpiuint;
    case ICU_MATCINT:     return 0xFFFF;
    case ICU_MKIUINTREG:  return s->mkiuint;
    case ICU_MGIUINTLREG: return s->mgiuintl;
    case ICU_MDSIUINTREG: return s->mdsiuint;
    case ICU_NMIREG:      return s->nmi;
    case ICU_SOFTINTREG:  return s->softint;
    case ICU_INTASSIGN2:  return s->intassign2;
    case ICU_INTASSIGN3:  return s->intassign3;
    case ICU_SYSINT2REG:  return s->sysint2;
    case ICU_GIUINTHREG:  return s->giuinth;
    case ICU_FIRINTREG:   return s->firint;
    case ICU_MSYSINT2REG: return s->msysint2;
    case ICU_MGIUINTHREG: return s->mgiuinth;
    case ICU_MFIRINTREG:  return s->mfirint;
    case ICU_PCIINTREG:   return s->pciint;
    case ICU_SCUINTREG:   return s->scuint;
    case ICU_CSIINTREG:   return s->csiint;
    case ICU_MPCIINTREG:  return s->mpciint;
    case ICU_MSCUINTREG:  return s->mscuint;
    case ICU_MCSIINTREG:  return s->mcsiint;
    case ICU_BCUINTREG:   return s->bcuint;
    case ICU_MBCUINTREG:  return s->mbcuint;
    case 0x3Cu:           return 0; /* RFU on VR4131 window */
    default:
        fprintf(stderr, "[ICU] Unhandled read offset 0x%02X\n", offset);
        return 0;
    }
}

void icu_write(icu_state_t *s, uint32_t offset, unsigned size, uint32_t val)
{
    (void)size;
    switch (offset) {
    /* Status registers: write-one-to-clear. */
    case ICU_SYSINT1REG:  s->sysint1  &= (uint16_t)~val; break;
    case ICU_PIUINTREG:   s->piuint   &= (uint16_t)~val; break;
    case ICU_KIUINTREG:   s->kiuint   &= (uint16_t)~val; break;
    case ICU_GIUINTLREG:  s->giuintl  &= (uint16_t)~val; break;
    case ICU_DSIUINTREG:  s->dsiuint  &= (uint16_t)~val; break;
    case ICU_SYSINT2REG:  s->sysint2  &= (uint16_t)~val; break;
    case ICU_GIUINTHREG:  s->giuinth  &= (uint16_t)~val; break;
    case ICU_FIRINTREG:   s->firint   &= (uint16_t)~val; break;
    case ICU_PCIINTREG:   s->pciint   &= (uint16_t)~val; break;
    case ICU_SCUINTREG:   s->scuint   &= (uint16_t)~val; break;
    case ICU_CSIINTREG:   s->csiint   &= (uint16_t)~val; break;
    case ICU_BCUINTREG:   s->bcuint   &= (uint16_t)~val; break;
    /* Mask registers: write directly */
    case ICU_MSYSINT1REG:
        /* Let the kernel control MSYSINT1 fully.  The forced ETIME
         * bit prevented WinCE from masking the timer interrupt,
         * causing its interrupt dispatch loop to spin forever. */
        s->msysint1 = (uint16_t)val;
        break;
    case ICU_MPIUINTREG:  s->mpiuint  = (uint16_t)val; break;
    case ICU_MKIUINTREG:  s->mkiuint  = (uint16_t)val; break;
    case ICU_MGIUINTLREG: s->mgiuintl = (uint16_t)val; break;
    case ICU_MDSIUINTREG: s->mdsiuint = (uint16_t)val; break;
    case ICU_MSYSINT2REG: s->msysint2 = (uint16_t)val; break;
    case ICU_MGIUINTHREG: s->mgiuinth = (uint16_t)val; break;
    case ICU_MFIRINTREG:  s->mfirint  = (uint16_t)val; break;
    case ICU_MPCIINTREG:  s->mpciint  = (uint16_t)val; break;
    case ICU_MSCUINTREG:  s->mscuint  = (uint16_t)val; break;
    case ICU_MCSIINTREG:  s->mcsiint  = (uint16_t)val; break;
    case ICU_MBCUINTREG:  s->mbcuint  = (uint16_t)val; break;
    case ICU_SOFTINTREG:  s->softint  = (uint16_t)val; break;
    case ICU_NMIREG:      s->nmi      = (uint16_t)val; break;
    case ICU_INTASSIGN2:  s->intassign2 = (uint16_t)val; break;
    case ICU_INTASSIGN3:  s->intassign3 = (uint16_t)val; break;
    case ICU_ATCINT:
    case ICU_MATCINT:
    case 0x3Cu:
        break;   /* stub */
    default:
        fprintf(stderr, "[ICU] Unhandled write offset 0x%02X = 0x%04X\n",
                offset, val);
        break;
    }
}

void icu_assert(icu_state_t *s, uint16_t src_bit)
{
    s->sysint1 |= src_bit;
}

void icu_deassert(icu_state_t *s, uint16_t src_bit)
{
    s->sysint1 &= ~src_bit;
}

bool icu_pending(const icu_state_t *s)
{
    /* Interrupt pending if any enabled source is active */
    if (s->sysint1 & s->msysint1) return true;
    if (s->sysint2 & s->msysint2) return true;
    return false;
}
