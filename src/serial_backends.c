/*
 *  src/serial_backends.c — the BE-300 host backends behind GXemul's
 *  ns16550 UARTs (gxemul/src/include/ns16550_backend.h).
 *
 *  Adapters over the PC Connect cable (pcconnect.c), the Stowaway keyboard
 *  dock (stowaway.c) and the plain host serial bridges (pcconnect_bridge.c).
 *  A UART name is claimed in that order, as dev_ns16550.c used to check
 *  them itself.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ns16550_backend.h"
#include "pcconnect.h"
#include "pcconnect_bridge.h"
#include "serial_backends.h"
#include "stowaway.h"

/* ---------- PC Connect ---------- */

static size_t pcc_rx_count(const char *name)
{
    (void)name;
    return pcconnect_uart_rx_count();
}

static int pcc_rx_pop(const char *name)
{
    (void)name;
    return pcconnect_uart_rx_pop();
}

static void pcc_rx_clear(const char *name)
{
    (void)name;
    pcconnect_uart_rx_clear();
}

static void pcc_tx_byte(const char *name, uint8_t byte)
{
    (void)name;
    pcconnect_uart_tx_byte(byte);
}

static int pcc_cable_connected(void)
{
    return pcconnect_cable_connected();
}

static int pcc_trace_enabled(void)
{
    return pcconnect_trace_enabled();
}

static void pcc_trace_access(const char *name, int write, unsigned reg,
    uint8_t value, uint32_t pc, uint8_t ier, uint8_t iir, uint8_t lcr,
    uint8_t mcr, uint8_t lsr, uint8_t msr, int dlab, int divisor)
{
    pcconnect_trace_uart_access(name, true, write != 0, reg, value, pc,
        ier, iir, lcr, mcr, lsr, msr, dlab, divisor);
}

static const struct ns16550_backend pcconnect_backend = {
    .kind = NS16550_BACKEND_PCCONNECT,
    .rx_count = pcc_rx_count,
    .rx_pop = pcc_rx_pop,
    .rx_clear = pcc_rx_clear,
    .tx_byte = pcc_tx_byte,
    .cable_connected = pcc_cable_connected,
    .baud = pcconnect_uart_baud,
    .signal_irq = pcconnect_signal_uart_irq,
    .note_config = pcconnect_note_uart_config,
    .trace_enabled = pcc_trace_enabled,
    .trace_access = pcc_trace_access,
};

/* ---------- Stowaway keyboard dock ---------- */

static size_t stw_rx_count(const char *name)
{
    (void)name;
    return stowaway_uart_rx_count();
}

static int stw_rx_pop(const char *name)
{
    (void)name;
    return stowaway_uart_rx_pop();
}

static void stw_rx_clear(const char *name)
{
    (void)name;
    stowaway_uart_rx_clear();
}

static void stw_tx_byte(const char *name, uint8_t byte)
{
    (void)name;
    stowaway_uart_tx_byte(byte);
}

static int stw_take_modem_delta(void)
{
    return stowaway_uart_take_modem_delta();
}

static void stw_note_modem_control(int dtr, int rts)
{
    stowaway_uart_note_modem_control(dtr != 0, rts != 0);
}

static const struct ns16550_backend stowaway_backend = {
    .kind = NS16550_BACKEND_STOWAWAY,
    .rx_count = stw_rx_count,
    .rx_pop = stw_rx_pop,
    .rx_clear = stw_rx_clear,
    .tx_byte = stw_tx_byte,
    .take_modem_delta = stw_take_modem_delta,
    .note_modem_wait = stowaway_uart_note_modem_wait,
    .note_port_config = stowaway_uart_note_port_config,
    .note_modem_control = stw_note_modem_control,
};

/* ---------- plain serial bridges ---------- */

static const struct ns16550_backend serial_bridge_backend = {
    .kind = NS16550_BACKEND_SERIAL_BRIDGE,
    .rx_count = serial_bridge_uart_rx_count,
    .rx_pop = serial_bridge_uart_rx_pop,
    .rx_clear = serial_bridge_uart_rx_clear,
    .tx_byte = serial_bridge_uart_tx_byte,
};

static const struct ns16550_backend *be300_serial_backend(const char *name)
{
    if (pcconnect_ns16550_claims(name))
        return &pcconnect_backend;
    if (stowaway_ns16550_claims(name))
        return &stowaway_backend;
    if (serial_bridge_ns16550_claims(name))
        return &serial_bridge_backend;
    return NULL;
}

void be300_serial_backends_install(void)
{
    ns16550_set_backend_resolver(be300_serial_backend);
}
