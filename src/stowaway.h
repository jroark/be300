#pragma once

#include <stdbool.h>
#include <stdint.h>

void stowaway_configure(bool enabled);
bool stowaway_ns16550_claims(const char *name);
bool stowaway_uart_rx_available(void);
int  stowaway_uart_rx_pop(void);
void stowaway_uart_tx_byte(uint8_t byte);
bool stowaway_queue_key(unsigned scancode, bool release);
