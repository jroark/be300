#include <stdio.h>
#include <string.h>
#include "siu.h"

void siu_init(siu_state_t *s)
{
    memset(s, 0, sizeof(*s));
    /* Transmitter always ready; receiver empty initially */
    s->lsr     = SIU_LSR_THRE | SIU_LSR_TEMT;
    s->iir     = 0x01;   /* No interrupt pending */
    s->divisor = 0x0001;
}

uint32_t siu_read(siu_state_t *s, uint32_t offset, unsigned size)
{
    (void)size;
    bool dlab = (s->lcr & SIU_LCR_DLAB) != 0;

    switch (offset) {
    case SIU_RBR:   /* 0x00 — RBR (read) or DLL */
        if (dlab)
            return s->divisor & 0xFF;
        if (s->rx_ready) {
            s->rx_ready = false;
            s->lsr &= ~SIU_LSR_DR;
            return s->rx_byte;
        }
        return 0;

    case SIU_IER:   /* 0x02 — IER or DLM */
        if (dlab)
            return (s->divisor >> 8) & 0xFF;
        return s->ier;

    case SIU_IIR:   /* 0x04 — IIR (read) */
        return s->iir;

    case SIU_LCR:   return s->lcr;
    case SIU_MCR:   return s->mcr;
    case SIU_LSR:   return s->lsr;
    case SIU_MSR:   return s->msr | 0xB0;  /* CTS/DSR/RI/DCD asserted */
    case SIU_SCR:   return s->scr;

    default:
        fprintf(stderr, "[SIU] Unhandled read offset 0x%02X\n", offset);
        return 0;
    }
}

void siu_write(siu_state_t *s, uint32_t offset, unsigned size, uint32_t val)
{
    (void)size;
    bool dlab = (s->lcr & SIU_LCR_DLAB) != 0;

    switch (offset) {
    case SIU_THR:   /* 0x00 — THR (write) or DLL */
        if (dlab) {
            s->divisor = (s->divisor & 0xFF00) | (val & 0xFF);
        } else {
            /* Output the character to the host terminal */
            putchar((int)(val & 0xFF));
            fflush(stdout);
        }
        break;

    case SIU_IER:   /* 0x02 — IER or DLM */
        if (dlab)
            s->divisor = (s->divisor & 0x00FF) | ((val & 0xFF) << 8);
        else
            s->ier = (uint8_t)val;
        break;

    case SIU_FCR:   s->fcr = (uint8_t)val; break;   /* 0x04 — FCR (write) */
    case SIU_LCR:   s->lcr = (uint8_t)val; break;
    case SIU_MCR:   s->mcr = (uint8_t)val; break;
    case SIU_SCR:   s->scr = (uint8_t)val; break;
    case SIU_LSR:   /* read-only, ignore */ break;
    case SIU_MSR:   /* read-only, ignore */ break;

    default:
        fprintf(stderr, "[SIU] Unhandled write offset 0x%02X = 0x%02X\n",
                offset, val);
        break;
    }
}
