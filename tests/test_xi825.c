/* Exercise both guest I/O and the card-facing Ethernet edge. No guest assets. */
#include "hw/cf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) {                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                    \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)
static uint8_t tx[1600];
static size_t tx_len;
static uint8_t mac[6] = {0x02, 0x60, 0xb3, 0, 0, 1};
/* The CF adapter uses these network entrypoints. Tests capture transmitted
 * frames; the production backend is exercised by the local guest smoke test. */
void net_generate_unique_mac(struct machine *m, unsigned char *p)
{
    memcpy(p, mac, 6);
}
void net_add_nic(struct net *n, struct nic_data *nic) {}
int net_ethernet_rx_avail(struct net *n, struct nic_data *nic)
{
    return 0;
}
int net_ethernet_rx(struct net *n, struct nic_data *nic, unsigned char **p, int *len)
{
    return 0;
}
void net_ethernet_tx(struct net *n, struct nic_data *nic, unsigned char *p, int len)
{
    CHECK(len <= sizeof(tx));
    memcpy(tx, p, len);
    tx_len = len;
}
int net_ether_eq(const uint8_t *a, const uint8_t *b)
{
    return !memcmp(a, b, 6);
}
int net_ether_broadcast(const uint8_t *a)
{
    static const uint8_t b[6] = {255, 255, 255, 255, 255, 255};
    return !memcmp(a, b, 6);
}
int net_ether_multicast(const uint8_t *a)
{
    return (a[0] & 1) != 0;
}

static void w(xi825_t *s, unsigned off, uint16_t v)
{
    xi825_write(s, off, 2, v);
}
static uint16_t r(xi825_t *s, unsigned off)
{
    return (uint16_t)xi825_read(s, off, 2);
}
static void cmd(xi825_t *s, uint16_t op, uint16_t arg)
{
    w(s, 2, arg);
    w(s, 0, op);
    CHECK(r(s, 0x30) & 16);
    CHECK((r(s, 8) & 0x7f00) == 0);
    w(s, 0x34, 16);
}
static void seek(xi825_t *s, unsigned port, uint16_t id, uint16_t off)
{
    w(s, 0x18 + 2 * port, id);
    w(s, 0x1c + 2 * port, off);
    CHECK(!(r(s, 0x1c + 2 * port) & 0xc000));
}
static void ltv(xi825_t *s, uint16_t id, const uint8_t *data, size_t n)
{
    seek(s, 0, id, 0);
    w(s, 0x36, (uint16_t)(1 + (n + 1) / 2));
    w(s, 0x36, id);
    for (size_t i = 0; i < n; i += 2)
        w(s, 0x36, data[i] | (i + 1 < n ? (uint16_t)data[i + 1] << 8 : 0));
    cmd(s, 0x121, id);
}
static uint16_t get_scalar(xi825_t *s, uint16_t id)
{
    cmd(s, 0x21, id);
    seek(s, 1, id, 4);
    return r(s, 0x38);
}

static void test_cis(void)
{
    cf_state_t *c = calloc(1, sizeof(*c));
    CHECK(c);
    cf_init(c);
    CHECK(cf_attach_xi825(c, NULL, NULL, mac) == 0);
    CHECK(cf_is_network(c));
    bool identity = false, config = false, network = false;
    for (unsigned p = 0; p < sizeof(c->cis) && c->cis[p] != 255;) {
        unsigned tag = c->cis[p], n = c->cis[p + 1];
        CHECK(p + 2 + n <= sizeof(c->cis));
        if (tag == 0x15) {
            CHECK(!strcmp((char *)c->cis + p + 4, "Z-Com"));
            CHECK(!strcmp((char *)c->cis + p + 10, "LANEscape/XI-825"));
            identity = true;
            CHECK(c->cis[p + 1 + n] == 255);
        }
        if (tag == 0x1a)
            config = true;
        if (tag == 0x21) {
            CHECK(c->cis[p + 2] == 6);
            network = true;
        }
        p += 2 + n;
    }
    CHECK(identity && config && network);
    cf_cis_write(c, 0x3e0, 1, 0x41);
    CHECK(cf_cis_read(c, 0x3e0, 1) == 0x41);
    cf_window_write(c, 0xc328, 2, 0x5a5a);
    CHECK(cf_window_read(c, 0xc328, 2) == 0x5a5a);
    cf_window_write(c, 0xc300, 2, 0);
    cf_window_write(c, 0xc332, 2, 16);
    CHECK(cf_giu_source_bits(c));
    cf_consume_state_change(c);
    cf_clear_irq(c);
    CHECK(cf_giu_source_bits(c));
    cf_window_write(c, 0xc334, 2, 16);
    CHECK(!cf_giu_source_bits(c));
    cf_cis_write(c, 0x3e0, 1, 0x80);
    CHECK(cf_window_read(c, 0xc300, 2) == UINT64_MAX);
    cf_cis_write(c, 0x3e0, 1, 0x41);
    CHECK(!c->xi825.enabled);
    cf_destroy(c);
    free(c);
}

static void test_commands(void)
{
    xi825_t s;
    xi825_init(&s, mac);
    w(&s, 0x32, 16);
    cmd(&s, 0, 0); /* INIT clears interrupt enable */
    CHECK(!xi825_irq_pending(&s));
    w(&s, 0x32, 16);
    w(&s, 0, 0x3f);
    CHECK(r(&s, 8) & 0x7f00);
    CHECK(xi825_irq_pending(&s));
    w(&s, 0x34, 16);
    CHECK(!xi825_irq_pending(&s));
    w(&s, 2, 0xdead);
    w(&s, 0, 0x21);
    CHECK(r(&s, 8) & 0x7f00);
    w(&s, 0x34, 16);
    CHECK(get_scalar(&s, 0xfdc0) == 2);
    cmd(&s, 0xa, 1600);
    uint16_t id = r(&s, 0x22);
    CHECK(r(&s, 0x30) & 8);
    w(&s, 0x34, 8);
    seek(&s, 0, id, 0);
    w(&s, 0x36, 0x1234);
    seek(&s, 1, id, 0);
    CHECK(r(&s, 0x38) == 0x1234);
    seek(&s, 0, id, 1598);
    w(&s, 0x36, 0x5678);
    CHECK(r(&s, 0x36) == 0xffff);
    CHECK(r(&s, 0x1c) & 0x4000);
    seek(&s, 0, id, 0);
    CHECK(r(&s, 0x36) == 0x1234);
    w(&s, 0x1c, 1);
    CHECK(r(&s, 0x1c) == 0);
    /* Stock miniport's odd packet-end verification: seek rounds down,
     * and the sentinel straddles the next two port reads. */
    seek(&s, 0, id, 124);
    w(&s, 0x36, 0xabcc);
    w(&s, 0x36, 0x00dd);
    seek(&s, 1, id, 125);
    uint16_t low = r(&s, 0x38);
    uint16_t high = r(&s, 0x38);
    CHECK((uint16_t)((low >> 8) | (high << 8)) == 0xddab);
    CHECK(xi825_read(&s, 63, 2) == UINT64_MAX);
    cmd(&s, 0, 0);
    seek(&s, 0, 0xfd02, 0);
    CHECK(r(&s, 0x36) == 5);

    /* The 4 KiB allocation boundary must not wrap a BAP back to byte 0. */
    cmd(&s, 0xa, 4096);
    id = r(&s, 0x22);
    w(&s, 0x34, 8);
    seek(&s, 0, id, 4094);
    w(&s, 0x36, 0x1234);
    CHECK(r(&s, 0x36) == 0xffff);
    CHECK(r(&s, 0x1c) & 0x4000);
    seek(&s, 1, id, 0);
    xi825_write(&s, 0x38, 1, 0x56);
    CHECK(r(&s, 0x1e) == 1);
    seek(&s, 0, id, 0);
    CHECK(xi825_read(&s, 0x36, 1) == 0x56);
}

static void test_association(void)
{
    xi825_t s;
    xi825_init(&s, mac);
    cmd(&s, 1, 0);
    xi825_tick(&s);
    CHECK(s.associated);
    CHECK(get_scalar(&s, 0xfd44) == 8);
    w(&s, 0x32, 128);
    CHECK(xi825_irq_pending(&s));
    seek(&s, 0, r(&s, 0x10), 0);
    CHECK(r(&s, 0x36) == 2);
    CHECK(r(&s, 0x36) == 0xf200);
    CHECK(r(&s, 0x36) == 1);
    w(&s, 0x34, 128);
    CHECK(!xi825_irq_pending(&s));
    uint8_t ssid[34] = {4, 0, 'n', 'o', 'p', 'e'};
    /* Staging a record through BAP does not change the active station. */
    seek(&s, 0, 0xfc02, 4);
    w(&s, 0x36, 4);
    w(&s, 0x36, 'n' | ('o' << 8));
    w(&s, 0x36, 'p' | ('e' << 8));
    xi825_tick(&s);
    CHECK(s.associated);
    ltv(&s, 0xfc02, ssid, sizeof(ssid));
    xi825_tick(&s);
    CHECK(!s.associated);
    CHECK(get_scalar(&s, 0xfd44) == 0);
    w(&s, 0x34, 128);
    ssid[0] = 0;
    ltv(&s, 0xfc02, ssid, sizeof(ssid));
    xi825_tick(&s);
    CHECK(s.associated);
    uint8_t wep[2] = {1, 0};
    ltv(&s, 0xfc28, wep, 2);
    xi825_tick(&s);
    CHECK(!s.associated);
    wep[0] = 0;
    ltv(&s, 0xfc28, wep, 2);
    xi825_tick(&s);
    CHECK(s.associated);
    cmd(&s, 2, 0);
    xi825_tick(&s);
    CHECK(!s.associated);
    cmd(&s, 0x11, 0xf101);
    CHECK(r(&s, 0x30) & 128);
    while (s.info_count > 1)
        w(&s, 0x34, 128);
    seek(&s, 0, r(&s, 0x10), 2);
    CHECK(r(&s, 0x36) == 0xf101);
    w(&s, 0x34, 128);
}

static void test_packets(void)
{
    cf_state_t *c = calloc(1, sizeof(*c));
    CHECK(c);
    cf_init(c);
    CHECK(cf_attach_xi825(c, NULL, (struct net *)1, mac) == 0);
    cf_xi825_tick(c);
    xi825_t *s = &c->xi825;
    uint8_t eth[61] = {0};
    memcpy(eth, mac, 6);
    memset(eth + 6, 0x12, 6);
    eth[12] = 8;
    for (unsigned i = 14; i < sizeof(eth); ++i)
        eth[i] = (uint8_t)i;
    CHECK(!xi825_receive(s, eth, sizeof(eth)));
    cmd(s, 1, 0);
    cf_xi825_tick(c);
    CHECK(s->associated);
    cmd(s, 0xa, 1600);
    uint16_t fid = r(s, 0x22);
    w(s, 0x34, 8);
    uint8_t frame[128] = {0};
    frame[12] = 2;
    memcpy(frame + 46, eth, 12);
    unsigned n = sizeof(eth) - 14 + 8;
    frame[58] = (uint8_t)(n >> 8);
    frame[59] = (uint8_t)n;
    frame[60] = frame[61] = 0xaa;
    frame[62] = 3;
    memcpy(frame + 66, eth + 12, sizeof(eth) - 12);
    seek(s, 0, fid, 0);
    for (unsigned i = 0; i < sizeof(frame); i += 2)
        w(s, 0x36, frame[i] | ((uint16_t)frame[i + 1] << 8));
    tx_len = 0;
    cmd(s, 0x10b, fid);
    CHECK(tx_len == sizeof(eth));
    CHECK(!memcmp(tx, eth, sizeof(eth)));
    CHECK((r(s, 0x30) & 10) == 10);
    CHECK(r(s, 0x22) == fid);
    w(s, 0x34, 10);
    /* Driver mode: tx_control=0, only the reclaim event is requested.
     * Repeat to catch a stale TX-success event blocking later traffic. */
    seek(s, 0, fid, 12);
    w(s, 0x36, 0);
    for (unsigned i = 0; i < 3; ++i) {
        tx_len = 0;
        cmd(s, 0x10b, fid);
        CHECK(tx_len == sizeof(eth));
        CHECK((r(s, 0x30) & 14) == 8);
        w(s, 0x34, 8);
    }
    CHECK(xi825_receive(s, eth, sizeof(eth)));
    seek(s, 1, r(s, 0x20), 44);
    CHECK(r(s, 0x38) == n);
    seek(s, 0, r(s, 0x20), 46);
    for (unsigned i = 0; i < 12; i += 2)
        CHECK(r(s, 0x36) == (eth[i] | ((uint16_t)eth[i + 1] << 8)));
    w(s, 0x32, 1);
    CHECK(xi825_irq_pending(s));
    w(s, 0x34, 1);
    CHECK(!xi825_irq_pending(s));
    for (unsigned i = 0; i < 8; ++i)
        CHECK(xi825_receive(s, eth, sizeof(eth)));
    CHECK(!xi825_receive(s, eth, sizeof(eth)));
    for (unsigned i = 0; i < 8; ++i) {
        CHECK(r(s, 0x30) & 1);
        w(s, 0x34, 1);
    }
    CHECK(!(r(s, 0x30) & 1));
    for (unsigned i = 0; i < 100; ++i) {
        CHECK(xi825_receive(s, eth, sizeof(eth)));
        w(s, 0x34, 1);
    }
    cf_destroy(c);
    free(c);
}

int main(void)
{
    test_cis();
    test_commands();
    test_association();
    test_packets();
    puts("XI-825 tests passed");
    return 0;
}
