/* CF data modem: a 16550 UART with a Hayes (AT) command interpreter, as on
 * the Rockwell RC56LD-PCC card (glaze-os docs/hardware/MODEM.md). Online
 * data goes to a host backend through callbacks; there is no phone line. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CF_MODEM_RX 4096u

typedef struct {
    /* 16550 registers (NS16550 data sheet). */
    uint8_t ier, lcr, mcr, fcr, scr, dll, dlm, msr_delta;
    bool thre_irq;
    uint8_t rx[CF_MODEM_RX];
    unsigned rx_head, rx_count;
    /* Hayes state. */
    bool online, echo, verbose;
    char cmd[96];
    unsigned cmd_len;
    unsigned connect_rate;
    /* Backend: dial returns whether a peer answered; tx carries online data. */
    void *opaque;
    bool (*dial)(void *opaque, const char *number);
    void (*hangup)(void *opaque);
    void (*tx)(void *opaque, const uint8_t *data, size_t len);
} cf_modem_t;

void cf_modem_reset(cf_modem_t *m);
uint8_t cf_modem_read(cf_modem_t *m, unsigned reg);
void cf_modem_write(cf_modem_t *m, unsigned reg, uint8_t value);
/* Online data from the peer; returns how much fit. */
size_t cf_modem_receive(cf_modem_t *m, const uint8_t *data, size_t len);
size_t cf_modem_rx_space(const cf_modem_t *m);
/* The peer went away: NO CARRIER, DCD drops. */
void cf_modem_carrier_lost(cf_modem_t *m);
bool cf_modem_irq_pending(const cf_modem_t *m);
