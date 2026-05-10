#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void stowaway_configure(bool enabled);
bool stowaway_ns16550_claims(const char *name);
void stowaway_uart_reset(void);
bool stowaway_uart_rx_available(void);
size_t stowaway_uart_rx_count(void);
int  stowaway_uart_rx_pop(void);
void stowaway_uart_rx_clear(void);
void stowaway_uart_tx_byte(uint8_t byte);
void stowaway_uart_note_port_config(void);
void stowaway_uart_note_modem_wait(void);
void stowaway_uart_note_modem_control(bool dtr_asserted, bool rts_asserted);
bool stowaway_uart_take_modem_wait_request(void);
bool stowaway_uart_take_modem_delta(void);
bool stowaway_queue_key(unsigned scancode, bool release);

/* Implemented in be300_devices.c. Synthesizes a CommMode modem event so the
 * WinCE OAL dispatches GIRQ0-4-4, waking serial.dll's IST to poll the UART.
 * The ns16550 IRQ goes to GIU pin 0 alone, but on real BE-300 the OAL only
 * dispatches GIU 0 by reading AA000004/AA008004 latches — without those
 * bits set, no SYSINTR fires and bytes sit in the UART RX queue. */
void be300_stowaway_signal_uart_irq(int pending);
