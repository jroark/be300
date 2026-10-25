/*
 * Marvell 88W8385 CF host/station-firmware interface, independently implemented
 * from the register and wire definitions in Linux Libertas if_cs.c, host.h,
 * cfg.c, tx.c and rx.c (v6.6). Firmware records are the supplied AMWL54G.dll's
 * embedded cf8385.bin. See docs/WL54CF.md for exact sources and assumptions.
 * The ARM firmware CPU is not emulated; commands model its station service.
 */
#include "wl54cf.h"
#include <string.h>

static const uint8_t bssid[6] = {2, 0x42, 0x45, 0x33, 0x30, 0x30};
static const uint8_t rates[] = {0x82, 0x84, 0x8b, 0x96, 12, 18, 24, 36, 48, 72, 96, 108};
static uint16_t get16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }
static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}
static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

void wl54cf_reset(wl54cf_t *s)
{
    uint8_t mac[6];
    memcpy(mac, s->mac, 6);
    void *opaque = s->opaque;
    void (*tx)(void *, const uint8_t *, size_t) = s->transmit;
    memset(s, 0, sizeof(*s));
    memcpy(s->mac, mac, 6);
    s->opaque = opaque;
    s->transmit = tx;
    s->mask = 31;
    s->radio = true;
    s->channel = 3;
}
void wl54cf_init(wl54cf_t *s, const uint8_t mac[6])
{
    memset(s, 0, sizeof(*s));
    memcpy(s->mac, mac, 6);
    wl54cf_reset(s);
}
bool wl54cf_irq_pending(const wl54cf_t *s) { return (s->cause & ~s->mask & 31) != 0; }

static void firmware_block(wl54cf_t *s)
{
    wl54cf_buffer_t *c = &s->command;
    if (c->pos != c->length || c->length > WL54CF_BUFFER)
        goto fail;
    if (!s->firmware_stage) {
        if (!c->length) {
            if (!s->helper_bytes)
                goto fail;
            s->firmware_stage = 1;
            s->request = 16;
        } else {
            if (c->length > 256 || (c->length & 1) || s->helper_bytes > 65536 - c->length)
                goto fail;
            s->helper_bytes += c->length;
        }
    } else if (s->firmware_stage == 1) {
        if (c->length != s->request)
            goto fail;
        s->firmware_bytes += c->length;
        if (s->firmware_bytes > 512 * 1024)
            goto fail;
        if (s->firmware_payload) {
            s->firmware_payload = 0;
            s->request = 16;
        } else {
            uint32_t command = get32(c->data), length = get32(c->data + 8);
            if (command == 4 && !length) {
                if (s->firmware_bytes <= 16)
                    goto fail;
                s->firmware_stage = 2;
                s->request = 0;
            } else if (command == 1 && length && length <= WL54CF_BUFFER && !(length & 1)) {
                s->firmware_payload = (uint16_t)length;
                s->request = (uint16_t)length;
            } else
                goto fail;
        }
    }
    return;
fail:
    s->firmware_stage = 3;
    s->request = 0;
}

/* Marvell TLVs have 16-bit type and byte length; IEEE beacon IEs use bytes. */
static const uint8_t *tlv(const uint8_t *p, size_t n, uint16_t type, uint16_t *len)
{
    while (n >= 4) {
        uint16_t t = get16(p), l = get16(p + 2);
        if (l > n - 4)
            return NULL;
        if (t == type) {
            *len = l;
            return p + 4;
        }
        n -= 4 + l;
        p += 4 + l;
    }
    return NULL;
}
static bool valid_tlvs(const uint8_t *p, size_t n)
{
    while (n >= 4) {
        unsigned l = get16(p + 2);
        if (l > n - 4)
            return false;
        p += 4 + l;
        n -= 4 + l;
    }
    return n == 0;
}
static void station_command(wl54cf_t *s)
{
    wl54cf_buffer_t *c = &s->command, *r = &s->response;
    /* A response remains owned by the host until CARD_INT_CAUSE RESP ack. */
    if (s->cause & WL54CF_RESPONSE)
        return;
    memset(r, 0, sizeof(*r));
    if (c->length < 8 || c->length > WL54CF_BUFFER || c->pos != c->length)
        return;
    uint16_t op = get16(c->data), n = get16(c->data + 2), result = 0;
    put16(r->data, op | 0x8000);
    put16(r->data + 4, get16(c->data + 4));
    r->length = 8;
    if (n != c->length || (op & 0x8000)) {
        result = 2;
        goto done;
    }
    switch (op) {
    case 3: /* GET_HW_SPEC: fixed layout from Libertas host.h. */
        if (n < 46) {
            result = 2;
            break;
        }
        r->length = 46;
        put16(r->data + 8, 1);
        put16(r->data + 10, 0x12);
        put16(r->data + 12, 1);
        put16(r->data + 14, 32);
        memcpy(r->data + 16, s->mac, 6);
        put16(r->data + 22, 0x10);
        put16(r->data + 24, 2);
        put32(r->data + 26, 0x00030208); /* 3.2.8p0 */
        put32(r->data + 42, 0x303);
        break;
    case 6: { /* SCAN: one BSS with a normal IEEE beacon IE body. */
        if (n < 15 || !valid_tlvs(c->data + 15, n - 15)) {
            result = 2;
            break;
        }
        uint16_t l = 0;
        const uint8_t *ssid = tlv(c->data + 15, n - 15, 0, &l);
        bool match = s->radio && (c->data[8] == 1 || c->data[8] == 3) &&
                     (!memcmp(c->data + 9, bssid, 6) || !memcmp(c->data + 9, "\0\0\0\0\0\0", 6)) &&
                     (!ssid || !l || (l == 5 && !memcmp(ssid, "BE300", 5)));
        r->length = 11;
        if (!match)
            break;
        uint8_t *p = r->data + 13;
        memcpy(p, bssid, 6);
        p[6] = 40;
        memset(p + 7, 0, 8);
        put16(p + 15, 100);
        put16(p + 17, 1);
        p += 19;
        *p++ = 0;
        *p++ = 5;
        memcpy(p, "BE300", 5);
        p += 5;
        *p++ = 1;
        *p++ = 8;
        memcpy(p, rates, 8);
        p += 8;
        *p++ = 50;
        *p++ = 4;
        memcpy(p, rates + 8, 4);
        p += 4;
        *p++ = 3;
        *p++ = 1;
        *p++ = 3;
        put16(r->data + 11, (uint16_t)(p - r->data - 13));
        put16(r->data + 8, (uint16_t)(p - r->data - 11));
        r->data[10] = 1;
        r->length = (uint16_t)(p - r->data);
        break;
    }
    case 0x11: /* AUTHENTICATE: select open-system authentication. */
        if (n != 25 || memcmp(c->data + 8, bssid, 6) || c->data[14] != 0 || !s->radio)
            result = 2;
        else
            s->authenticated = true;
        break;
    case 0x50: { /* ASSOCIATE with the BSSID and SSID/channel/rate TLVs. */
        r->length = 14;
        put16(r->data + 8, 1);
        uint16_t l = 0;
        const uint8_t *ssid = n >= 21 ? tlv(c->data + 21, n - 21, 0, &l) : NULL;
        bool ok = n >= 21 && valid_tlvs(c->data + 21, n - 21) && ssid && l == 5 &&
                  !memcmp(ssid, "BE300", 5) && !memcmp(c->data + 8, bssid, 6) && s->authenticated &&
                  s->radio && s->channel == 3;
        uint16_t chlen = 0;
        const uint8_t *ch = n >= 21 ? tlv(c->data + 21, n - 21, 3, &chlen) : NULL;
        ok = ok && ch && chlen == 1 && ch[0] == 3 && !(get16(c->data + 14) & 0x12);
        s->associated = ok;
        put16(r->data + 10, ok ? 0 : 4);
        put16(r->data + 12, ok ? 1 : 0);
        break;
    }
    case 0x24: /* DEAUTHENTICATE */
        if (n != 16)
            result = 2;
        else {
            s->associated = false;
            s->authenticated = false;
        }
        break;
    case 0x28: /* MAC_CONTROL */
        if (n != 12 || (get16(c->data + 8) & ~0x1d3u))
            result = 2;
        else {
            s->mac_control = get16(c->data + 8);
            r->length = n;
            memcpy(r->data + 8, c->data + 8, n - 8);
        }
        break;
    case 0x1c: /* RADIO_CONTROL: action, control */
        if (n != 12 || get16(c->data + 8) != 1)
            result = 2;
        else {
            s->radio = (get16(c->data + 10) & 1) != 0;
            if (!s->radio) {
                s->associated = false;
                s->authenticated = false;
            }
            r->length = n;
            memcpy(r->data + 8, c->data + 8, n - 8);
        }
        break;
    default:
        result = 2;
        break;
    }
done:
    put16(r->data + 2, r->length);
    put16(r->data + 6, result);
    /* if_cs_receive_cmdres documents the CF firmware's extra eight bytes. */
    memset(r->data + r->length, 0, 8);
    r->length += 8;
    s->cause |= WL54CF_RESPONSE;
}
static void transmit(wl54cf_t *s)
{
    wl54cf_buffer_t *b = &s->tx;
    if (b->length >= 24 && b->length <= WL54CF_BUFFER && b->pos == b->length && s->associated &&
        (s->mac_control & 2)) {
        uint32_t off = get32(b->data + 8);
        unsigned n = get16(b->data + 12);
        if (off >= 24 && off <= b->length && n >= 14 && n <= 1514 && n <= b->length - off &&
            s->transmit)
            s->transmit(s->opaque, b->data + off, n);
    }
    s->cause |= WL54CF_TX; /* CF download completion; not an over-air ACK. */
}
bool wl54cf_receive(wl54cf_t *s, const uint8_t *packet, size_t n)
{
    if (!s->associated || !(s->mac_control & 1) || n < 14 || n > 1514 ||
        s->rx_count == WL54CF_QUEUE)
        return false;
    bool broadcast = !memcmp(packet, "\xff\xff\xff\xff\xff\xff", 6);
    if (memcmp(packet, s->mac, 6) && !(s->mac_control & 0x80) &&
        !(broadcast && (s->mac_control & 0x40)) &&
        !(packet[0] & 1 && !broadcast && (s->mac_control & 0x100)))
        return false;
    wl54cf_buffer_t *b = &s->rx[(s->rx_head + s->rx_count) % WL54CF_QUEUE];
    memset(b, 0, sizeof(*b));
    b->length = (uint16_t)(20 + n + 8);
    put16(b->data, 1);
    b->data[2] = 40;
    b->data[6] = 96;
    b->data[7] = 11;
    put16(b->data + 4, (uint16_t)(n + 8));
    put32(b->data + 8, 20);
    memcpy(b->data + 20, packet, 12);
    b->data[32] = (uint8_t)((n - 6) >> 8);
    b->data[33] = (uint8_t)(n - 6);
    memcpy(b->data + 34, "\xaa\xaa\x03\0\0\0", 6);
    memcpy(b->data + 40, packet + 12, n - 12);
    s->rx_count++;
    s->cause |= WL54CF_RX;
    return true;
}
static uint64_t stream_read(wl54cf_buffer_t *b, unsigned size)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++)
        v |= (uint64_t)(b->pos < b->length ? b->data[b->pos++] : 0xff) << (8 * i);
    return v;
}
uint64_t wl54cf_read(wl54cf_t *s, unsigned off, unsigned size)
{
    if (!size || size > 8 || off >= 64 || size > 64 - off)
        return UINT64_MAX;
    if (off == 0x10)
        return s->rx_count ? stream_read(&s->rx[s->rx_head], size) : UINT64_MAX;
    if (off == 0x12)
        return stream_read(&s->response, size);
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        unsigned a = off + i;
        uint16_t x = 0;
        switch (a & ~1u) {
        case 0:
            x = s->host_status;
            break;
        case 4:
            x = s->mask;
            break;
        case 0x14:
            x = s->tx.length;
            break;
        case 0x18:
            x = s->command.length;
            break;
        case 0x1c:
            x = 0x12;
            break; /* CF8385 B1 (if_cs.c) */
        case 0x20:
            x = s->firmware_stage == 2
                    ? (WL54CF_TX | WL54CF_COMMAND | (s->cause & (WL54CF_RX | WL54CF_RESPONSE)))
                    : (s->firmware_stage < 2 ? WL54CF_COMMAND : 0);
            break;
        case 0x22:
            x = s->cause;
            break;
        case 0x24:
            x = s->rx_count ? s->rx[s->rx_head].length : 0;
            break;
        case 0x28:
            x = s->request;
            break;
        case 0x30:
            x = s->response.length;
            break;
        case 0x3e:
            x = s->firmware_stage == 2 ? 0x5a00 : 0;
            break;
        }
        v |= (uint64_t)((x >> ((a & 1) * 8)) & 255) << (8 * i);
    }
    return v;
}
static void stream_write(wl54cf_buffer_t *b, unsigned size, uint64_t v)
{
    for (unsigned i = 0; i < size; i++) {
        if (b->pos < b->length && b->pos < WL54CF_BUFFER)
            b->data[b->pos++] = (uint8_t)(v >> (8 * i));
    }
}
void wl54cf_write(wl54cf_t *s, unsigned off, unsigned size, uint64_t v)
{
    if (!size || size > 8 || off >= 64 || size > 64 - off)
        return;
    if (off == 0x16) {
        stream_write(&s->tx, size, v);
        return;
    }
    if (off == 0x1a) {
        stream_write(&s->command, size, v);
        return;
    }
    if ((size != 1 && size != 2) || (off & 1))
        return;
    switch (off) {
    case 0:
        s->host_status = (uint16_t)v & 31;
        break;
    case 2:
        v &= s->host_status;
        if ((v & WL54CF_COMMAND) && s->firmware_stage < 2)
            firmware_block(s);
        else if ((v & WL54CF_COMMAND) && s->firmware_stage == 2)
            station_command(s);
        if ((v & WL54CF_TX) && s->firmware_stage == 2)
            transmit(s);
        if ((v & WL54CF_RX) && s->rx_count) {
            s->rx_head = (s->rx_head + 1) % WL54CF_QUEUE;
            s->rx_count--;
        }
        s->host_status = 0;
        break;
    case 4:
        s->mask = (uint16_t)v & 31;
        break;
    case 0x14:
        s->tx.length = (uint16_t)v;
        s->tx.pos = 0;
        break;
    case 0x18:
        s->command.length = (uint16_t)v;
        s->command.pos = 0;
        break;
    case 0x22:
        s->cause &= ~v;
        if (v & WL54CF_RESPONSE) {
            s->response.length = 0;
            s->response.pos = 0;
        }
        if (s->rx_count)
            s->cause |= WL54CF_RX;
        break;
    }
}
