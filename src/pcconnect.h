#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void pcconnect_configure(bool enabled);
void pcconnect_set_rx_ready_callback(void (*cb)(void *opaque), void *opaque);
void pcconnect_set_cable_connected(bool connected);
bool pcconnect_ns16550_claims(const char *name);
bool pcconnect_cable_connected(void);
bool pcconnect_guest_uart_ready(void);
bool pcconnect_trace_enabled(void);
void pcconnect_signal_uart_irq(void);
void pcconnect_note_uart_config(const char *name, uint8_t lcr, uint8_t mcr,
    uint8_t ier, int divisor, int dlab);
void pcconnect_trace_uart_access(const char *name, bool claimed,
    bool write, unsigned reg, uint8_t value, uint32_t pc,
    uint8_t ier, uint8_t iir, uint8_t lcr, uint8_t mcr,
    uint8_t lsr, uint8_t msr, int dlab, int divisor);
bool pcconnect_uart_rx_available(void);
size_t pcconnect_uart_rx_count(void);
int  pcconnect_uart_rx_pop(void);
void pcconnect_uart_rx_clear(void);
void pcconnect_uart_tx_byte(uint8_t byte);
uint32_t pcconnect_uart_baud(void);
