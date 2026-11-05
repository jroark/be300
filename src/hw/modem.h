/* PC Card modem model: an NS16550A UART behind a Hayes-compatible command
 * set. ATD connects a TCP socket to the --modem HOST:PORT peer (a PPP
 * server in tests) and reports CONNECT; the data then flows both ways
 * until the peer closes, ATH, or DTR drops (&D2). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MODEM_FIFO 4096u

typedef struct {
    /* UART registers (NS16550A datasheet). */
    uint8_t ier, lcr, mcr, scr, fcr, dll, dlm;
    uint8_t msr_last;
    bool thre_pending;
    uint8_t rx[MODEM_FIFO];
    unsigned rx_head, rx_count;
    /* Modem. */
    char peer[128];             /* host:port for ATD */
    int sock;                   /* -1: on hook */
    bool online;                /* data mode */
    bool echo, verbose;
    char line[128];
    unsigned line_len;
    unsigned connects;
} modem_t;

void modem_init(modem_t *m, const char *peer);
void modem_reset(modem_t *m);
uint8_t modem_read(modem_t *m, unsigned reg);
void modem_write(modem_t *m, unsigned reg, uint8_t value);
bool modem_irq_pending(const modem_t *m);
void modem_tick(modem_t *m);
void modem_close(modem_t *m);
