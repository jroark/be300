/* Marvell CF8385 station interface. Sources and limits: docs/WL54CF.md. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "wifi_security.h"

#define WL54CF_BUFFER 2048u
#define WL54CF_QUEUE 8u
#define WL54CF_TX 1u
#define WL54CF_RX 2u
#define WL54CF_COMMAND 4u
#define WL54CF_RESPONSE 8u
#define WL54CF_EVENT 16u
#define WL54CF_EVENT_LINK_LOST 0x03u
#define WL54CF_EVENT_PS_AWAKE 0x0au
#define WL54CF_EVENT_PS_SLEEP 0x0bu
#define WL54CF_PS_IDLE_TICKS 4u     /* idle ticks before PS_SLEEP */
#define WL54CF_PS_LISTEN_TICKS 10u  /* ticks asleep per DTIM listen */

typedef struct {
    uint8_t data[WL54CF_BUFFER];
    uint16_t length, pos;
} wl54cf_buffer_t;
typedef struct {
    uint8_t mac[6];
    wifi_security_t security;
    uint8_t wep[4][16], wep_len[4], wep_index;
    bool rsn;
    uint8_t firmware_stage; /* ROM, helper, station, failed */
    uint16_t mask, cause, host_status, mac_control;
    uint16_t request, firmware_payload;
    uint32_t helper_bytes, firmware_bytes;
    bool authenticated, associated, radio;
    uint8_t channel;
    /* 802.11 power save (Linux libertas cmd.c/cmdresp.c): the firmware
     * raises PS_SLEEP when idle, sleeps on the host's confirm and raises
     * PS_AWAKE at each listen interval. Host access while asleep counts as
     * a violation, which tests require to stay zero. */
    uint8_t event;              /* CARD_STATUS bits 14:8 while EVENT is set */
    bool ps, ps_pending, asleep;
    uint16_t ps_dtim;
    unsigned idle_ticks, sleep_ticks, ps_sleeps, ps_wakes, ps_violations;
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
void wl54cf_ps_tick(wl54cf_t *s);
bool wl54cf_receive(wl54cf_t *s, const uint8_t *packet, size_t length);
