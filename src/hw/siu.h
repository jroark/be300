#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * SIU — Serial Interface Unit (VR4131 hardware manual §20)
 *
 * NS16550-compatible UART.  Registers are 16-bit wide and located at
 * 2-byte-aligned offsets from SIU base (0x0F000800).
 *
 * This implementation outputs TX characters to stdout and
 * optionally reads RX characters from stdin.
 */

/* Register offsets from SIU base (0x0F000800) */
#define SIU_RBR   0x00u   /* Receive Buffer Register  (read,  DLAB=0) */
#define SIU_THR   0x00u   /* Transmit Holding Register(write, DLAB=0) */
#define SIU_DLL   0x00u   /* Divisor Latch Low        (DLAB=1)        */
#define SIU_IER   0x02u   /* Interrupt Enable Register       (DLAB=0) */
#define SIU_DLM   0x02u   /* Divisor Latch High              (DLAB=1) */
#define SIU_IIR   0x04u   /* Interrupt ID Register    (read)          */
#define SIU_FCR   0x04u   /* FIFO Control Register    (write)         */
#define SIU_LCR   0x06u   /* Line Control Register                    */
#define SIU_MCR   0x08u   /* Modem Control Register                   */
#define SIU_LSR   0x0Au   /* Line Status Register     (read-only)     */
#define SIU_MSR   0x0Cu   /* Modem Status Register    (read-only)     */
#define SIU_SCR   0x0Eu   /* Scratch Register                         */

/* LSR bits */
#define SIU_LSR_DR   (1u << 0)   /* Data Ready                 */
#define SIU_LSR_THRE (1u << 5)   /* THR Empty (ready to send)  */
#define SIU_LSR_TEMT (1u << 6)   /* Transmitter Empty          */

/* LCR bits */
#define SIU_LCR_DLAB (1u << 7)   /* Divisor Latch Access Bit   */

typedef struct {
    uint8_t  ier;
    uint8_t  iir;
    uint8_t  fcr;
    uint8_t  lcr;
    uint8_t  mcr;
    uint8_t  lsr;
    uint8_t  msr;
    uint8_t  scr;
    uint16_t divisor;     /* baud rate divisor (DLL | DLM<<8) */

    /* Simple 1-byte RX buffer */
    bool    rx_ready;
    uint8_t rx_byte;
} siu_state_t;

void     siu_init (siu_state_t *s);
uint32_t siu_read (siu_state_t *s, uint32_t offset, unsigned size);
void     siu_write(siu_state_t *s, uint32_t offset, unsigned size, uint32_t val);
