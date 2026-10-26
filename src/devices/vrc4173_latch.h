/*
 *  src/devices/vrc4173_latch.h — the VRC4173 companion latch shared by
 *  src/be300_devices.c (dispatch and registration) and the per-function
 *  files split from it: serial_dock.c and display_ops.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "be300.h"
#include "interrupt.h"
#include "hw/usbf.h"

struct cpu;

#define VRC4173_LATCH_BASE   0x0A000000ULL
#define VRC4173_LATCH_SIZE   0x00020000     /* 128KB covers all VRC4173 space */
struct be300_vrc4173_latch {
    uint8_t  bytes[0x20000];
    bool     log_mmio;
    usbf_state_t usbf;
    struct interrupt usb_irq;
    bool     usb_irq_connected;
    bool     usb_irq_asserted;
    struct interrupt aiu_irq;
    bool     aiu_irq_connected;
    bool     aiu_irq_asserted;
    struct interrupt pcconnect_irq;
    bool     pcconnect_irq_connected;
    bool     pcconnect_irq_asserted;
    struct interrupt cf_irq;
    bool     cf_irq_connected;
    bool     cf_irq_asserted;
    bool     pcconnect_dock_connected;
    uint16_t pcconnect_commmode_pending;
    uint16_t stowaway_commmode_events;
    bool     pcconnect_modem_after_socket_sent;
    size_t   pcconnect_rx_wake_count;
    bool     serial_dock_pccard_status_pending;
    bool     pcconnect_insert_armed;
    uint32_t pcconnect_insert_delay_ms;
    uint64_t pcconnect_insert_deadline_ns;
};

static inline uint32_t be300_latch_peek_u32(struct be300_vrc4173_latch *d,
    uint32_t off)
{
    if (!d || off + 4u > VRC4173_LATCH_SIZE)
        return 0;

    return (uint32_t)d->bytes[off + 0u]
         | ((uint32_t)d->bytes[off + 1u] << 8)
         | ((uint32_t)d->bytes[off + 2u] << 16)
         | ((uint32_t)d->bytes[off + 3u] << 24);
}

static inline uint16_t be300_latch_peek_u16(const struct be300_vrc4173_latch *d,
    uint32_t off)
{
    if (!d || off + 2u > VRC4173_LATCH_SIZE)
        return 0;

    return (uint16_t)d->bytes[off + 0u]
         | ((uint16_t)d->bytes[off + 1u] << 8);
}

/* serial_dock.c */
void be300_commmode_event_ack(struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len, uint64_t val);
uint32_t be300_pcconnect_connect_delay_ms(void);
uint32_t be300_pcconnect_girq0_source_bits(
    const struct be300_vrc4173_latch *d);
void be300_pcconnect_irq_update(struct be300_vrc4173_latch *d);
void be300_pcconnect_note_comm_write(struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len, uint64_t val, uint32_t pc);
uint64_t be300_pcconnect_pccard_status_read(
    struct be300_vrc4173_latch *d, bool cf_attached, uint32_t off,
    unsigned len, uint64_t val);
void be300_pcconnect_trace(const struct be300_vrc4173_latch *d,
    const char *what, uint32_t off, uint32_t len, uint64_t val, uint32_t pc);
void be300_pcconnect_uart_irq_ready(void *opaque);
bool be300_commmode_modem_event_claims(struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len);
uint64_t be300_commmode_modem_event_read(struct cpu *cpu,
    struct be300_vrc4173_latch *d, uint32_t off, unsigned len);
bool be300_commmode_serial_socket_claims(struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len);
uint64_t be300_commmode_serial_socket_read(struct cpu *cpu,
    struct be300_vrc4173_latch *d, uint32_t off, unsigned len);
bool be300_commmode_socket_claims(struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len);
uint64_t be300_commmode_socket_read(struct cpu *cpu,
    struct be300_vrc4173_latch *d, uint32_t off, unsigned len);
bool be300_commmode_status_claims(struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len);
uint64_t be300_commmode_status_read(struct cpu *cpu,
    struct be300_vrc4173_latch *d, uint32_t off, unsigned len);

/* display_ops.c */
void be300_vrc4173_200_display_op_maybe(
    struct be300_vrc4173_latch *d);
void be300_vrc4173_a00_blit_maybe(struct cpu *cpu,
    struct be300_vrc4173_latch *d);
