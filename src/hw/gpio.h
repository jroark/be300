#pragma once
#include <stdint.h>

/*
 * GPIO — General-Purpose I/O (VR4131 hardware manual §14)
 *
 * 16-bit registers. Stub: accept writes, return last written value.
 * The BE-300 uses GPIO for buttons, battery status, etc.
 */

/* Register offsets from GPIO base (0x0F000140) */
#define GPIO_GIUIOSELL   0x00u   /* I/O select low  */
#define GPIO_GIUIOSELH   0x02u   /* I/O select high */
#define GPIO_GIUPIODL    0x04u   /* Port I/O data low  */
#define GPIO_GIUPIODH    0x06u   /* Port I/O data high */
#define GPIO_GIUINTSTATL 0x08u   /* Interrupt status low  */
#define GPIO_GIUINTSTATH 0x0Au   /* Interrupt status high */
#define GPIO_GIUINTENL   0x0Cu   /* Interrupt enable low  */
#define GPIO_GIUINTENH   0x0Eu   /* Interrupt enable high */
#define GPIO_GIUINTTYPL  0x10u   /* Interrupt type low    */
#define GPIO_GIUINTTYPH  0x12u   /* Interrupt type high   */
#define GPIO_GIUINTALSELL 0x14u  /* Active level select low */
#define GPIO_GIUINTALSELH 0x16u  /* Active level select high */
#define GPIO_GIUINTHTSELL 0x18u  /* Hold trigger select low */
#define GPIO_GIUINTHTSELH 0x1Au  /* Hold trigger select high */

typedef struct {
    uint16_t iosel_lo, iosel_hi;
    uint16_t data_lo,  data_hi;
    uint16_t intstat_lo, intstat_hi;
    uint16_t inten_lo,   inten_hi;
    uint16_t inttype_lo, inttype_hi;
    uint16_t intalsel_lo, intalsel_hi;
    uint16_t inthtsel_lo, inthtsel_hi;
} gpio_state_t;

void     gpio_init (gpio_state_t *s);
uint32_t gpio_read (gpio_state_t *s, uint32_t offset, unsigned size);
void     gpio_write(gpio_state_t *s, uint32_t offset, unsigned size, uint32_t val);
