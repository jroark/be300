#pragma once

#include <stdint.h>
#include "machine.h"

/*
 * Internal I/O register sub-regions (offsets from PA_IO_BASE = 0x0F000000).
 * Addresses sourced from VR4131 hardware manual chapter references.
 */

/* BCU — Bus Control Unit §7 */
#define IO_BCU_BASE   0x0000u
#define IO_BCU_SIZE   0x0020u

/* PMU — Power Management Unit §12 */
#define IO_PMU_BASE   0x0030u
#define IO_PMU_SIZE   0x0010u

/* CMU — Clock Mask Unit §10 */
#define IO_CMU_BASE   0x0060u
#define IO_CMU_SIZE   0x0004u

/* ICU — Interrupt Control Unit §11 */
#define IO_ICU_BASE   0x0080u
#define IO_ICU_SIZE   0x0040u

/* RTC — Real-Time Clock §13 */
#define IO_RTC_BASE   0x0100u
#define IO_RTC_SIZE   0x0040u

/* GPIO — General-Purpose I/O §14 */
#define IO_GPIO_BASE  0x0140u
#define IO_GPIO_SIZE  0x0020u

/* SIU — Serial Interface Unit §20 (NS16550-compatible) */
#define IO_SIU_BASE   0x0800u
#define IO_SIU_SIZE   0x0010u

/*
 * VRC4173 companion chip — external bus CS3.
 * Physical address: 0x0A000000 (CS3 base for Casio BE-300).
 * Provides USB host, KbC, GPIO, and an NS16550-compatible UART.
 *
 * UART register base: PA 0x0A008680
 * Registers are 4-byte spaced (not 1-byte as on a typical 16550):
 *   +0x00 (PA 0x0A008680) = THR (write) / RBR (read)
 *   +0x14 (PA 0x0A008694) = LSR
 */
#define PA_VRC4173_BASE  UINT32_C(0x0A000000)
#define PA_VRC4173_SIZE  (16u * 1024u * 1024u)   /* 16 MB CS3 window */
#define VRC4173_UART_BASE  0x8680u   /* offset from PA_VRC4173_BASE */
#define VRC4173_UART_THR   0x8680u   /* transmit holding register */
#define VRC4173_UART_LSR   0x8694u   /* line status register (+0x14) */

#define PA_FRAMEBUFFER_BASE UINT32_C(0x0A200000)
#define PA_FRAMEBUFFER_SIZE (2u * 1024u * 1024u)   /* 2 MB VRAM */

void bus_init(machine_t *m);
