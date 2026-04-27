#include "stowaway.h"

#include <stdio.h>
#include <string.h>

#define STOWAWAY_RX_CAP 4096u
#define STOWAWAY_RELEASE 0x80u
#define STOWAWAY_KEY_MASK 0x7fu

typedef struct {
    bool enabled;
    uint8_t rx[STOWAWAY_RX_CAP];
    size_t head;
    size_t tail;
    size_t count;
} stowaway_state_t;

static stowaway_state_t g_stowaway;

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

bool stowaway_uart_rx_available(void)
{
    return g_stowaway.enabled && g_stowaway.count > 0;
}

int stowaway_uart_rx_pop(void)
{
    uint8_t byte;

    if (!stowaway_uart_rx_available())
        return -1;

    byte = g_stowaway.rx[g_stowaway.tail];
    g_stowaway.tail = (g_stowaway.tail + 1u) % STOWAWAY_RX_CAP;
    g_stowaway.count--;
    return byte;
}

void stowaway_uart_tx_byte(uint8_t byte)
{
    (void)byte;
}

bool stowaway_queue_key(unsigned scancode, bool release)
{
    uint8_t byte;

    if (!g_stowaway.enabled)
        return false;
    if (scancode > STOWAWAY_KEY_MASK)
        return false;

    byte = (uint8_t)scancode;
    if (release)
        byte |= STOWAWAY_RELEASE;

    if (g_stowaway.count == STOWAWAY_RX_CAP) {
        g_stowaway.tail = (g_stowaway.tail + 1u) % STOWAWAY_RX_CAP;
        g_stowaway.count--;
    }

    g_stowaway.rx[g_stowaway.head] = byte;
    g_stowaway.head = (g_stowaway.head + 1u) % STOWAWAY_RX_CAP;
    g_stowaway.count++;
    return true;
}
