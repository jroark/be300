/* Marvell CF8385 station interface. Sources and limits: docs/WL54CF.md. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WL54CF_BUFFER 2048u
#define WL54CF_QUEUE 8u
#define WL54CF_TX 1u
#define WL54CF_RX 2u
#define WL54CF_COMMAND 4u
#define WL54CF_RESPONSE 8u
#define WL54CF_EVENT 16u

typedef struct {
    uint8_t data[WL54CF_BUFFER];
    uint16_t length, pos;
} wl54cf_buffer_t;
typedef struct {
    uint8_t mac[6];
    uint8_t firmware_stage; /* ROM, helper, station, failed */
    uint16_t mask, cause, host_status, mac_control;
    uint16_t request, firmware_payload;
    uint32_t helper_bytes, firmware_bytes;
    bool authenticated, associated, radio;
    uint8_t channel;
    wl54cf_buffer_t command, response, tx, rx[WL54CF_QUEUE];
    unsigned rx_head, rx_count;
    void *opaque;
    void (*transmit)(void *, const uint8_t *, size_t);
} wl54cf_t;

void wl54cf_init(wl54cf_t *s, const uint8_t mac[6]);
void wl54cf_reset(wl54cf_t *s);
uint64_t wl54cf_read(wl54cf_t *s, unsigned offset, unsigned size);
void wl54cf_write(wl54cf_t *s, unsigned offset, unsigned size, uint64_t value);
bool wl54cf_irq_pending(const wl54cf_t *s);
bool wl54cf_receive(wl54cf_t *s, const uint8_t *packet, size_t length);
