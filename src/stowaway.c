#include "stowaway.h"

#include <stdio.h>
#include <string.h>

#define STOWAWAY_RX_CAP 4096u
#define STOWAWAY_RELEASE 0x80u
#define STOWAWAY_KEY_MASK 0x7fu
#define STOWAWAY_PROBE_ACK_0 0xfau
#define STOWAWAY_PROBE_ACK_1 0xfdu
#define STOWAWAY_MODEM_EVENT_LIMIT 2u

typedef struct {
    bool enabled;
    bool connected;
    bool modem_wait_requested;
    unsigned modem_events_delivered;
    bool dtr_asserted;
    bool rts_asserted;
    bool probe_ack_pending;
    unsigned probe_ack_seen;
    uint8_t rx[STOWAWAY_RX_CAP];
    size_t head;
    size_t tail;
    size_t count;
} stowaway_state_t;

static stowaway_state_t g_stowaway;

static void stowaway_queue_byte(uint8_t byte)
{
    if (g_stowaway.count == STOWAWAY_RX_CAP) {
        g_stowaway.tail = (g_stowaway.tail + 1u) % STOWAWAY_RX_CAP;
        g_stowaway.count--;
    }

    g_stowaway.rx[g_stowaway.head] = byte;
    g_stowaway.head = (g_stowaway.head + 1u) % STOWAWAY_RX_CAP;
    g_stowaway.count++;
}

void stowaway_configure(bool enabled)
{
    memset(&g_stowaway, 0, sizeof(g_stowaway));
    g_stowaway.enabled = enabled;
    if (enabled)
        fprintf(stderr, "[STOWAWAY] serial keyboard dock enabled on COM1:\n");
}

bool stowaway_ns16550_claims(const char *name)
{
    return g_stowaway.enabled && name && strcmp(name, "vrc4173siu") == 0;
}

void stowaway_uart_reset(void)
{
    bool enabled = g_stowaway.enabled;

    memset(&g_stowaway, 0, sizeof(g_stowaway));
    g_stowaway.enabled = enabled;
}

bool stowaway_uart_rx_available(void)
{
    return g_stowaway.enabled && g_stowaway.count > 0;
}

size_t stowaway_uart_rx_count(void)
{
    if (!g_stowaway.enabled)
        return 0;
    return g_stowaway.count;
}

int stowaway_uart_rx_pop(void)
{
    uint8_t byte;

    if (!stowaway_uart_rx_available())
        return -1;

    byte = g_stowaway.rx[g_stowaway.tail];
    g_stowaway.tail = (g_stowaway.tail + 1u) % STOWAWAY_RX_CAP;
    g_stowaway.count--;
    if (g_stowaway.probe_ack_pending) {
        if (byte == STOWAWAY_PROBE_ACK_0 && g_stowaway.probe_ack_seen == 0) {
            g_stowaway.probe_ack_seen = 1;
        } else if (byte == STOWAWAY_PROBE_ACK_1 &&
            g_stowaway.probe_ack_seen == 1) {
            g_stowaway.connected = true;
            g_stowaway.probe_ack_pending = false;
            g_stowaway.probe_ack_seen = 0;
        } else {
            g_stowaway.probe_ack_pending = false;
            g_stowaway.probe_ack_seen = 0;
        }
    }
    return byte;
}

void stowaway_uart_rx_clear(void)
{
    if (!g_stowaway.enabled)
        return;

    g_stowaway.head = 0;
    g_stowaway.tail = 0;
    g_stowaway.count = 0;
    g_stowaway.probe_ack_pending = false;
    g_stowaway.probe_ack_seen = 0;
}

void stowaway_uart_tx_byte(uint8_t byte)
{
    (void)byte;
}

static void stowaway_queue_probe_ack(void)
{
    if (g_stowaway.probe_ack_pending || g_stowaway.connected)
        return;

    stowaway_queue_byte(STOWAWAY_PROBE_ACK_0);
    stowaway_queue_byte(STOWAWAY_PROBE_ACK_1);
    g_stowaway.probe_ack_pending = true;
    g_stowaway.probe_ack_seen = 0;
    /*
     * The probe ACK is host-to-guest RX data just like later key events.
     * Wake serial.dll through the BE-300 CommMode latch so it drains the
     * UART before the driver times out and resets the FIFO.
     */
    be300_stowaway_signal_uart_irq(1);
}

void stowaway_uart_note_port_config(void)
{
    if (!g_stowaway.enabled)
        return;

    g_stowaway.connected = false;
    g_stowaway.modem_wait_requested = false;
    g_stowaway.modem_events_delivered = 0;
    g_stowaway.dtr_asserted = false;
    g_stowaway.rts_asserted = false;
    g_stowaway.probe_ack_pending = false;
    g_stowaway.probe_ack_seen = 0;
    g_stowaway.head = 0;
    g_stowaway.tail = 0;
    g_stowaway.count = 0;
}

void stowaway_uart_note_modem_wait(void)
{
    if (!g_stowaway.enabled)
        return;

    g_stowaway.modem_wait_requested = true;
}

void stowaway_uart_note_modem_control(bool dtr_asserted, bool rts_asserted)
{
    bool old_rts;

    if (!g_stowaway.enabled)
        return;

    old_rts = g_stowaway.rts_asserted;
    g_stowaway.dtr_asserted = dtr_asserted;
    g_stowaway.rts_asserted = rts_asserted;

    /*
     * The physical dock's DCD/modem wake can be observed before the driver
     * asserts DTR.  The keyboard probe ACK is produced when the driver raises
     * RTS while DTR is already asserted.
     */
    if (dtr_asserted && rts_asserted && !old_rts)
        stowaway_queue_probe_ack();
}

bool stowaway_uart_take_modem_wait_request(void)
{
    if (!g_stowaway.enabled || !g_stowaway.modem_wait_requested ||
        g_stowaway.modem_events_delivered >= STOWAWAY_MODEM_EVENT_LIMIT)
        return false;

    g_stowaway.modem_events_delivered++;
    return true;
}

bool stowaway_queue_key(unsigned scancode, bool release)
{
    uint8_t byte;

    if (!g_stowaway.enabled || !g_stowaway.connected)
        return false;
    if (scancode > STOWAWAY_KEY_MASK)
        return false;

    byte = (uint8_t)scancode;
    if (release)
        byte |= STOWAWAY_RELEASE;

    stowaway_queue_byte(byte);

    /*
     * Wake serial.dll: the ns16550 IRQ on giu.0 alone is invisible to the
     * WinCE OAL, which dispatches GIU 0 by reading the AA000004 / AA008004
     * latches. Synthesize a CommMode modem event so SYSINTR 0x23 fires and
     * serial.dll's IST polls the UART, drains our RX queue, and signals
     * EV_RXCHAR back to the user-mode worker thread blocked in
     * WaitCommEvent.
     */
    be300_stowaway_signal_uart_irq(1);
    return true;
}
