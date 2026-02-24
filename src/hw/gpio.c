#include <stdio.h>
#include "gpio.h"

void gpio_init(gpio_state_t *s)
{
    s->iosel_lo  = 0x0000;
    s->iosel_hi  = 0x0000;
    s->data_lo   = 0x0000;
    s->data_hi   = 0x0000;
    s->intstat_lo = 0x0000;
    s->intstat_hi = 0x0000;
    s->inten_lo  = 0x0000;
    s->inten_hi  = 0x0000;
}

uint32_t gpio_read(gpio_state_t *s, uint32_t offset, unsigned size)
{
    (void)size;
    switch (offset) {
    case GPIO_GIUIOSELL:   return s->iosel_lo;
    case GPIO_GIUIOSELH:   return s->iosel_hi;
    case GPIO_GIUPIODL:    return s->data_lo;
    case GPIO_GIUPIODH:    return s->data_hi;
    case GPIO_GIUINTSTATL: return s->intstat_lo;
    case GPIO_GIUINTSTATH: return s->intstat_hi;
    case GPIO_GIUINTENL:   return s->inten_lo;
    case GPIO_GIUINTENH:   return s->inten_hi;
    default:
        fprintf(stderr, "[GPIO] Unhandled read offset 0x%02X\n", offset);
        return 0;
    }
}

void gpio_write(gpio_state_t *s, uint32_t offset, unsigned size, uint32_t val)
{
    (void)size;
    switch (offset) {
    case GPIO_GIUIOSELL:   s->iosel_lo  = (uint16_t)val; break;
    case GPIO_GIUIOSELH:   s->iosel_hi  = (uint16_t)val; break;
    case GPIO_GIUPIODL:    s->data_lo   = (uint16_t)val; break;
    case GPIO_GIUPIODH:    s->data_hi   = (uint16_t)val; break;
    case GPIO_GIUINTSTATL: s->intstat_lo = (uint16_t)val; break;
    case GPIO_GIUINTSTATH: s->intstat_hi = (uint16_t)val; break;
    case GPIO_GIUINTENL:   s->inten_lo  = (uint16_t)val; break;
    case GPIO_GIUINTENH:   s->inten_hi  = (uint16_t)val; break;
    default:
        fprintf(stderr, "[GPIO] Unhandled write offset 0x%02X = 0x%04X\n",
                offset, val);
        break;
    }
}
