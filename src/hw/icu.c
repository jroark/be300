#include <stdio.h>
#include "icu.h"

void icu_init(icu_state_t *s)
{
    /* All sources masked at reset; no pending interrupts */
    s->sysint1  = 0x0000;
    s->msysint1 = 0xFFFF;
    s->sysint2  = 0x0000;
    s->msysint2 = 0xFFFF;
    s->piuint   = 0; s->mpiuint  = 0xFFFF;
    s->kiuint   = 0; s->mkiuint  = 0xFFFF;
    s->giuintl  = 0; s->mgiuintl = 0xFFFF;
    s->giuinth  = 0; s->mgiuinth = 0xFFFF;
    s->dsiuint  = 0; s->mdsiuint = 0xFFFF;
    s->firint   = 0; s->mfirint  = 0xFFFF;
    s->softint  = 0;
    s->nmi      = 0;
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
    case ICU_SYSINT2REG:  return s->sysint2;
    case ICU_GIUINTHREG:  return s->giuinth;
    case ICU_FIRINTREG:   return s->firint;
    case ICU_MSYSINT2REG: return s->msysint2;
    case ICU_MGIUINTHREG: return s->mgiuinth;
    case ICU_MFIRINTREG:  return s->mfirint;
    default:
        fprintf(stderr, "[ICU] Unhandled read offset 0x%02X\n", offset);
        return 0;
    }
}

void icu_write(icu_state_t *s, uint32_t offset, unsigned size, uint32_t val)
{
    (void)size;
    switch (offset) {
    /* Status registers: writing 0 clears the bit (W1C semantics) */
    case ICU_SYSINT1REG:  s->sysint1  &= (uint16_t)val; break;
    case ICU_PIUINTREG:   s->piuint   &= (uint16_t)val; break;
    case ICU_KIUINTREG:   s->kiuint   &= (uint16_t)val; break;
    case ICU_GIUINTLREG:  s->giuintl  &= (uint16_t)val; break;
    case ICU_DSIUINTREG:  s->dsiuint  &= (uint16_t)val; break;
    case ICU_SYSINT2REG:  s->sysint2  &= (uint16_t)val; break;
    case ICU_GIUINTHREG:  s->giuinth  &= (uint16_t)val; break;
    case ICU_FIRINTREG:   s->firint   &= (uint16_t)val; break;
    /* Mask registers: write directly */
    case ICU_MSYSINT1REG: s->msysint1 = (uint16_t)val; break;
    case ICU_MPIUINTREG:  s->mpiuint  = (uint16_t)val; break;
    case ICU_MKIUINTREG:  s->mkiuint  = (uint16_t)val; break;
    case ICU_MGIUINTLREG: s->mgiuintl = (uint16_t)val; break;
    case ICU_MDSIUINTREG: s->mdsiuint = (uint16_t)val; break;
    case ICU_MSYSINT2REG: s->msysint2 = (uint16_t)val; break;
    case ICU_MGIUINTHREG: s->mgiuinth = (uint16_t)val; break;
    case ICU_MFIRINTREG:  s->mfirint  = (uint16_t)val; break;
    case ICU_SOFTINTREG:  s->softint  = (uint16_t)val; break;
    case ICU_NMIREG:      s->nmi      = (uint16_t)val; break;
    case ICU_ATCINT:
    case ICU_MATCINT:
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
    /* Interrupt pending if any unmasked source is active */
    if (s->sysint1 & ~s->msysint1) return true;
    if (s->sysint2 & ~s->msysint2) return true;
    return false;
}
