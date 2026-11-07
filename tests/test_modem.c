/* The CF modem model (src/hw/modem.c): 16550 registers, Hayes commands as
 * the Rockwell RC56LD answers them, online data, DTR hang-up, interrupts,
 * and the card's COR and COM1 window through src/hw/cf.c. */
#include "hw/cf.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* cf.c links the network cards' entry points; the modem uses none of them. */
void net_generate_unique_mac(struct machine *m, unsigned char *p) { (void)m; memset(p, 2, 6); }
void net_add_nic(struct net *n, struct nic_data *nic) { (void)n; (void)nic; }
int net_ethernet_rx_avail(struct net *n, struct nic_data *nic) { (void)n; (void)nic; return 0; }
int net_ethernet_rx(struct net *n, struct nic_data *nic, unsigned char **p, int *len)
{ (void)n; (void)nic; (void)p; (void)len; return 0; }
void net_ethernet_tx(struct net *n, struct nic_data *nic, unsigned char *p, int len)
{ (void)n; (void)nic; (void)p; (void)len; }
int net_ether_eq(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, 6); }
int net_ether_multicast(const uint8_t *a) { return a[0] & 1; }
int net_ether_broadcast(const uint8_t *a) { static const uint8_t b[6] = {255,255,255,255,255,255}; return !memcmp(a, b, 6); }

static bool answer = true;
static unsigned dials, hangups;
static char dialed[64];
static uint8_t sent[64];
static size_t sent_len;
static bool dial(void *o, const char *number) { (void)o; dials++; strcpy(dialed, number); return answer; }
static void hangup(void *o) { (void)o; hangups++; }
static void tx(void *o, const uint8_t *p, size_t n) { (void)o; memcpy(sent + sent_len, p, n); sent_len += n; }

static void type(cf_modem_t *m, const char *s)
{
    while (*s)
        cf_modem_write(m, 0, (uint8_t)*s++);
}

static void drain(cf_modem_t *m, char *out, size_t size)
{
    size_t n = 0;
    while ((cf_modem_read(m, 5) & 1) && n + 1 < size)
        out[n++] = (char)cf_modem_read(m, 0);
    out[n] = 0;
}

static void test_gps(void)
{
    /* --cf-gps: the same 16550, no AT interpreter, always streaming. */
    static cf_state_t g;
    sent_len = 0;
    assert(cf_attach_gps(&g, NULL, tx) == 0 && g.modem.gps && g.modem.online);
    assert(cf_cis_read_byte(&g, 0) == 0x01);
    char vers[8] = {0};
    for (unsigned i = 0; i < 5; i++) vers[i] = (char)cf_cis_read_byte(&g, 0x27 + i);
    assert(!strcmp(vers, "CFGPS"));
    assert(cf_window_read(&g, 0xC3FD, 1) == 0xff);              /* not configured yet */
    cf_cis_write(&g, 0x420, 1, 0x4d);                           /* the modem's COR: ignored */
    assert(cf_window_read(&g, 0xC3FD, 1) == 0xff);
    cf_cis_write(&g, 0x400, 1, 0x4d);
    assert(cf_cis_read(&g, 0x400, 1) == 0x4d);
    assert(cf_window_read(&g, 0xC3FD, 1) == 0x60);              /* LSR: nothing yet */
    cf_window_write(&g, 0xC3F8, 1, 'A'); cf_window_write(&g, 0xC3F8, 1, 'T'); cf_window_write(&g, 0xC3F8, 1, '\r');
    assert(!(cf_window_read(&g, 0xC3FD, 1) & 1) && sent_len == 3 && !memcmp(sent, "AT\r", 3));   /* no echo */
    assert(!(cf_window_read(&g, 0xC3FE, 1) & 0x80));            /* no DCD */
    assert(cf_modem_receive(&g.modem, (const uint8_t *)"$GP", 3) == 3);
    char got[8];
    drain(&g.modem, got, sizeof(got));
    assert(!strcmp(got, "$GP"));
    cf_window_write(&g, 0xC3FC, 1, 0x00);                         /* DTR down: still streaming */
    assert(cf_modem_receive(&g.modem, (const uint8_t *)"x", 1) == 1);
    cf_destroy(&g);
    sent_len = 0;
    puts("gps: CIS, COR, no echo or carrier, streaming regardless of DTR PASS");
}

int main(void)
{
    test_gps();
    cf_modem_t m = { .dial = dial, .hangup = hangup, .tx = tx };
    char out[256];
    cf_modem_reset(&m);
    /* Divisor latch and line control. */
    cf_modem_write(&m, 3, 0x83); cf_modem_write(&m, 0, 1); cf_modem_write(&m, 1, 0);
    assert(cf_modem_read(&m, 0) == 1 && cf_modem_read(&m, 1) == 0);
    cf_modem_write(&m, 3, 0x03);
    assert(cf_modem_read(&m, 5) == 0x60 && (cf_modem_read(&m, 6) & 0xb0) == 0x30);
    /* Echo and verbose results, as captured on the real card. */
    type(&m, "AT\r"); drain(&m, out, sizeof(out));
    assert(!strcmp(out, "AT\r\r\nOK\r\n"));
    type(&m, "ATI3\r"); drain(&m, out, sizeof(out));
    assert(!strcmp(out, "ATI3\r\r\nL27-01\r\n\r\nOK\r\n"));
    type(&m, "ATE0V1X4&D2&C1S7=60\r"); drain(&m, out, sizeof(out));
    assert(strstr(out, "OK") && m.echo == false);
    type(&m, "ATDT,\r"); drain(&m, out, sizeof(out));
    assert(!strcmp(out, "\r\nOK\r\n") && !dials);             /* no digits: no call */
    type(&m, "foo\r"); drain(&m, out, sizeof(out));
    assert(!strcmp(out, "\r\nERROR\r\n"));
    answer = false;
    type(&m, "ATDT555-0100\r"); drain(&m, out, sizeof(out));
    assert(!strcmp(out, "\r\nNO CARRIER\r\n") && dials == 1 && !strcmp(dialed, "5550100"));
    answer = true;
    cf_modem_write(&m, 4, 0x0b);                               /* DTR, RTS, OUT2 */
    type(&m, "ATDT5550100\r"); drain(&m, out, sizeof(out));
    assert(!strcmp(out, "\r\nCONNECT 52000\r\n") && m.online);
    assert(cf_modem_read(&m, 6) & 0x88);                       /* DCD, delta DCD */
    /* Online: bytes pass through both ways, interrupts follow IER. */
    type(&m, "~\x7d\x23");
    assert(sent_len == 3 && sent[0] == '~');
    cf_modem_write(&m, 1, 0x01);
    assert(!cf_modem_irq_pending(&m));
    assert(cf_modem_receive(&m, (const uint8_t *)"\x7e\xff", 2) == 2 && cf_modem_irq_pending(&m));
    assert((cf_modem_read(&m, 2) & 0x0f) == 0x04);
    drain(&m, out, sizeof(out));
    assert(!cf_modem_irq_pending(&m));
    cf_modem_write(&m, 1, 0x03);
    assert(cf_modem_irq_pending(&m) && (cf_modem_read(&m, 2) & 0x0f) == 0x02 && !cf_modem_irq_pending(&m));
    /* AT&D2: dropping DTR hangs up. */
    cf_modem_write(&m, 4, 0x0a);
    drain(&m, out, sizeof(out));
    assert(!m.online && hangups == 1 && !strcmp(out, "\r\nNO CARRIER\r\n"));
    assert(!(cf_modem_read(&m, 6) & 0x80));
    assert(cf_modem_receive(&m, (const uint8_t *)"x", 1) == 0);
    /* The card: the captured CIS, COR at 0x420, registers at 3f8h once configured. */
    static cf_state_t s;
    cf_init(&s);
    assert(cf_attach_modem(&s, NULL, dial, hangup, tx) == 0 && cf_is_io_card(&s) && !cf_is_network(&s));
    assert(cf_cis_read_byte(&s, 0x2e) == 0x13 && cf_cis_read_byte(&s, 0x35) == 0x02);
    assert(cf_window_read(&s, 0xC3FD, 1) == 0xff);              /* not configured yet */
    cf_cis_write(&s, 0x420, 1, 0x45);
    assert(cf_cis_read(&s, 0x420, 1) == 0x45);
    assert(cf_window_read(&s, 0xC3FD, 1) == 0x60);              /* LSR */
    cf_window_write(&s, 0xC3F8, 1, 'A'); cf_window_write(&s, 0xC3F8, 1, 'T'); cf_window_write(&s, 0xC3F8, 1, '\r');
    assert(cf_window_read(&s, 0xC3FD, 1) & 1);
    cf_window_write(&s, 0xC3F9, 1, 0x01); cf_window_write(&s, 0xC3FC, 1, 0x0b);
    assert(cf_giu_source_bits(&s) & 1);
    cf_destroy(&s);
    puts("modem: 16550 registers, Hayes results, dialing, online data, interrupts, DTR hang-up, CIS/COR/window PASS");
    return 0;
}
