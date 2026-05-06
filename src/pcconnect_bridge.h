#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    PCC_BR_NONE = 0,
    PCC_BR_TCP_CONNECT,    /* tcp:host:port           */
    PCC_BR_TCP_LISTEN,     /* tcp-listen:port[@addr]  */
    PCC_BR_UNIX_CONNECT,   /* unix:/path              */
    PCC_BR_UNIX_LISTEN,    /* unix-listen:/path       */
    PCC_BR_PTY,            /* pty:auto                */
} pcc_bridge_kind_t;

typedef struct {
    pcc_bridge_kind_t kind;
    const char *target;     /* host:port / :port / /path / "auto" */
    const char *tee_path;   /* optional, NULL = no tee */
    const char *uart_name;  /* default "vrc4173siu" */
    bool trace;
    /* Pace guest->host transmission to a serial-equivalent rate so PCConnect
     * sees inter-byte gaps similar to real hardware. 0 = unlimited.
     * 115200 = 8N1 baud, ~87 us per byte. */
    uint32_t baud;
} pcc_bridge_config_t;

/* Parse a `--pcconnect-bridge <spec>` argument into a config. Returns
 * false (and prints to stderr) if the spec is malformed. The returned
 * `target` borrows from the supplied string; caller must keep it alive. */
bool pcconnect_bridge_parse_spec(const char *spec, pcc_bridge_config_t *out);

/* Open the host fd / listener and arm the bridge. Returns false on error
 * and prints details to stderr. */
bool pcconnect_bridge_configure(const pcc_bridge_config_t *cfg);

/* Drain TX, accept new connections, reconnect on error. Called every
 * machine tick from be300_pcconnect_poll(). */
void pcconnect_bridge_tick(void);

/* Close fds, flush tee, release resources. */
void pcconnect_bridge_shutdown(void);

/* Backend entry points called via dispatch from pcconnect.c shims. */
bool pcconnect_bridge_ns16550_claims(const char *name);
bool pcconnect_bridge_uart_rx_available(void);
size_t pcconnect_bridge_uart_rx_count(void);
int  pcconnect_bridge_uart_rx_pop(void);
void pcconnect_bridge_uart_rx_clear(void);
void pcconnect_bridge_uart_tx_byte(uint8_t byte);
uint32_t pcconnect_bridge_uart_baud(void);
void pcconnect_bridge_set_cable_connected(bool connected);
void pcconnect_bridge_reset_guest_serial(void);
void pcconnect_bridge_set_rx_ready_callback(void (*cb)(void *opaque),
    void *opaque);
void pcconnect_bridge_note_uart_config(const char *name, uint8_t lcr,
    uint8_t mcr, uint8_t ier, int divisor, int dlab);
bool pcconnect_bridge_cable_connected(void);
bool pcconnect_bridge_trace_enabled(void);
