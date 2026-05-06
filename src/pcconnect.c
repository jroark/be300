#include <stdio.h>

#include "pcconnect.h"
#include "pcconnect_bridge.h"

static void (*g_rx_ready_cb)(void *opaque);
static void *g_rx_ready_opaque;

void pcconnect_set_rx_ready_callback(void (*cb)(void *opaque), void *opaque)
{
    g_rx_ready_cb = cb;
    g_rx_ready_opaque = opaque;
    pcconnect_bridge_set_rx_ready_callback(cb, opaque);
}

void pcconnect_set_cable_connected(bool connected)
{
    pcconnect_bridge_set_cable_connected(connected);
}

bool pcconnect_ns16550_claims(const char *name)
{
    return pcconnect_bridge_ns16550_claims(name);
}

bool pcconnect_cable_connected(void)
{
    return pcconnect_bridge_cable_connected();
}

void pcconnect_signal_uart_irq(void)
{
    if (!pcconnect_cable_connected())
        return;
    if (g_rx_ready_cb)
        g_rx_ready_cb(g_rx_ready_opaque);
}

bool pcconnect_trace_enabled(void)
{
    return pcconnect_bridge_trace_enabled();
}

void pcconnect_note_uart_config(const char *name, uint8_t lcr, uint8_t mcr,
    uint8_t ier, int divisor, int dlab)
{
    pcconnect_bridge_note_uart_config(name, lcr, mcr, ier, divisor, dlab);
}

void pcconnect_trace_uart_access(const char *name, bool claimed,
    bool write, unsigned reg, uint8_t value, uint32_t pc,
    uint8_t ier, uint8_t iir, uint8_t lcr, uint8_t mcr,
    uint8_t lsr, uint8_t msr, int dlab, int divisor)
{
    if (!pcconnect_trace_enabled())
        return;

    fprintf(stderr,
        "[PC_CONNECT_UART] %s %s reg=%u val=0x%02x pc=0x%08x "
        "claimed=%u ier=0x%02x iir=0x%02x lcr=0x%02x mcr=0x%02x "
        "lsr=0x%02x msr=0x%02x dlab=%d divisor=%d\n",
        name ? name : "", write ? "write" : "read", reg, value, pc,
        claimed ? 1u : 0u, ier, iir, lcr, mcr, lsr, msr, dlab, divisor);
}

bool pcconnect_uart_rx_available(void)
{
    return pcconnect_bridge_uart_rx_available();
}

size_t pcconnect_uart_rx_count(void)
{
    return pcconnect_bridge_uart_rx_count();
}

int pcconnect_uart_rx_pop(void)
{
    return pcconnect_bridge_uart_rx_pop();
}

void pcconnect_uart_rx_clear(void)
{
    pcconnect_bridge_uart_rx_clear();
}

void pcconnect_uart_tx_byte(uint8_t byte)
{
    pcconnect_bridge_uart_tx_byte(byte);
}

uint32_t pcconnect_uart_baud(void)
{
    return pcconnect_bridge_uart_baud();
}
