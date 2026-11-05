/* 3Com EtherLink III PC Card (3c589) register-window model; see el3.h.
 * Commands complete at once, so CMD_BUSY never reads set. */
#include "el3.h"
#include <string.h>

enum {
    INT_LATCH = 0x0001, ADAPTER_FAILURE = 0x0002, TX_COMPLETE = 0x0004,
    TX_AVAILABLE = 0x0008, RX_COMPLETE = 0x0010, RX_EARLY = 0x0020,
    INT_REQ = 0x0040, STATS_FULL = 0x0080,
};
enum { RX_STATION = 1, RX_MULTICAST = 2, RX_BROADCAST = 4, RX_PROMISC = 8 };

void el3_reset(el3_t *s)
{
    s->window = 0;
    s->config_ctrl = s->addr_config = s->media = 0;
    s->resource = 0x3F00;
    s->eeprom_cmd = 0;
    s->latched = s->int_mask = s->status_mask = 0;
    s->rx_filter = 0;
    s->rx_enabled = s->tx_enabled = false;
    s->tx_used = s->tx_status_count = 0;
    s->rx_head = s->rx_count = s->rx_pos = 0;
}

void el3_init(el3_t *s, const uint8_t mac[6])
{
    void *opaque = s->opaque;
    void (*transmit)(void *, const uint8_t *, size_t) = s->transmit;
    memset(s, 0, sizeof(*s));
    s->opaque = opaque;
    s->transmit = transmit;
    /* EEPROM words 0-2: the station address, high byte first (Linux
     * tc589_config byte-swaps them with htons). Word 7: 3Com's ID. */
    for (int i = 0; i < 3; i++)
        s->eeprom[i] = (uint16_t)(mac[2 * i] << 8 | mac[2 * i + 1]);
    s->eeprom[3] = 0x9058;
    s->eeprom[7] = 0x6D50;
    el3_reset(s);
}

static uint16_t status(const el3_t *s)
{
    uint16_t st = s->latched;
    if (s->rx_enabled && s->rx_count)
        st |= RX_COMPLETE;
    if (st & s->int_mask & 0xFE)
        st |= INT_LATCH;
    return (uint16_t)((st & (s->status_mask | INT_LATCH)) | (s->window << 13));
}

bool el3_irq_pending(const el3_t *s)
{
    return (status(s) & s->int_mask & 0xFF) != 0;
}

bool el3_rx_full(const el3_t *s) { return s->rx_count == EL3_RX_FRAMES; }

static void rx_pop(el3_t *s)
{
    if (!s->rx_count)
        return;
    s->rx_head = (s->rx_head + 1) % EL3_RX_FRAMES;
    s->rx_count--;
    s->rx_pos = 0;
}

static void command(el3_t *s, uint16_t value)
{
    unsigned op = value >> 11, arg = value & 0x7FF;
    switch (op) {
    case 0: el3_reset(s); break;                        /* TotalReset */
    case 1: s->window = arg & 7; break;                 /* SelectWindow */
    case 3: s->rx_enabled = false; break;
    case 4: s->rx_enabled = true; break;
    case 5: s->rx_head = s->rx_count = s->rx_pos = 0; s->rx_enabled = false; break;
    case 8: rx_pop(s); break;                           /* RxDiscard */
    case 9: s->tx_enabled = true; break;
    case 10: s->tx_enabled = false; break;
    case 11: s->tx_used = s->tx_status_count = 0; s->tx_enabled = false; break;
    case 12: s->latched |= INT_REQ; break;              /* RequestInterrupt */
    case 13: s->latched &= (uint16_t)~arg; break;       /* AckIntr */
    case 14: s->int_mask = arg & 0xFF; break;           /* SetIntrEnb */
    case 15: s->status_mask = arg & 0xFF; break;        /* SetReadZeroMask */
    case 16: s->rx_filter = arg & 0x0F; break;
    default: break;                                     /* coax, stats, thresholds */
    }
}

/* A complete frame in the TX FIFO: the length dword, data padded to 4. */
static void tx_flush(el3_t *s)
{
    while (s->tx_used >= 4) {
        unsigned length = (s->tx[0] | s->tx[1] << 8) & 0x7FF;
        unsigned need = 4 + ((length + 3) & ~3u);
        if (s->tx_used < need)
            return;
        if (s->tx_enabled && s->transmit && length >= 14)
            s->transmit(s->opaque, s->tx + 4, length);
        if (s->tx[1] & 0x80) {                          /* interrupt on success */
            if (s->tx_status_count < 4)
                s->tx_status[s->tx_status_count++] = 0x80;
            s->latched |= TX_COMPLETE;
        }
        memmove(s->tx, s->tx + need, s->tx_used - need);
        s->tx_used -= need;
    }
}

static void fifo_write(el3_t *s, unsigned size, uint64_t value)
{
    for (unsigned i = 0; i < size; i++) {
        if (s->tx_used >= sizeof(s->tx)) {
            s->latched |= ADAPTER_FAILURE;              /* overrun */
            s->tx_used = 0;
            return;
        }
        s->tx[s->tx_used++] = (uint8_t)(value >> (8 * i));
    }
    tx_flush(s);
}

static uint64_t fifo_read(el3_t *s, unsigned size)
{
    uint64_t v = 0;
    if (!s->rx_count)
        return 0;
    const uint8_t *d = s->rx[s->rx_head].data;
    for (unsigned i = 0; i < size; i++) {
        unsigned p = s->rx_pos++;
        v |= (uint64_t)(p < sizeof(s->rx[0].data) ? d[p] : 0) << (8 * i);
    }
    return v;
}

static uint16_t reg16(el3_t *s, unsigned offset)
{
    switch (s->window) {
    case 0:
        switch (offset) {
        case 0x00: return 0x6D50;
        case 0x02: return 0x9058;
        case 0x04: return s->config_ctrl;
        case 0x06: return s->addr_config;
        case 0x08: return s->resource;
        case 0x0A: return s->eeprom_cmd & 0x7FFF;
        case 0x0C: return s->eeprom[s->eeprom_cmd & 0x0F];
        }
        break;
    case 1:
        switch (offset) {
        case 0x08:
            if (!s->rx_count) return 0x8000;
            return s->rx[s->rx_head].length & 0x7FF;
        case 0x0A:                                      /* TX status in the high byte */
            return (uint16_t)((s->tx_status_count ? s->tx_status[0] : 0) << 8);
        case 0x0C: return (uint16_t)(EL3_FIFO - s->tx_used);
        }
        break;
    case 2:
        if (offset < 6)
            return (uint16_t)(s->station[offset] | s->station[offset + 1] << 8);
        break;
    case 4:
        if (offset == 0x0A) return s->media | 0x0800;   /* link beat detected */
        break;
    }
    return 0;
}

uint64_t el3_read(el3_t *s, unsigned offset, unsigned size)
{
    offset &= 0x0F;
    if (offset >= 0x0E)
        return status(s) >> (8 * (offset - 0x0E));
    if (s->window == 1 && offset < 4)
        return fifo_read(s, size);
    uint16_t w = reg16(s, offset & ~1u);
    if (size == 1)
        return (offset & 1) ? w >> 8 : w & 0xFF;
    return w;
}

void el3_write(el3_t *s, unsigned offset, unsigned size, uint64_t value)
{
    offset &= 0x0F;
    if (offset == 0x0E && size >= 2) {
        command(s, (uint16_t)value);
        return;
    }
    if (s->window == 1 && offset < 4) {
        fifo_write(s, size, value);
        return;
    }
    if (s->window == 1 && offset == 0x0B) {             /* pop TX status */
        if (s->tx_status_count) {
            memmove(s->tx_status, s->tx_status + 1, --s->tx_status_count);
        }
        if (!s->tx_status_count)
            s->latched &= (uint16_t)~TX_COMPLETE;
        return;
    }
    if (s->window == 2 && offset < 6) {
        for (unsigned i = 0; i < size && offset + i < 6; i++)
            s->station[offset + i] = (uint8_t)(value >> (8 * i));
        return;
    }
    if (s->window == 0) {
        switch (offset) {
        case 0x04: s->config_ctrl = (uint16_t)value; break;
        case 0x06: s->addr_config = (uint16_t)value; break;
        case 0x08: s->resource = (uint16_t)value; break;
        case 0x0A: s->eeprom_cmd = (uint16_t)value; break;
        }
        return;
    }
    if (s->window == 4 && offset == 0x0A)
        s->media = (uint16_t)value;
}

bool el3_receive(el3_t *s, const uint8_t *frame, size_t length)
{
    if (!s->rx_enabled || length < 14 || length > sizeof(s->rx[0].data))
        return false;
    bool broadcast = !memcmp(frame, "\xff\xff\xff\xff\xff\xff", 6);
    bool ours = !memcmp(frame, s->station, 6);
    if (!(s->rx_filter & RX_PROMISC) &&
        !(ours && (s->rx_filter & RX_STATION)) &&
        !(broadcast && (s->rx_filter & RX_BROADCAST)) &&
        !(!broadcast && (frame[0] & 1) && (s->rx_filter & RX_MULTICAST)))
        return true;                                    /* filtered, consumed */
    if (s->rx_count == EL3_RX_FRAMES) {
        s->latched |= ADAPTER_FAILURE;                  /* RX overrun */
        return false;
    }
    unsigned slot = (s->rx_head + s->rx_count++) % EL3_RX_FRAMES;
    s->rx[slot].length = (uint16_t)length;
    memcpy(s->rx[slot].data, frame, length);
    return true;
}
