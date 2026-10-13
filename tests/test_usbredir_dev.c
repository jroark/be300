/*
 * usbredir device transport test (src/usbredir_dev.c).
 *
 * A Unix-socket peer plays QEMU's usb-redir (the usbredir "usb-guest" side)
 * against the real USB function controller model (src/hw/usbf.c), while the
 * test also plays usb.dll through the controller registers, using the
 * guest sequences documented in docs/BE300_USB_FUNCTION.md.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "hw/usbf.h"
#include "usbredir_dev.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
            #cond); \
        failures++; \
    } \
} while (0)

enum {
    T_HELLO = 0, T_DEVICE_CONNECT = 1, T_DEVICE_DISCONNECT = 2,
    T_INTERFACE_INFO = 4, T_EP_INFO = 5, T_SET_CONFIGURATION = 6,
    T_CONFIGURATION_STATUS = 8, T_START_INTERRUPT_RECEIVING = 15,
    T_INTERRUPT_RECEIVING_STATUS = 17, T_CANCEL_DATA_PACKET = 21,
    T_CONTROL_PACKET = 100, T_BULK_PACKET = 101, T_INTERRUPT_PACKET = 103,
};

/* Peer (QEMU) caps: connect_device_version, filter, ep_info_max_packet_size,
 * 64bits_ids, 32bits_bulk_length, bulk_receiving. */
#define PEER_CAPS 0xF6u

/* Descriptor bytes from hw_dump_vrc4173.txt:105-116. */
static const uint8_t k_desc[] = {
    0x12, 0x01, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0x10, 0xCF, 0x07, 0x02, 0x20,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x09, 0x02, 0x27, 0x00, 0x01, 0x01, 0x00, 0x40, 0x01,
    0x09, 0x04, 0x00, 0x00, 0x03, 0xFF, 0xFF, 0xFF, 0x00,
    0x07, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x83, 0x03, 0x08, 0x00, 0x01,
};

static uint8_t g_ram[0x10000];
static usbf_state_t g_u;
static int g_peer = -1;
static size_t g_hl = 12;
static uint8_t g_rx[8192];
static size_t g_rx_len;

typedef struct {
    uint32_t type;
    uint64_t id;
    uint32_t len;
    uint8_t p[4096];
} pkt_t;

static uint8_t *ram_fn(void *ctx, uint32_t pa, uint32_t len)
{
    (void)ctx;
    if (pa + len > sizeof(g_ram))
        return NULL;
    return &g_ram[pa];
}

/* usb.dll init (pc 0x01923398 / 0x01923574), as in test_usbf.c. */
static void guest_init(usbf_state_t *u)
{
    usbf_write(u, USBF_TOP_EN, 0);
    usbf_write(u, USBF_DMA_EN, 0);
    usbf_write(u, USBF_UNIT_EN, 1);
    usbf_write(u, USBF_RESET, 1);
    usbf_write(u, USBF_RESET, 0);
    usbf_write(u, USBF_TOP_CLR, 3);
    usbf_write(u, USBF_CORE_EN, 1);
    for (unsigned i = 0; i < sizeof(k_desc); i++)
        usbf_write(u, USBF_DESC_BASE + 4u * i, k_desc[i]);
    usbf_write(u, USBF_EP_CTRL, 0x3F);
    usbf_write(u, USBF_INTMASK0, 0x7D);
    usbf_write(u, USBF_INTMASK1, 0x38);
    usbf_write(u, USBF_INTMASK2, 0x09);
    usbf_write(u, USBF_TOP_EN, 0x1A);
    usbf_write(u, USBF_CORE_EN, 0);
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return le16(p) | ((uint32_t)le16(p + 2) << 16); }

static void peer_send(uint32_t type, uint64_t id, const void *th, size_t thlen,
    const void *data, size_t dlen)
{
    uint8_t buf[4096];
    size_t n = 0;

    put32(buf, type);
    put32(buf + 4, (uint32_t)(thlen + dlen));
    put32(buf + 8, (uint32_t)id);
    if (g_hl == 16)
        put32(buf + 12, (uint32_t)(id >> 32));
    n = g_hl;
    memcpy(buf + n, th, thlen);
    n += thlen;
    if (dlen)
        memcpy(buf + n, data, dlen);
    n += dlen;
    if (write(g_peer, buf, n) != (ssize_t)n) {
        perror("peer write");
        exit(2);
    }
}

/* Tick the device until one packet arrives; false on timeout. */
static bool peer_recv(pkt_t *pk, int timeout_ms)
{
    for (int t = 0; t <= timeout_ms; t++) {
        ssize_t n;

        usbredir_dev_tick();
        n = read(g_peer, g_rx + g_rx_len, sizeof(g_rx) - g_rx_len);
        if (n > 0)
            g_rx_len += (size_t)n;
        if (g_rx_len >= g_hl) {
            uint32_t len = le32(g_rx + 4);

            if (g_rx_len >= g_hl + len) {
                pk->type = le32(g_rx);
                pk->len = len;
                pk->id = le32(g_rx + 8);
                if (g_hl == 16)
                    pk->id |= (uint64_t)le32(g_rx + 12) << 32;
                memcpy(pk->p, g_rx + g_hl, len);
                memmove(g_rx, g_rx + g_hl + len, g_rx_len - g_hl - len);
                g_rx_len -= g_hl + len;
                return true;
            }
        }
        sleep_ms(1);
    }
    return false;
}

static bool expect(pkt_t *pk, uint32_t type)
{
    if (!peer_recv(pk, 1000)) {
        fprintf(stderr, "timeout waiting for packet type %u\n", type);
        failures++;
        return false;
    }
    if (pk->type != type) {
        fprintf(stderr, "got packet type %u, want %u\n", pk->type, type);
        failures++;
        return false;
    }
    return true;
}

static bool quiet(int ms)
{
    pkt_t pk;
    return !peer_recv(&pk, ms);
}

static void control(uint64_t id, uint8_t rt, uint8_t req, uint16_t value,
    uint16_t index, uint16_t length)
{
    uint8_t h[10] = { (uint8_t)(rt & 0x80u), req, rt, 0 };
    put16(h + 4, value);
    put16(h + 6, index);
    put16(h + 8, length);
    peer_send(T_CONTROL_PACKET, id, h, sizeof(h), NULL, 0);
}

static void bulk(uint64_t id, uint8_t ep, uint32_t len, const uint8_t *data)
{
    uint8_t h[10] = { ep, 0 };
    put16(h + 2, (uint16_t)len);
    put16(h + 8, (uint16_t)(len >> 16));
    peer_send(T_BULK_PACKET, id, h, sizeof(h), (ep & 0x80) ? NULL : data,
        (ep & 0x80) ? 0 : len);
}

static void expect_offer(void)
{
    pkt_t pk;

    if (expect(&pk, T_INTERFACE_INFO)) {
        CHECK(le32(pk.p) == 1);
        CHECK(pk.p[4 + 32] == 0xFF && pk.p[4 + 64] == 0xFF &&
              pk.p[4 + 96] == 0xFF);
    }
    if (expect(&pk, T_EP_INFO)) {
        CHECK(pk.len == 160);
        CHECK(pk.p[0] == 0 && pk.p[16] == 0);          /* EP0 control */
        CHECK(pk.p[17] == 2 && pk.p[2] == 2);          /* 0x81, 0x02 bulk */
        CHECK(pk.p[19] == 3 && pk.p[32 + 19] == 1);    /* 0x83 interrupt */
        CHECK(pk.p[1] == 255);                         /* 0x01 unused */
        CHECK(le16(pk.p + 96 + 2 * 17) == 64);
        CHECK(le16(pk.p + 96 + 2 * 19) == 8);
        CHECK(le16(pk.p + 96) == 16);
    }
    if (expect(&pk, T_DEVICE_CONNECT)) {
        CHECK(pk.len == 10);
        CHECK(pk.p[0] == 1);                           /* full speed */
        CHECK(pk.p[1] == 0xFF);
        CHECK(le16(pk.p + 4) == 0x07CF && le16(pk.p + 6) == 0x2002);
        CHECK(le16(pk.p + 8) == 0x0100);
    }
}

int main(void)
{
    char dir[] = "/tmp/usbredir.XXXXXX";
    char path[256];
    struct sockaddr_un sa;
    usbredir_dev_config_t cfg;
    uint8_t hello[68], buf[256], data[100];
    pkt_t pk;

    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(path, sizeof(path), "%s/r.sock", dir);

    g_u.ram = ram_fn;
    usbf_init(&g_u);
    memset(&cfg, 0, sizeof(cfg));
    cfg.kind = HOST_CHARDEV_UNIX_LISTEN;
    cfg.target = path;
    if (!usbredir_dev_configure(&cfg))
        return 2;
    usbredir_dev_bind(&g_u);

    g_peer = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    if (connect(g_peer, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        perror("connect");
        return 2;
    }
    fcntl(g_peer, F_SETFL, fcntl(g_peer, F_GETFL, 0) | O_NONBLOCK);

    /* Hello exchange in 32-bit-id headers, then 64-bit ids. */
    if (expect(&pk, T_HELLO)) {
        CHECK(pk.len == 68);
        CHECK((le32(pk.p + 64) & 0x72u) == 0x72u);
    }
    memset(hello, 0, sizeof(hello));
    strcpy((char *)hello, "qemu usb-redir guest test");
    put32(hello + 64, PEER_CAPS);
    peer_send(T_HELLO, 0, hello, sizeof(hello), NULL, 0);
    g_hl = 16;

    /* Cable in but usb.dll has not brought the controller up: no device. */
    usbredir_dev_set_present(true);
    CHECK(quiet(20));
    guest_init(&g_u);
    expect_offer();

    /* Standard request answered from descriptor RAM. */
    control(0x100000001ull, 0x80, 0x06, 0x0100, 0, 64);
    if (expect(&pk, T_CONTROL_PACKET)) {
        CHECK(pk.id == 0x100000001ull);
        CHECK(pk.p[3] == 0 && le16(pk.p + 8) == 18 && pk.len == 10 + 18);
        CHECK(memcmp(pk.p + 10, k_desc, 18) == 0);
    }
    control(2, 0x80, 0x06, 0x0300, 0, 255);           /* no strings */
    if (expect(&pk, T_CONTROL_PACKET))
        CHECK(pk.p[3] == 4 && pk.len == 10);

    /* SET_CONFIGURATION reaches the guest as the INTSTAT2 config event. */
    buf[0] = 1;
    peer_send(T_SET_CONFIGURATION, 3, buf, 1, NULL, 0);
    if (expect(&pk, T_CONFIGURATION_STATUS))
        CHECK(pk.id == 3 && pk.p[0] == 0 && pk.p[1] == 1);
    expect(&pk, T_INTERFACE_INFO);
    expect(&pk, T_EP_INFO);
    CHECK(usbf_read(&g_u, USBF_INTSTAT2) & USBF_I2_CONFIG);
    CHECK(usbf_read(&g_u, USBF_CONFIG) == 1);
    usbf_write(&g_u, USBF_INTCLR2, 0);

    /* SET_CONTROL_LINE_STATE(DTR|RTS) completes when usb.dll commits EP0. */
    control(4, 0x21, 0x22, 3, 0, 0);
    CHECK(quiet(20));
    CHECK(usbf_read(&g_u, USBF_INTSTAT1) & USBF_I1_SETUP);
    for (int i = 0; i < 8; i++)
        buf[i] = (uint8_t)usbf_read(&g_u, USBF_SETUP_FIFO);
    CHECK(buf[0] == 0x21 && buf[1] == 0x22 && buf[2] == 3);
    usbf_write(&g_u, USBF_EP_COMMIT, 1);
    if (expect(&pk, T_CONTROL_PACKET))
        CHECK(pk.id == 4 && pk.p[3] == 0 && pk.len == 10);

    /* EP3 serial state: forwarded once interrupt receiving is started. */
    buf[0] = 0x83;
    peer_send(T_START_INTERRUPT_RECEIVING, 5, buf, 1, NULL, 0);
    if (expect(&pk, T_INTERRUPT_RECEIVING_STATUS))
        CHECK(pk.p[0] == 0 && pk.p[1] == 0x83);
    usbf_write(&g_u, USBF_EP3_FIFO, 0x81);
    usbf_write(&g_u, USBF_EP3_FIFO, 0x00);
    usbf_write(&g_u, USBF_EP_COMMIT, 4);
    if (expect(&pk, T_INTERRUPT_PACKET)) {
        CHECK(pk.p[0] == 0x83 && pk.p[1] == 0 && le16(pk.p + 2) == 2);
        CHECK(pk.p[4] == 0x81 && pk.p[5] == 0x00);
    }

    /* Bulk OUT of 100 bytes arrives as a 64 + 36 byte packet pair. */
    for (int i = 0; i < 100; i++)
        data[i] = (uint8_t)(i * 3 + 1);
    bulk(6, 0x02, sizeof(data), data);
    if (expect(&pk, T_BULK_PACKET)) {
        CHECK(pk.id == 6 && pk.p[0] == 0x02 && pk.p[1] == 0);
        CHECK(le16(pk.p + 2) == 100 && pk.len == 10);
    }
    {
        unsigned got = 0;

        for (int k = 0; k < 2; k++) {
            unsigned cnt = usbf_read(&g_u, USBF_OUT_COUNT);

            CHECK(cnt == (k == 0 ? 64u : 36u));
            for (unsigned i = 0; i < cnt && got < sizeof(buf); i++)
                buf[got++] = (uint8_t)usbf_read(&g_u, USBF_OUT_FIFO);
        }
        CHECK(got == 100 && memcmp(buf, data, 100) == 0);
    }

    /* Bulk IN waits (NAK) until usb.dll DMAs and commits a packet. */
    bulk(7, 0x81, 128, NULL);
    CHECK(quiet(20));
    for (int i = 0; i < 20; i++)
        g_ram[0x8000 + i] = (uint8_t)(0xA0 + i);
    usbf_write(&g_u, USBF_DMA_DIR, 1);
    usbf_write(&g_u, USBF_DMA_LEN, 20);
    usbf_write(&g_u, USBF_DMA_ADDR, 0xA0008000u);
    usbf_write(&g_u, USBF_DMA_START, 1);
    usbf_write(&g_u, USBF_EP_COMMIT, 2);
    if (expect(&pk, T_BULK_PACKET)) {
        CHECK(pk.id == 7 && pk.p[0] == 0x81 && pk.p[1] == 0);
        CHECK(le16(pk.p + 2) == 20 && pk.len == 10 + 20);
        CHECK(pk.p[10] == 0xA0 && pk.p[29] == 0xA0 + 19);
    }

    /*
     * A 72-byte reply: a full 64-byte fill goes out without a commit, then
     * the 8-byte tail is committed.  One 4096-byte IN request (what Windows
     * submits) gets all 72 bytes.
     */
    bulk(11, 0x81, 4096, NULL);
    for (int i = 0; i < 72; i++)
        g_ram[0x8040 + i] = (uint8_t)(0x40 + i);
    usbf_write(&g_u, USBF_DMA_LEN, 64);
    usbf_write(&g_u, USBF_DMA_ADDR, 0xA0008040u);
    usbf_write(&g_u, USBF_DMA_START, 1);
    usbf_write(&g_u, USBF_DMA_LEN, 8);
    usbf_write(&g_u, USBF_DMA_ADDR, 0xA0008080u);
    usbf_write(&g_u, USBF_DMA_START, 1);
    usbf_write(&g_u, USBF_EP_COMMIT, 2);
    if (expect(&pk, T_BULK_PACKET)) {
        CHECK(pk.id == 11 && pk.p[1] == 0);
        CHECK(le16(pk.p + 2) == 72 && pk.len == 10 + 72);
        CHECK(pk.p[10] == 0x40 && pk.p[10 + 71] == 0x40 + 71);
    }

    /* A cancelled IN request is answered with status cancelled. */
    bulk(8, 0x81, 64, NULL);
    peer_send(T_CANCEL_DATA_PACKET, 8, NULL, 0, NULL, 0);
    if (expect(&pk, T_BULK_PACKET))
        CHECK(pk.id == 8 && pk.p[1] == 1 && le16(pk.p + 2) == 0);

    /* Guest reset (KjCMU) drops the device; usb.dll re-init offers it again. */
    usbf_reset(&g_u);
    expect(&pk, T_DEVICE_DISCONNECT);
    CHECK(quiet(20));
    guest_init(&g_u);
    expect_offer();

    /* Data before configuration is NAKed by the controller, not lost. */
    bulk(9, 0x02, 4, data);
    CHECK(quiet(20));
    peer_send(T_SET_CONFIGURATION, 10, (uint8_t[]){1}, 1, NULL, 0);
    expect(&pk, T_CONFIGURATION_STATUS);
    expect(&pk, T_INTERFACE_INFO);
    expect(&pk, T_EP_INFO);
    if (expect(&pk, T_BULK_PACKET))
        CHECK(pk.id == 9 && le16(pk.p + 2) == 4);

    /* Peer loss and shutdown. */
    close(g_peer);
    for (int i = 0; i < 10; i++)
        usbredir_dev_tick();
    CHECK(!g_u.attached);
    usbredir_dev_shutdown();
    unlink(path);
    rmdir(dir);

    if (failures) {
        fprintf(stderr, "test_usbredir_dev: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_usbredir_dev: ok\n");
    return 0;
}
