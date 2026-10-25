/*
 * Z-Com XI-825 PRISM/HFA384x station firmware interface.
 * Register/record/frame evidence: docs/XI825.md, nb825be.dll 1.1.0
 * (preferred image base 0x10000000), and Linux hostap's HFA384x definitions.
 * This models the card's resident station firmware, including a virtual AP;
 * it does not replace the guest miniport or modify guest memory.
 */
#include "xi825.h"
#include <string.h>

enum {
    CMD = 0,
    PARAM0 = 1,
    STATUS = 4,
    RESP0 = 5,
    INFOFID = 8,
    SELECT0 = 12,
    OFFSET0 = 14,
    RXFID = 16,
    ALLOCFID = 17,
    TXFID = 18,
    EVSTAT = 24,
    INTEN = 25,
    EVACK = 26,
    DATA0 = 27,
    DATA1 = 28
};
enum {
    EV_RX = 1,
    EV_TX = 2,
    EV_TXEXC = 4,
    EV_ALLOC = 8,
    EV_CMD = 16,
    EV_INFO = 128,
    EV_INFDROP = 0x2000
};
static const uint8_t ap_bssid[6] = {0x02, 0x42, 0x45, 0x33, 0x30, 0x30};
static const uint8_t ap_ssid[] = "BE300";

static uint16_t rd16(const uint8_t *p)
{
    return p[0] | ((uint16_t)p[1] << 8);
}
static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static xi825_record_t *record(xi825_t *s, uint16_t id)
{
    for (unsigned i = 0; i < XI825_RECORDS; ++i)
        if (s->records[i].id == id)
            return &s->records[i];
    return NULL;
}

static void add_record(xi825_t *s, uint16_t id, const void *data, size_t len)
{
    xi825_record_t *r = record(s, id);
    if (!r) {
        for (unsigned i = 0; i < XI825_RECORDS; ++i)
            if (!s->records[i].id) {
                r = &s->records[i];
                break;
            }
    }
    if (!r || len > sizeof(r->data) - 4)
        return;
    memset(r, 0, sizeof(*r));
    r->id = id;
    r->length = (uint16_t)(4 + ((len + 1) & ~1u));
    wr16(r->data, (uint16_t)(1 + (len + 1) / 2));
    wr16(r->data + 2, id);
    if (len)
        memcpy(r->data + 4, data, len);
    memcpy(r->committed, r->data, sizeof(r->data));
}

static void scalar(xi825_t *s, uint16_t id, uint16_t value)
{
    uint8_t b[2];
    wr16(b, value);
    add_record(s, id, b, sizeof(b));
}

static xi825_buffer_t *buffer(xi825_t *s, uint16_t id)
{
    for (unsigned i = 0; i < XI825_BUFFERS; ++i)
        if (s->buffers[i].used && s->buffers[i].id == id)
            return &s->buffers[i];
    return NULL;
}

static xi825_buffer_t *allocate(xi825_t *s, unsigned length)
{
    if (length > XI825_BUFFER_SIZE || !length)
        return NULL;
    for (unsigned i = 0; i < XI825_BUFFERS; ++i) {
        xi825_buffer_t *b = &s->buffers[i];
        if (b->used)
            continue;
        memset(b, 0, sizeof(*b));
        b->used = true;
        b->length = (uint16_t)length;
        b->id = s->next_fid++;
        /* Keep FIDs out of the RID space, including after many receives. */
        if (s->next_fid >= 0x8000)
            s->next_fid = 0x100;
        while (buffer(s, s->next_fid))
            ++s->next_fid;
        return b;
    }
    return NULL;
}

static void release(xi825_t *s, uint16_t id)
{
    xi825_buffer_t *b = buffer(s, id);
    if (b)
        b->used = false;
}

static bool info(xi825_t *s, uint16_t type, const void *data, size_t len)
{
    if (s->info_count == 8) {
        s->regs[EVSTAT] |= EV_INFDROP;
        return false;
    }
    xi825_buffer_t *b = allocate(s, (unsigned)(4 + len));
    if (!b) {
        s->regs[EVSTAT] |= EV_INFDROP;
        return false;
    }
    wr16(b->data, (uint16_t)(1 + len / 2));
    wr16(b->data + 2, type);
    if (len)
        memcpy(b->data + 4, data, len);
    s->info_queue[(s->info_head + s->info_count++) % 8] = b->id;
    s->regs[INFOFID] = s->info_queue[s->info_head];
    s->regs[EVSTAT] |= EV_INFO;
    return true;
}

static bool can_associate(xi825_t *s)
{
    xi825_record_t *ssid = record(s, 0xfc02);
    unsigned n = rd16(ssid->committed + 4);
    if (!s->security.valid || s->security.mode > 1) return false;
    unsigned flags=rd16(record(s,0xfc28)->committed+4);
    if (!!(flags&1) != (s->security.mode==1)) return false;
    if (s->security.mode==1) {
        unsigned index=rd16(record(s,0xfc23)->committed+4);
        xi825_record_t *key=index<4?record(s,0xfc24+index):NULL;
        if(!key || !wifi_security_wep(&s->security,index,key->committed+4,
            s->security.secret_len))return false;
        unsigned size=s->security.secret_len==5?6:14;
        for(unsigned i=0;i<4;i++) {
            xi825_record_t *slot=record(s,0xfc24+i);
            if(!slot || slot->length != 4+size)return false;
        }
    }
    return s->enabled && rd16(record(s, 0xfc00)->committed + 4) == 1 &&
           (n == 0 || (n == sizeof(ap_ssid)-1 && !memcmp(ssid->committed+6,ap_ssid,n)));
}

void xi825_tick(xi825_t *s)
{
    bool connected = can_associate(s);
    s->security.authorized = connected;
    if (s->link_pending || connected != s->associated) {
        uint8_t b[2];
        wr16(b, connected ? 1 : 2);
        if (info(s, 0xf200, b, sizeof(b))) {
            s->associated = connected;
            s->link_pending = false;
        }
    }
}

static void scan(xi825_t *s)
{
    /* HFA384x SCANRESULTS: reserved/reason + one 62-byte BSS entry. */
    uint8_t b[66] = {0};
    wr16(b + 2, 1);
    wr16(b + 4, 3);
    wr16(b + 6, 40);
    wr16(b + 8, 100);
    memcpy(b + 10, ap_bssid, 6);
    wr16(b + 16, 100);
    wr16(b + 18, s->security.mode ? 0x11 : 1);
    wr16(b + 20, sizeof(ap_ssid) - 1);
    memcpy(b + 22, ap_ssid, sizeof(ap_ssid) - 1);
    b[54] = 0x82;
    b[55] = 0x84;
    b[56] = 0x0b;
    b[57] = 0x16;
    wr16(b + 64, 22);
    info(s, 0xf101, b, sizeof(b));
}

static void refresh_record(xi825_t *s, uint16_t id)
{
    uint8_t b[34] = {0};
    switch (id) {
    case 0xfc01:
        add_record(s, id, s->mac, 6);
        break;
    case 0xfd40:
        scalar(s, id, s->associated ? 1 : (s->enabled ? 2 : 0));
        break;
    case 0xfd41:
        if (s->associated) {
            wr16(b, sizeof(ap_ssid) - 1);
            memcpy(b + 2, ap_ssid, sizeof(ap_ssid) - 1);
        }
        add_record(s, id, b, sizeof(b));
        break;
    case 0xfd42:
        add_record(s, id, s->associated ? ap_bssid : b, 6);
        break;
    case 0xfd43:
        if (s->associated) {
            wr16(b, 60);
            wr16(b + 2, 100);
            wr16(b + 4, 40);
        }
        add_record(s, id, b, 6);
        break;
    /* nb825be.dll 0x10002ed0..0x10002f44: 1/2/4/8 select
     * 1/2/5.5/11 Mbps respectively (not the 802.11 half-Mbps value 22). */
    case 0xfd44:
        scalar(s, id, s->associated ? 8 : 0);
        break;
    case 0xfdc1:
        scalar(s, id, s->associated ? 3 : 0);
        break;
    default:
        break;
    }
}

void xi825_reset(xi825_t *s)
{
    /* INIT resets MAC state and allocations; host callback and EEPROM MAC persist. */
    uint8_t cis[124];
    size_t cis_length = 0;
    xi825_record_t *cis_record = record(s, 0xfd13);
    if (cis_record) {
        cis_length = cis_record->length - 4;
        memcpy(cis, cis_record->committed + 4, cis_length);
    }
    uint8_t mac[6];
    memcpy(mac, s->mac, 6);
    void *opaque = s->opaque;
    void (*tx)(void *, const uint8_t *, size_t) = s->transmit;
    wifi_security_t security=s->security; wifi_security_reset(&security);
    memset(s, 0, sizeof(*s));
    s->security=security;
    memcpy(s->mac, mac, 6);
    s->opaque = opaque;
    s->transmit = tx;
    s->next_fid = 0x100;
    static const struct {
        uint16_t id, value;
    } defaults[] = {
        {0xfc00, 1}, {0xfc03, 3}, {0xfc06, 1},    {0xfc07, 2304},  {0xfc09, 0},
        {0xfc23, 0}, {0xfc28, 0}, {0xfc2a, 1},    {0xfc2d, 1},     {0xfc33, 100},
        {0xfc41, 0}, {0xfc81, 0}, {0xfc82, 2346}, {0xfc83, 2347},  {0xfc84, 15},
        {0xfc85, 0}, {0xfcb0, 1}, {0xfcb3, 3},    {0xfcb4, 15},    {0xfce0, 0},
        {0xfd4f, 0}, {0xfd11, 1}, {0xfdc0, 2},    {0xfd10, 0x7ff},
    };
    for (unsigned i = 0; i < sizeof(defaults) / sizeof(defaults[0]); ++i)
        scalar(s, defaults[i].id, defaults[i].value);
    uint8_t b[120] = {0};
    for(unsigned i=0;i<4;i++)add_record(s,0xfc24+i,b,14);
    add_record(s, 0xfc01, mac, 6);
    add_record(s, 0xfc02, b, 34);
    add_record(s, 0xfc04, b, 34);
    add_record(s, 0xfc0e, b, 34);
    add_record(s, 0xfc80, b, 0);
    add_record(s, 0xfce1, b, 4);
    add_record(s, 0xfce2, b, 8);
    /* PRISM component identities: PRI 0x15, STA 0x1f (hostap_common.h).
     * These identify the emulated station implementation, not a dumped ROM. */
    wr16(b, 0x15);
    wr16(b + 2, 1);
    wr16(b + 4, 1);
    wr16(b + 6, 1);
    add_record(s, 0xfd02, b, 8);
    wr16(b, 0x1f);
    wr16(b + 4, 1);
    wr16(b + 6, 7);
    add_record(s, 0xfd20, b, 8);
    memset(b, 0, sizeof(b));
    wr16(b, 0x8000);
    wr16(b + 4, 4096);
    add_record(s, 0xfd01, b, 6);
    memset(b, 0, sizeof(b));
    wr16(b, 1);
    wr16(b + 2, 3);
    wr16(b + 4, 1);
    wr16(b + 6, 1);
    wr16(b + 8, 1);
    add_record(s, 0xfd03, b, 10);
    add_record(s, 0xfd04, b, 10);
    add_record(s, 0xfd21, b, 10);
    scalar(s, 0xfd40, 0);
    scalar(s, 0xfd44, 0);
    scalar(s, 0xfdc1, 0);
    memset(b, 0, sizeof(b));
    add_record(s, 0xfd41, b, 34);
    add_record(s, 0xfd42, b, 6);
    add_record(s, 0xfd43, b, 6);
    if (cis_length)
        xi825_set_cis(s, cis, cis_length);
}

void xi825_init(xi825_t *s, const uint8_t mac[6])
{
    memset(s, 0, sizeof(*s));
    memcpy(s->mac, mac, 6);
    wifi_security_init(&s->security,mac);
    xi825_reset(s);
}

/* Attach supplies the same CIS to the attribute window and the firmware RID.
 * nb825be.dll 0x1000ea6c checks MANFID 0xd601 in this record. */
void xi825_set_cis(xi825_t *s, const uint8_t *cis, size_t length)
{
    add_record(s, 0xfd13, cis, length);
}

static uint8_t *bap_memory(xi825_t *s, unsigned port, unsigned *capacity)
{
    uint16_t id = s->regs[SELECT0 + port];
    xi825_record_t *r = record(s, id);
    if (r) {
        *capacity = sizeof(r->data);
        return r->data;
    }
    xi825_buffer_t *b = buffer(s, id);
    if (b) {
        *capacity = b->length;
        return b->data;
    }
    *capacity = 0;
    return NULL;
}

static uint16_t bap_data(xi825_t *s, unsigned port, unsigned size, bool write,
                         uint16_t value)
{
    unsigned capacity;
    uint8_t *p = bap_memory(s, port, &capacity);
    uint16_t *offset = &s->regs[OFFSET0 + port];
    unsigned pos = *offset & 0x3fff;
    /* BAP error is sticky until a new seek. No wrap into another FID. */
    if ((*offset & 0x4000) || !p || pos + size > capacity) {
        *offset |= 0x4000;
        return 0xffff;
    }
    uint16_t out = p[pos];
    if (size == 2)
        out |= (uint16_t)p[pos + 1] << 8;
    if (write) {
        p[pos] = (uint8_t)value;
        if (size == 2)
            p[pos + 1] = (uint8_t)(value >> 8);
    }
    *offset = (uint16_t)(pos + size);
    return out;
}

static bool transmit(xi825_t *s, xi825_buffer_t *b)
{
    uint8_t eth[1600];
    if (!s->associated || !b || b->length < 60)
        return false;
    unsigned n = rd16(b->data + 44);
    /* The XI-825 driver uses firmware Ethernet encapsulation and supplies
     * the network-order length at 0x3a (0x10007fa8), leaving data_len zero. */
    if (!n)
        n = ((unsigned)b->data[58] << 8) | b->data[59];
    if (n > sizeof(eth) - 14 || 60 + n > b->length)
        return false;
    /* Firmware 802.3 header at 0x2e; RFC1042/bridge tunnel SNAP at 0x3c.
     * nb825be.dll 0x10002c98/0x10002cc4 constructs exactly this layout. */
    const uint8_t *p = b->data + 60;
    memcpy(eth, b->data + 46, 12);
    if (n >= 8 && p[0] == 0xaa && p[1] == 0xaa && p[2] == 3 && p[3] == 0 && p[4] == 0 &&
        (p[5] == 0 || p[5] == 0xf8)) {
        memcpy(eth + 12, p + 6, n - 6);
        n -= 8;
    } else {
        memcpy(eth + 12, b->data + 58, 2);
        memcpy(eth + 14, p, n);
    }
    if (s->transmit)
        s->transmit(s->opaque, eth, n + 14);
    return true;
}

static void command(xi825_t *s, uint16_t value)
{
    uint16_t op = value & 0x3f, param = s->regs[PARAM0], result = 0;
    xi825_buffer_t *b;
    xi825_record_t *r;
    s->regs[CMD] = value & 0x7fff;
    s->regs[RESP0] = s->regs[RESP0 + 1] = s->regs[RESP0 + 2] = 0;
    switch (op) {
    case 0:
        xi825_reset(s);
        break;
    case 1: /* ENABLE port 0 */
        if (value & 0x700) {
            result = 1;
            break;
        }
        s->enabled = true;
        s->link_pending = true;
        break;
    case 2: /* DISABLE */
        s->enabled = false;
        s->link_pending = s->associated;
        break;
    case 0x0a: /* ALLOC */
        if (s->regs[EVSTAT] & EV_ALLOC) {
            result = 5;
            break;
        }
        b = allocate(s, param);
        if (!b)
            result = 5;
        else {
            s->regs[ALLOCFID] = s->regs[RESP0] = b->id;
            s->regs[EVSTAT] |= EV_ALLOC;
        }
        break;
    case 0x0b: /* TX, bit 8 returns the FID through the reclaim event. */
        b = buffer(s, param);
        if (!b || (s->regs[EVSTAT] & EV_ALLOC)) {
            result = 5;
            break;
        }
        /* Firmware only reports TX/TXEXC when requested in tx_control.
         * nb825be.dll uses Ethernet mode (control=0), reclaim only. */
        if (transmit(s, b)) {
            wr16(b->data, 0);
            if (rd16(b->data + 12) & 2)
                s->regs[EVSTAT] |= EV_TX;
        } else {
            wr16(b->data, 1);
            if (rd16(b->data + 12) & 4)
                s->regs[EVSTAT] |= EV_TXEXC;
        }
        s->regs[TXFID] = param;
        if (value & 0x100) {
            s->regs[ALLOCFID] = param;
            s->regs[EVSTAT] |= EV_ALLOC;
        } else
            release(s, param);
        break;
    case 0x11: /* INQUIRE */
        if (param == 0xf100) {
            uint8_t tallies[42] = {0};
            info(s, param, tallies, sizeof(tallies));
        } else if (param == 0xf101)
            scan(s);
        else if (param == 0xf200)
            s->link_pending = true;
        else
            result = 1;
        break;
    case 0x21: /* ACCESS: BAP writes stage the LTV; ACCESS|WRITE commits. */
        r = record(s, param);
        if (!r) {
            result = 1;
            break;
        }
        if (value & 0x100) {
            unsigned words = rd16(r->data);
            if ((param & 0xff00) != 0xfc00 || !words || words > 63 ||
                rd16(r->data + 2) != param)
                result = 1;
            else {
                r->length = (uint16_t)(2 + words * 2);
                memcpy(r->committed, r->data, sizeof(r->data));
                if (param == 0xfc01 && words == 4)
                    memcpy(s->mac, r->data + 4, 6);
                if (param == 0xfce1)
                    scan(s);
            }
        } else {
            refresh_record(s, param);
            memcpy(r->data, r->committed, sizeof(r->data));
        }
        break;
    default:
        result = 1;
        break;
    }
    s->regs[STATUS] = (uint16_t)(op | (result << 8));
    s->regs[EVSTAT] |= EV_CMD;
}

bool xi825_irq_pending(const xi825_t *s)
{
    return (s->regs[EVSTAT] & s->regs[INTEN]) != 0;
}

static void ack(xi825_t *s, uint16_t value)
{
    s->regs[EVSTAT] &= (uint16_t)~value;
    if ((value & EV_INFO) && s->info_count) {
        release(s, s->info_queue[s->info_head]);
        s->info_head = (s->info_head + 1) % 8;
        --s->info_count;
    }
    if (s->info_count) {
        s->regs[INFOFID] = s->info_queue[s->info_head];
        s->regs[EVSTAT] |= EV_INFO;
    }
    if ((value & EV_RX) && s->rx_count) {
        release(s, s->rx_queue[s->rx_head]);
        s->rx_head = (s->rx_head + 1) % 8;
        --s->rx_count;
    }
    if (s->rx_count) {
        s->regs[RXFID] = s->rx_queue[s->rx_head];
        s->regs[EVSTAT] |= EV_RX;
    }
}

uint64_t xi825_read(xi825_t *s, unsigned offset, unsigned size)
{
    if (offset >= 64 || !size || size > 8 || offset + size > 64)
        return UINT64_MAX;
    if ((offset == DATA0 * 2 || offset == DATA1 * 2) && size <= 2)
        return bap_data(s, offset == DATA1 * 2, size, false, 0);
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i)
        value |=
            (uint64_t)((s->regs[(offset + i) / 2] >> (((offset + i) & 1) * 8)) & 255)
            << (i * 8);
    return value;
}

void xi825_write(xi825_t *s, unsigned offset, unsigned size, uint64_t value)
{
    if (offset >= 64 || !size || size > 8 || offset + size > 64)
        return;
    if ((offset == DATA0 * 2 || offset == DATA1 * 2) && size <= 2) {
        bap_data(s, offset == DATA1 * 2, size, true, (uint16_t)value);
        return;
    }
    if (size > 2) {
        for (unsigned i = 0; i < size; i += 2)
            xi825_write(s, offset + i, size - i >= 2 ? 2 : 1, value >> (8 * i));
        return;
    }
    unsigned reg = offset / 2;
    uint16_t v = (uint16_t)value;
    if (size == 1) {
        unsigned shift = (offset & 1) * 8;
        v = (uint16_t)((s->regs[reg] & ~(255u << shift)) | ((value & 255u) << shift));
    }
    if (reg == CMD) {
        command(s, v);
        return;
    }
    if (reg == EVACK) {
        ack(s, v);
        return;
    }
    if (reg == OFFSET0 || reg == OFFSET0 + 1) {
        unsigned capacity;
        uint8_t *p = bap_memory(s, reg - OFFSET0, &capacity);
        /* OFFSET selects a word boundary. nb825be.dll 0x10008238..0x100082bc
         * seeks to an odd packet end, then combines two words to verify its
         * trailing sentinel. Bit 0 is ignored, rather than causing ERR. */
        s->regs[reg] = (uint16_t)(v & 0xffe);
        if (!p || (v & 0xffe) >= capacity)
            s->regs[reg] |= 0x4000;
        return;
    }
    if ((reg >= PARAM0 && reg <= 3) || reg == INTEN || reg == SELECT0 ||
        reg == SELECT0 + 1 || (reg >= 20 && reg <= 22))
        s->regs[reg] = v;
}

bool xi825_receive(xi825_t *s, const uint8_t *eth, size_t length)
{
    if (!s->associated || !eth || length < 14 || length > 1514 || s->rx_count == 8)
        return false;
    bool multicast = (eth[0] & 1) != 0;
    if (!multicast && memcmp(eth, s->mac, 6) && !rd16(record(s, 0xfc85)->committed + 4))
        return false;
    unsigned n = (unsigned)length - 14;
    xi825_buffer_t *b = allocate(s, 60 + 8 + n);
    if (!b)
        return false;
    wr16(b->data, 0x2000); /* RFC1042 message type */
    b->data[6] = 40;
    b->data[7] = 100;
    b->data[8] = 110;
    wr16(b->data + 14, 0x0208); /* data from DS */
    memcpy(b->data + 18, eth, 6);
    memcpy(b->data + 24, ap_bssid, 6);
    memcpy(b->data + 30, eth + 6, 6);
    wr16(b->data + 44, (uint16_t)(n + 8));
    memcpy(b->data + 46, eth, 12);
    b->data[58] = (uint8_t)((n + 8) >> 8);
    b->data[59] = (uint8_t)(n + 8);
    static const uint8_t snap[6] = {0xaa, 0xaa, 3, 0, 0, 0};
    memcpy(b->data + 60, snap, 6);
    memcpy(b->data + 66, eth + 12, length - 12);
    s->rx_queue[(s->rx_head + s->rx_count++) % 8] = b->id;
    s->regs[RXFID] = s->rx_queue[s->rx_head];
    s->regs[EVSTAT] |= EV_RX;
    return true;
}
