/* 3Com EtherLink III PC Card (3c589) register-window model.
 * Register map: 3Com "EtherLink III Parallel Tasking Adapter Technical
 * Reference" and Linux 3c589_cs.c. Only what a PIO driver uses: windows
 * 0, 1, 2, 4 and 6, the command register, TX/RX FIFOs, the EEPROM. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EL3_RX_FRAMES 8u
#define EL3_FIFO 2048u

typedef struct {
    unsigned window;
    uint16_t eeprom[16];
    uint8_t station[6];
    uint16_t config_ctrl, addr_config, resource, media;
    uint16_t eeprom_cmd;
    uint16_t latched;          /* INT_LATCH, ADAPTER_FAILURE, TX_COMPLETE, ... */
    uint16_t int_mask, status_mask;
    uint8_t rx_filter;
    bool rx_enabled, tx_enabled;
    uint8_t tx[EL3_FIFO + 8];
    unsigned tx_used;
    uint8_t tx_status[4];
    unsigned tx_status_count;
    struct { uint16_t length; uint8_t data[1536]; } rx[EL3_RX_FRAMES];
    unsigned rx_head, rx_count, rx_pos;
    void *opaque;
    void (*transmit)(void *, const uint8_t *, size_t);
} el3_t;

void el3_init(el3_t *s, const uint8_t mac[6]);
void el3_reset(el3_t *s);
uint64_t el3_read(el3_t *s, unsigned offset, unsigned size);
void el3_write(el3_t *s, unsigned offset, unsigned size, uint64_t value);
bool el3_irq_pending(const el3_t *s);
bool el3_receive(el3_t *s, const uint8_t *frame, size_t length);
bool el3_rx_full(const el3_t *s);
