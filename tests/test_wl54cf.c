/* Exercise both guest I/O and the card-facing Ethernet edge. No guest assets. */
#include "hw/cf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static uint8_t tx[1600];
static size_t tx_len;
static uint8_t mac[6] = {0x02, 0x60, 0xb3, 0, 0, 1};
/* The CF adapter uses these network entrypoints. Tests capture transmitted
 * frames; the production backend is exercised by the local guest smoke test. */
void net_generate_unique_mac(struct machine *m, unsigned char *p) { memcpy(p, mac, 6); }
void net_add_nic(struct net *n, struct nic_data *nic) {}
int net_ethernet_rx_avail(struct net *n, struct nic_data *nic) { return 0; }
int net_ethernet_rx(struct net *n, struct nic_data *nic, unsigned char **p, int *len) { return 0; }
void net_ethernet_tx(struct net *n, struct nic_data *nic, unsigned char *p, int len)
{
    CHECK(len <= sizeof(tx));
    memcpy(tx, p, len);
    tx_len = len;
}
int net_ether_eq(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, 6); }
int net_ether_broadcast(const uint8_t *a)
{
    static const uint8_t b[6] = {255, 255, 255, 255, 255, 255};
    return !memcmp(a, b, 6);
}
int net_ether_multicast(const uint8_t *a) { return (a[0] & 1) != 0; }

static void w(wl54cf_t *s, unsigned off, uint16_t v) { wl54cf_write(s, off, 2, v); }
static uint16_t r(wl54cf_t *s, unsigned off) { return (uint16_t)wl54cf_read(s, off, 2); }
static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}
static uint16_t get16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static void fifo(wl54cf_t *s, unsigned port, const uint8_t *p, unsigned n)
{
    unsigned i = 0;
    for (; i + 1 < n; i += 2)
        w(s, port, get16(p + i));
    if (i < n)
        wl54cf_write(s, port, 1, p[i]);
}
static void host_signal(wl54cf_t *s, uint16_t bits)
{
    w(s, 0, bits);
    w(s, 2, bits);
}
static void block(wl54cf_t *s, const uint8_t *p, unsigned n)
{
    w(s, 0x18, n);
    fifo(s, 0x1a, p, n);
    host_signal(s, 4);
}
/* Structurally valid firmware records, not executable guest/firmware assets. */
static void boot(wl54cf_t *s)
{
    uint8_t b[256] = {0};
    block(s, b, sizeof(b));
    block(s, b, 0);
    CHECK(r(s, 0x28) == 16);
    b[0] = 1;
    b[8] = 2;
    block(s, b, 16);
    CHECK(r(s, 0x28) == 2);
    block(s, b, 2);
    CHECK(r(s, 0x28) == 16);
    memset(b, 0, 16);
    b[0] = 4;
    block(s, b, 16);
    CHECK(wl54cf_read(s, 0x3f, 1) == 0x5a);
    CHECK(r(s, 0x20) == 5);
}
static unsigned command(wl54cf_t *s, uint8_t *b, unsigned n, uint16_t op, uint16_t result)
{
    put16(b, op);
    put16(b + 2, n);
    put16(b + 4, 0x1234);
    put16(b + 6, 0);
    block(s, b, n);
    CHECK(r(s, 0x22) & 8);
    unsigned count = r(s, 0x30);
    CHECK(count >= 16 && count < 2048);
    memset(b, 0, 2048);
    for (unsigned i = 0; i < count; i++)
        b[i] = (uint8_t)wl54cf_read(s, 0x12, 1);
    CHECK(get16(b) == (op | 0x8000));
    CHECK(get16(b + 4) == 0x1234);
    CHECK(get16(b + 6) == result);
    CHECK(get16(b + 2) == count - 8);
    w(s, 0x22, 8);
    CHECK(!(r(s, 0x22) & 8));
    return count - 8;
}
static const uint8_t ap[6] = {2, 0x42, 0x45, 0x33, 0x30, 0x30};
static void associate(wl54cf_t *s, uint8_t b[2048], bool ok)
{
    memset(b, 0, 2048);
    memcpy(b + 8, ap, 6);
    b[14] = 1;
    put16(b + 23, 5);
    memcpy(b + 25, ok ? "BE300" : "WRONG", 5);
    put16(b + 30, 3);
    put16(b + 32, 1);
    b[34] = 3;
    command(s, b, 35, 0x50, 0);
    CHECK(get16(b + 10) == (ok ? 0 : 4));
    CHECK(s->associated == ok);
}
static void test_protocol(void)
{
    wl54cf_t *s = calloc(1, sizeof(*s));
    CHECK(s);
    uint8_t b[2048] = {0};
    wl54cf_init(s, mac);
    CHECK(wl54cf_read(s, 0x3f, 1) == 0);
    CHECK(!wl54cf_irq_pending(s));
    block(s, b, 0);
    CHECK(s->firmware_stage == 3);
    CHECK(r(s, 0x20) == 0);
    wl54cf_reset(s);
    boot(s);
    /* Firmware must reject truncated records and not publish readiness. */
    wl54cf_reset(s);
    block(s, b, 16);
    block(s, b, 0);
    b[0] = 1;
    b[8] = 255;
    block(s, b, 16);
    CHECK(s->firmware_stage == 3);
    wl54cf_reset(s);
    boot(s);
    memset(b, 0, sizeof(b));
    CHECK(command(s, b, 46, 3, 0) == 46);
    CHECK(!memcmp(b + 16, mac, 6));
    CHECK(command(s, b, 8, 0x7777, 2) == 8);
    memset(b, 0, sizeof(b));
    b[8] = 3;
    CHECK(command(s, b, 15, 6, 0) > 11);
    CHECK(b[10] == 1);
    CHECK(!memcmp(b + 13, ap, 6));
    memset(b, 0, sizeof(b));
    b[8] = 3;
    put16(b + 15, 0);
    put16(b + 17, 5);
    memcpy(b + 19, "WRONG", 5);
    CHECK(command(s, b, 24, 6, 0) == 11);
    CHECK(b[10] == 0);
    memset(b, 0, sizeof(b));
    b[8] = 3;
    put16(b + 17, 2000);
    command(s, b, 19, 6, 2); /* truncated TLV */
    associate(s, b, false);
    memset(b, 0, sizeof(b));
    memcpy(b + 8, ap, 6);
    command(s, b, 25, 0x11, 0);
    associate(s, b, false);
    associate(s, b, true);
    memset(b, 0, sizeof(b));
    put16(b + 8, 0x43);
    command(s, b, 12, 0x28, 0);
    memset(b, 0, sizeof(b));
    memcpy(b, mac, 6);
    b[12] = 8;
    b[13] = 0;
    b[60] = 0x51;
    /* RX publishes an 802.3/SNAP frame at the RxPD offset. */
    CHECK(wl54cf_receive(s, b, 61));
    CHECK(!wl54cf_irq_pending(s));
    w(s, 4, 0);
    CHECK(wl54cf_irq_pending(s));
    CHECK(r(s, 0x24) == 89);
    uint8_t frame[2048];
    unsigned n = r(s, 0x24);
    for (unsigned i = 0; i < n; i++)
        frame[i] = (uint8_t)wl54cf_read(s, 0x10, 1);
    CHECK(frame[8] == 20 && get16(frame + 4) == 69);
    CHECK(!memcmp(frame + 20, b, 12));
    CHECK(!memcmp(frame + 34, "\xaa\xaa\x03\0\0\0", 6));
    CHECK(frame[88] == 0x51);
    w(s, 0x22, 2);
    CHECK(wl54cf_irq_pending(s)); /* unread ownership */
    host_signal(s, 2);
    w(s, 0x22, 2);
    CHECK(!wl54cf_irq_pending(s));
    CHECK(!wl54cf_receive(s, b, 13));
    CHECK(!wl54cf_receive(s, b, 1515));
    for (unsigned i = 0; i < 8; i++)
        CHECK(wl54cf_receive(s, b, 61));
    CHECK(!wl54cf_receive(s, b, 61));
    /* Pending command, packets and association all disappear on reset. */
    wl54cf_reset(s);
    CHECK(!s->rx_count && !s->associated && !s->cause);
    CHECK(wl54cf_read(s, 0x10, 2) == UINT64_MAX);
    /* Overlong FIFO writes stay bounded, fail commands rather than wrapping. */
    boot(s);
    w(s, 0x18, 65535);
    for (unsigned i = 0; i < 5000; i++)
        w(s, 0x1a, 0xffff);
    host_signal(s, 4);
    CHECK(!(r(s, 0x22) & 8));
    free(s);
}
static void test_socket(void)
{
    cf_state_t *s = calloc(1, sizeof(*s));
    CHECK(s);
    cf_init(s);
    CHECK(!cf_attach_wl54cf(s, NULL, (struct net *)1, mac));
    CHECK(cf_is_network(s) && !cf_is_ne2000(s));
    bool identity = false, config = false;
    for (unsigned i = 0; i < sizeof(s->cis) && s->cis[i] != 255;) {
        unsigned tag = s->cis[i], n = s->cis[i + 1];
        CHECK(i + 2 + n <= sizeof(s->cis));
        if (tag == 0x20) {
            CHECK(n == 4 && get16(s->cis + i + 2) == 0x2df && get16(s->cis + i + 4) == 0x8103);
            identity = true;
        }
        if (tag == 0x1a) {
            CHECK(get16(s->cis + i + 4) == 0x200);
            config = true;
        }
        i += n + 2;
    }
    CHECK(identity && config);
    CHECK(cf_window_read(s, 0x31c, 1) == UINT64_MAX);
    cf_cis_write(s, 0x200, 1, 1);
    CHECK(cf_cis_read(s, 0x200, 1) == 1);
    CHECK(cf_window_read(s, 0x31c, 1) == 0x12);
    boot(&s->wl54cf);
    uint8_t b[2048] = {0};
    memcpy(b + 8, ap, 6);
    command(&s->wl54cf, b, 25, 0x11, 0);
    associate(&s->wl54cf, b, true);
    memset(b, 0, sizeof(b));
    put16(b + 8, 0x43);
    command(&s->wl54cf, b, 12, 0x28, 0);
    cf_wl54cf_tick(s);
    memset(b, 0, sizeof(b));
    b[8] = 24;
    put16(b + 12, 61);
    memcpy(b + 24, mac, 6);
    b[24 + 60] = 0x6d;
    w(&s->wl54cf, 0x14, 85);
    fifo(&s->wl54cf, 0x16, b, 85);
    host_signal(&s->wl54cf, 1);
    CHECK(tx_len == 61 && tx[60] == 0x6d);
    cf_clear_irq(s);
    cf_consume_state_change(s);
    w(&s->wl54cf, 4, 0);
    CHECK(cf_giu_source_bits(s) & 1);
    w(&s->wl54cf, 0x22, 1);
    CHECK(!cf_giu_source_bits(s));
    cf_cis_write(s, 0x200, 1, 0x80);
    CHECK(!s->wl54cf.associated);
    CHECK(cf_window_read(s, 0x31c, 1) == UINT64_MAX);
    cf_cis_write(s, 0x200, 1, 1);
    CHECK(wl54cf_read(&s->wl54cf, 0x3f, 1) == 0);
    cf_destroy(s);
    free(s);
}
int main(void)
{
    test_protocol();
    test_socket();
    puts("wl54cf: OK");
    return 0;
}
