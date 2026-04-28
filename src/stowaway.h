#pragma once

#include <stdbool.h>
#include <stdint.h>

void stowaway_configure(bool enabled);
bool stowaway_ns16550_claims(const char *name);
void stowaway_uart_reset(void);
bool stowaway_uart_rx_available(void);
int  stowaway_uart_rx_pop(void);
void stowaway_uart_tx_byte(uint8_t byte);
void stowaway_uart_note_port_config(void);
void stowaway_uart_note_modem_wait(void);
void stowaway_uart_note_modem_control(bool dtr_asserted, bool rts_asserted);
bool stowaway_uart_take_modem_wait_request(void);
bool stowaway_queue_key(unsigned scancode, bool release);
