/*
 *  PCMCIA jacket: Windows CE's socket 1 on the BE-300.
 *
 *  A PC Card jacket in the CF slot puts its card on a second socket
 *  controller, not the CF socket at 0x0A00A000. Read on a real BE-300
 *  (glaze-os docs/hardware/PCCARDS.md, "Socket 1"), with pcmcia.dll's
 *  register table at 0x0198AA64 and window table at 0x0198B9AC:
 *
 *  - card detect: 0x0A001008 bit 3 and 0x0A00104C bit 6 read 1 while the
 *    jacket's socket is empty (0x4/0xC and 0x0C/0x48);
 *  - bus power: 0x0A001B00 bit 0 (with GPIO 5); 0x0A001B50 bit 3 then
 *    reports power good;
 *  - the controller at 0x0B000000 reads 0 while its bus is off; powered,
 *    0x0B000100 and 0x0B000010 identify it (0x641, 0x7402), 0x0B000108-118
 *    and 0x0B000150 are pin edge detectors (bit 6 the level), and
 *    0x0B000500 is a socket block laid out like 0x0A00A000 (CTRL, RESET,
 *    STATUS at +0, +4, +8; A020/A024's counterparts at +0x20/+0x24);
 *  - card windows: I/O at 0x0B200000 + port, attribute memory at
 *    0x0B600000, decoded once the card is powered and out of reset.
 *
 *  Not modelled: socket 1's interrupt (unknown; Glaze polls the card).
 */
#ifndef BE300_HW_JACKET_H
#define BE300_HW_JACKET_H

#include <stdbool.h>
#include <stdint.h>
#include "cf.h"

#define JACKET_REG_SIZE     0x600u      /* 0x0B000000-0x0B0005FF */
#define JACKET_IO_BASE      0x0B200000u
#define JACKET_IO_SIZE      0x00200000u
#define JACKET_ATTR_OFFSET  0x00200000u /* 0x0B600000 within the 0x0B400000 window */

typedef struct jacket {
    cf_state_t *card;   /* the jacket's card (cf_present: one is inserted) */
    cf_state_t *cf;     /* the CF socket's state, whose companion page holds bus power */
    uint32_t regs[JACKET_REG_SIZE / 4];
    bool was_up;
} jacket_t;

void     jacket_init(jacket_t *j, cf_state_t *card, cf_state_t *cf);
bool     jacket_bus_on(const jacket_t *j);
/* Powered, supplied (CTRL 0xB) and out of reset: the card's windows decode. */
bool     jacket_card_up(jacket_t *j);
uint64_t jacket_reg_read(jacket_t *j, uint32_t offset, unsigned size);
void     jacket_reg_write(jacket_t *j, uint32_t offset, unsigned size, uint64_t value);
uint64_t jacket_io_read(jacket_t *j, uint32_t offset, unsigned size);
void     jacket_io_write(jacket_t *j, uint32_t offset, unsigned size, uint64_t value);
uint64_t jacket_attr_read(jacket_t *j, uint32_t offset, unsigned size);
void     jacket_attr_write(jacket_t *j, uint32_t offset, unsigned size, uint64_t value);
/* The CF socket's companion page bytes that report the jacket: detect at
 * 0x008 and 0x04C, power good at 0xB50. */
uint8_t  jacket_companion_byte(const jacket_t *j, uint32_t offset, uint8_t byte);

#endif
