/*
 *  src/hw/cf_internal.h — declarations shared between cf.c and the files
 *  split out of it. Not included by anything outside src/hw/cf*.c.
 */
#pragma once

#include <stdint.h>

#include "cf.h"

/* NE2000 register macros used by both the attach helper in cf.c and
 * the register-bank machinery in cf_ne2000.c. */
#define NE_CR_STP 0x01u
#define NE_CR_STA 0x02u
#define NE_CR_TXP 0x04u
#define NE_CR_RD_MASK 0x38u
#define NE_CR_RD0 0x08u
#define NE_CR_RD1 0x10u
#define NE_CR_RD2 0x20u
#define NE_CR_PS_MASK 0xC0u
#define NE_ISR_PRX 0x01u
#define NE_ISR_PTX 0x02u
#define NE_ISR_RXE 0x04u
#define NE_ISR_TXE 0x08u
#define NE_ISR_OVW 0x10u
#define NE_ISR_CNT 0x20u
#define NE_ISR_RDC 0x40u
#define NE_ISR_RST 0x80u
#define NE_RSR_PRX 0x01u
#define NE_RSR_PHY 0x20u
#define NE_TSR_PTX 0x01u
#define NE_DCR_WTS 0x01u
#define NE_RCR_SEP 0x01u
#define NE_RCR_AB  0x04u
#define NE_RCR_AM  0x08u
#define NE_RCR_PRO 0x10u
#define NE_RCR_MON 0x20u
#define NE_RESET_PORT 0x1Fu

/* NE2000 helpers exported across the split. Definitions live in
 * src/hw/cf_ne2000.c; cf.c calls these from cf_attach_ne2000 and
 * cf_window_read/write. */
void     ne2000_seed_prom(cf_state_t *s);
void     ne2000_reset(cf_state_t *s);
uint64_t ne2000_window_read(cf_state_t *s, uint32_t offset, unsigned size);
void     ne2000_window_write(cf_state_t *s, uint32_t offset, unsigned size,
    uint64_t value);
