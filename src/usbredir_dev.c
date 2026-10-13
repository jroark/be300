/*
 * usbredir_dev — usbredir "usb-host" endpoint for the BE-300 USB function
 * controller.
 *
 * Wire format: usbredir 0.7 (usbredirproto.h).  Every packet is a header
 * {u32 type, u32 length, u32|u64 id} followed by `length` bytes holding a
 * type-specific header and optional data, all little-endian.  The id is
 * 64 bits once both peers have advertised usb_redir_cap_64bits_ids; the
 * hello packets themselves always use the 32-bit form.
 *
 * Device side mapping (usbf.c host API):
 *   control_packet      -> usbf_host_control (standard requests answer at
 *                          once; class/vendor requests complete when usb.dll
 *                          finishes the status stage, via ctrl_done)
 *   set/get_configuration, set/get_alt_setting
 *                       -> the matching standard request.  QEMU consumes
 *                          SET_ADDRESS itself, so after every bus reset the
 *                          controller is given a nominal SET_ADDRESS(1): the
 *                          address value is never visible to the host side.
 *   bulk_packet OUT     -> split into max-packet chunks, usbf_host_out; a
 *                          full OUT FIFO NAKs and the rest waits for the
 *                          guest to drain it
 *   bulk_packet IN      -> filled from usbf_host_in until a short packet or
 *                          the requested length
 *   start_interrupt_receiving + interrupt_packet
 *                       -> EP3 serial-state packets forwarded as committed
 *
 * The device is offered (interface_info, ep_info, device_connect) once the
 * cable is present, the peer's hello has arrived and usb.dll has written
 * valid descriptors and enabled the controller -- the point at which the
 * real device's pull-up makes it visible on the bus.  A guest reset clears
 * the controller (usbf_reset) and sends device_disconnect; re-init offers
 * the device again.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "usbredir_dev.h"

/* usbredirproto.h packet types */
enum {
    UR_HELLO = 0,
    UR_DEVICE_CONNECT = 1,
    UR_DEVICE_DISCONNECT = 2,
    UR_RESET = 3,
    UR_INTERFACE_INFO = 4,
    UR_EP_INFO = 5,
    UR_SET_CONFIGURATION = 6,
    UR_GET_CONFIGURATION = 7,
    UR_CONFIGURATION_STATUS = 8,
    UR_SET_ALT_SETTING = 9,
    UR_GET_ALT_SETTING = 10,
    UR_ALT_SETTING_STATUS = 11,
    UR_START_ISO_STREAM = 12,
    UR_STOP_ISO_STREAM = 13,
    UR_ISO_STREAM_STATUS = 14,
    UR_START_INTERRUPT_RECEIVING = 15,
    UR_STOP_INTERRUPT_RECEIVING = 16,
    UR_INTERRUPT_RECEIVING_STATUS = 17,
    UR_ALLOC_BULK_STREAMS = 18,
    UR_FREE_BULK_STREAMS = 19,
    UR_BULK_STREAMS_STATUS = 20,
    UR_CANCEL_DATA_PACKET = 21,
    UR_FILTER_REJECT = 22,
    UR_FILTER_FILTER = 23,
    UR_DEVICE_DISCONNECT_ACK = 24,
    UR_START_BULK_RECEIVING = 25,
    UR_STOP_BULK_RECEIVING = 26,
    UR_BULK_RECEIVING_STATUS = 27,
    UR_CONTROL_PACKET = 100,
    UR_BULK_PACKET = 101,
    UR_ISO_PACKET = 102,
    UR_INTERRUPT_PACKET = 103,
    UR_BUFFERED_BULK_PACKET = 104,
};

/* usbredirproto.h status codes */
enum {
    UR_SUCCESS = 0,
    UR_CANCELLED = 1,
    UR_INVAL = 2,
    UR_IOERROR = 3,
    UR_STALL = 4,
    UR_TIMEOUT = 5,
    UR_BABBLE = 6,
};

/* usbredirproto.h capability bits */
#define UR_CAP_CONNECT_DEVICE_VERSION  (1u << 1)
#define UR_CAP_EP_INFO_MAX_PACKET_SIZE (1u << 4)
#define UR_CAP_64BITS_IDS              (1u << 5)
#define UR_CAP_32BITS_BULK_LENGTH      (1u << 6)
#define UR_OUR_CAPS (UR_CAP_CONNECT_DEVICE_VERSION | \
                     UR_CAP_EP_INFO_MAX_PACKET_SIZE | \
                     UR_CAP_64BITS_IDS | UR_CAP_32BITS_BULK_LENGTH)

#define UR_SPEED_FULL     1
#define UR_TYPE_CONTROL   0
#define UR_TYPE_BULK      2
#define UR_TYPE_INTERRUPT 3
#define UR_TYPE_INVALID   255

#define UR_MAX_PAYLOAD (4u * 1024u * 1024u)
#define UR_QUEUE_LEN   64u
#define UR_VERSION     "be300-usbredir-dev 1.0"

typedef struct {
    uint64_t id;
    uint8_t  ep;
    uint8_t *data;
    uint32_t len;   /* OUT: bytes to send; IN: bytes requested */
    uint32_t off;   /* bytes moved so far */
} ur_xfer_t;

typedef struct {
    ur_xfer_t x[UR_QUEUE_LEN];
    unsigned head, count;
} ur_queue_t;

static struct {
    bool armed;
    bool trace;
    host_chardev_t ch;
    FILE *tee;
    usbf_state_t *u;
    bool present;

    uint8_t *rx;
    size_t rx_len, rx_cap;
    uint8_t *tx;
    size_t tx_len, tx_cap;

    bool peer_hello;
    uint32_t peer_caps;
    bool connected;

    bool ctrl_active;
    uint64_t ctrl_id;
    uint8_t ctrl_hdr[10];

    ur_queue_t bulk_in, bulk_out;
    bool intr_receiving;
    uint64_t intr_seq;

    /* Endpoints from the guest's configuration descriptor. */
    uint8_t ep_bulk_in, ep_bulk_out, ep_intr_in;
    uint16_t mps_bulk_in, mps_bulk_out;
} g;

/* ---------- helpers ---------- */

static uint64_t mono_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void ur_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ur_trace(const char *fmt, ...)
{
    va_list ap;

    if (!g.trace)
        return;
    va_start(ap, fmt);
    fprintf(stderr, "[USB_REDIR] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static bool both_cap(uint32_t cap)
{
    return (UR_OUR_CAPS & g.peer_caps & cap) != 0;
}

static size_t hdr_len(void)
{
    return both_cap(UR_CAP_64BITS_IDS) ? 16u : 12u;
}

static size_t bulk_hdr_len(void)
{
    return both_cap(UR_CAP_32BITS_BULK_LENGTH) ? 10u : 8u;
}

static void tee_packet(const char *dir, uint32_t type, uint64_t id,
    const uint8_t *th, size_t thlen, const uint8_t *data, size_t dlen)
{
    if (!g.tee)
        return;
    fprintf(g.tee, "%llu %s type=%u id=%llu hdr:",
        (unsigned long long)mono_ns(), dir, type, (unsigned long long)id);
    for (size_t i = 0; i < thlen; i++)
        fprintf(g.tee, " %02x", th[i]);
    if (dlen) {
        fprintf(g.tee, " data[%zu]:", dlen);
        for (size_t i = 0; i < dlen && i < 64; i++)
            fprintf(g.tee, " %02x", data[i]);
        if (dlen > 64)
            fprintf(g.tee, " ...");
    }
    fputc('\n', g.tee);
}

static bool buf_reserve(uint8_t **buf, size_t *cap, size_t need)
{
    size_t n;
    uint8_t *p;

    if (need <= *cap)
        return true;
    n = *cap ? *cap : 4096u;
    while (n < need)
        n *= 2u;
    p = realloc(*buf, n);
    if (!p)
        return false;
    *buf = p;
    *cap = n;
    return true;
}

/* ---------- queues ---------- */

static ur_xfer_t *queue_head(ur_queue_t *q)
{
    return q->count ? &q->x[q->head] : NULL;
}

static ur_xfer_t *queue_push(ur_queue_t *q)
{
    ur_xfer_t *x;

    if (q->count >= UR_QUEUE_LEN)
        return NULL;
    x = &q->x[(q->head + q->count) % UR_QUEUE_LEN];
    memset(x, 0, sizeof(*x));
    q->count++;
    return x;
}

static void queue_pop(ur_queue_t *q)
{
    free(q->x[q->head].data);
    q->x[q->head].data = NULL;
    q->head = (q->head + 1u) % UR_QUEUE_LEN;
    q->count--;
}

/* Remove the entry with this id; returns its endpoint or -1. */
static int queue_remove(ur_queue_t *q, uint64_t id)
{
    for (unsigned i = 0; i < q->count; i++) {
        unsigned k = (q->head + i) % UR_QUEUE_LEN;
        int ep;

        if (q->x[k].id != id)
            continue;
        ep = q->x[k].ep;
        free(q->x[k].data);
        for (unsigned j = i; j + 1u < q->count; j++)
            q->x[(q->head + j) % UR_QUEUE_LEN] =
                q->x[(q->head + j + 1u) % UR_QUEUE_LEN];
        q->count--;
        return ep;
    }
    return -1;
}

static void queue_clear(ur_queue_t *q)
{
    while (q->count)
        queue_pop(q);
    q->head = 0;
}

/* ---------- transmit ---------- */

static void flush_tx(void);

static void send_packet(uint32_t type, uint64_t id, const void *th,
    size_t thlen, const void *data, size_t dlen)
{
    size_t hl = hdr_len();
    uint8_t *p;

    if (!host_chardev_ready(&g.ch))
        return;
    if (!buf_reserve(&g.tx, &g.tx_cap, g.tx_len + hl + thlen + dlen)) {
        fprintf(stderr, "[USB_REDIR] oom; dropping packet type %u\n", type);
        return;
    }
    p = g.tx + g.tx_len;
    put32(p, type);
    put32(p + 4, (uint32_t)(thlen + dlen));
    put32(p + 8, (uint32_t)id);
    if (hl == 16u)
        put32(p + 12, (uint32_t)(id >> 32));
    if (thlen)
        memcpy(p + hl, th, thlen);
    if (dlen)
        memcpy(p + hl + thlen, data, dlen);
    g.tx_len += hl + thlen + dlen;
    tee_packet("D>H", type, id, th, thlen, data, dlen);
    flush_tx();
}

static void send_hello(void)
{
    uint8_t h[64 + 4];

    memset(h, 0, sizeof(h));
    snprintf((char *)h, 64, "%s", UR_VERSION);
    put32(h + 64, UR_OUR_CAPS);
    send_packet(UR_HELLO, 0, h, sizeof(h), NULL, 0);
}

static void reply_control(uint64_t id, const uint8_t hdr[10], uint8_t status,
    const uint8_t *data, uint16_t dlen)
{
    uint8_t h[10];
    bool in = (hdr[2] & 0x80u) != 0;

    memcpy(h, hdr, 10);
    h[3] = status;
    if (status != UR_SUCCESS)
        put16(h + 8, 0);
    else if (in)
        put16(h + 8, dlen);
    send_packet(UR_CONTROL_PACKET, id, h, sizeof(h),
        in && status == UR_SUCCESS ? data : NULL,
        in && status == UR_SUCCESS ? dlen : 0);
}

static void reply_bulk(uint64_t id, uint8_t ep, uint8_t status,
    const uint8_t *data, uint32_t len)
{
    uint8_t h[10];
    bool in = (ep & 0x80u) != 0;

    memset(h, 0, sizeof(h));
    h[0] = ep;
    h[1] = status;
    put16(h + 2, (uint16_t)len);
    put16(h + 8, (uint16_t)(len >> 16));
    send_packet(UR_BULK_PACKET, id, h, bulk_hdr_len(),
        in ? data : NULL, in ? len : 0);
}

static void send_status2(uint32_t type, uint64_t id, uint8_t a, uint8_t b)
{
    uint8_t h[2] = { a, b };
    send_packet(type, id, h, sizeof(h), NULL, 0);
}

/* ---------- device description ---------- */

static unsigned ep_index(uint8_t ep)
{
    return ((ep & 0x80u) >> 3) | (ep & 0x0Fu);
}

/* Send interface_info and ep_info from the guest-written configuration
 * descriptor, and remember the data endpoints. */
static void send_interface_and_ep_info(void)
{
    uint8_t cfg[256], dev[18];
    uint8_t ii[4 + 4 * 32];
    uint8_t ei[96 + 64];
    size_t n, pos;
    unsigned nif = 0;
    uint8_t cur_if = 0;
    bool cur_alt0 = false;

    memset(ii, 0, sizeof(ii));
    memset(ei, 0, sizeof(ei));
    memset(ei, UR_TYPE_INVALID, 32);
    g.ep_bulk_in = g.ep_bulk_out = g.ep_intr_in = 0;

    usbf_descriptor_copy(g.u, 1, dev, sizeof(dev));
    ei[0] = ei[16] = UR_TYPE_CONTROL;
    put16(ei + 96 + 2 * 0, dev[7]);
    put16(ei + 96 + 2 * 16, dev[7]);

    n = usbf_descriptor_copy(g.u, 2, cfg, sizeof(cfg));
    for (pos = 0; pos + 2 <= n && cfg[pos] >= 2; pos += cfg[pos]) {
        const uint8_t *d = &cfg[pos];

        if (pos + d[0] > n)
            break;
        if (d[1] == 4 && d[0] >= 9) {
            cur_if = d[2];
            cur_alt0 = d[3] == 0;
            if (cur_alt0 && nif < 32) {
                ii[4 + nif] = d[2];
                ii[4 + 32 + nif] = d[5];
                ii[4 + 64 + nif] = d[6];
                ii[4 + 96 + nif] = d[7];
                nif++;
            }
        } else if (d[1] == 5 && d[0] >= 7 && cur_alt0) {
            uint8_t ep = d[2], type = d[3] & 3u;
            unsigned k = ep_index(ep);

            ei[k] = type;
            ei[32 + k] = d[6];
            ei[64 + k] = cur_if;
            put16(ei + 96 + 2 * k, le16(d + 4));
            if (type == UR_TYPE_BULK && (ep & 0x80u)) {
                g.ep_bulk_in = ep;
                g.mps_bulk_in = le16(d + 4);
            } else if (type == UR_TYPE_BULK) {
                g.ep_bulk_out = ep;
                g.mps_bulk_out = le16(d + 4);
            } else if (type == UR_TYPE_INTERRUPT && (ep & 0x80u)) {
                g.ep_intr_in = ep;
            }
        }
    }
    put32(ii, nif);
    send_packet(UR_INTERFACE_INFO, 0, ii, sizeof(ii), NULL, 0);
    send_packet(UR_EP_INFO, 0, ei,
        both_cap(UR_CAP_EP_INFO_MAX_PACKET_SIZE) ? 160u : 96u, NULL, 0);
}

static void send_device_connect(void)
{
    uint8_t dev[18], h[10];

    usbf_descriptor_copy(g.u, 1, dev, sizeof(dev));
    h[0] = UR_SPEED_FULL;
    h[1] = dev[4];
    h[2] = dev[5];
    h[3] = dev[6];
    put16(h + 4, le16(dev + 8));
    put16(h + 6, le16(dev + 10));
    put16(h + 8, le16(dev + 12));
    send_packet(UR_DEVICE_CONNECT, 0, h,
        both_cap(UR_CAP_CONNECT_DEVICE_VERSION) ? 10u : 8u, NULL, 0);
    fprintf(stderr, "[USB_REDIR] device %04x:%04x offered to host\n",
        le16(dev + 8), le16(dev + 10));
}

/* ---------- device side ---------- */

static int standard_request(uint8_t rt, uint8_t req, uint16_t value,
    uint16_t index)
{
    uint8_t setup[8] = { rt, req, (uint8_t)value, (uint8_t)(value >> 8),
                         (uint8_t)index, (uint8_t)(index >> 8), 0, 0 };
    uint8_t in[8];
    uint16_t in_len;

    return usbf_host_control(g.u, setup, in, &in_len);
}

static void bus_reset(void)
{
    usbf_host_bus_reset(g.u);
    (void)standard_request(0x00, 0x05, 1, 0);   /* SET_ADDRESS, see header */
}

static void drop_transfers(void)
{
    queue_clear(&g.bulk_in);
    queue_clear(&g.bulk_out);
    g.ctrl_active = false;
    g.intr_receiving = false;
}

static void device_disconnect(bool tell_peer)
{
    if (!g.connected)
        return;
    g.connected = false;
    drop_transfers();
    if (tell_peer)
        send_packet(UR_DEVICE_DISCONNECT, 0, NULL, 0, NULL, 0);
    usbf_host_attach(g.u, false);
    fprintf(stderr, "[USB_REDIR] device disconnected\n");
}

static void update_connection(void)
{
    bool want = g.u && g.present && g.peer_hello &&
        host_chardev_ready(&g.ch) && usbf_descriptors_ready(g.u);

    if (want && !g.connected) {
        usbf_host_attach(g.u, true);
        bus_reset();
        send_interface_and_ep_info();
        send_device_connect();
        g.connected = true;
    } else if (!want && g.connected) {
        device_disconnect(host_chardev_ready(&g.ch));
    }
}

static void service_endpoints(void)
{
    ur_xfer_t *x;
    uint8_t pkt[USBF_EP_MAX_PACKET];

    if (!g.connected)
        return;

    /* Host -> device: one max-packet chunk per OUT token. */
    while ((x = queue_head(&g.bulk_out)) != NULL) {
        uint32_t chunk = x->len - x->off;
        uint32_t mps = g.mps_bulk_out ? g.mps_bulk_out : USBF_EP_MAX_PACKET;

        if (chunk > mps)
            chunk = mps;
        if (!usbf_host_out(g.u, x->data + x->off, chunk))
            break;                                  /* NAK: FIFO full */
        x->off += chunk;
        if (x->off < x->len)
            continue;
        reply_bulk(x->id, x->ep, UR_SUCCESS, NULL, x->len);
        queue_pop(&g.bulk_out);
    }

    /* Device -> host: packets until short or the request is filled. */
    while ((x = queue_head(&g.bulk_in)) != NULL) {
        uint32_t mps = g.mps_bulk_in ? g.mps_bulk_in : USBF_EP_MAX_PACKET;
        int n = usbf_host_in(g.u, pkt, sizeof(pkt));
        uint8_t status = UR_SUCCESS;
        bool done;

        if (n < 0)
            break;                                  /* NAK */
        if ((uint32_t)n > x->len - x->off) {
            n = (int)(x->len - x->off);
            status = UR_BABBLE;
        }
        memcpy(x->data + x->off, pkt, (size_t)n);
        x->off += (uint32_t)n;
        done = status != UR_SUCCESS || (uint32_t)n < mps || x->off == x->len;
        if (!done)
            continue;
        reply_bulk(x->id, x->ep, status, x->data, x->off);
        queue_pop(&g.bulk_in);
    }

    if (g.intr_receiving) {
        int n = usbf_host_intr_in(g.u, pkt, sizeof(pkt));

        if (n >= 0) {
            uint8_t h[4] = { g.ep_intr_in, UR_SUCCESS, 0, 0 };
            put16(h + 2, (uint16_t)n);
            send_packet(UR_INTERRUPT_PACKET, g.intr_seq++, h, sizeof(h),
                pkt, (size_t)n);
        }
    }
}

static void on_ctrl_done(void *ctx, int status)
{
    (void)ctx;
    if (!g.ctrl_active)
        return;
    g.ctrl_active = false;
    reply_control(g.ctrl_id, g.ctrl_hdr,
        status == USBF_CTRL_OK ? UR_SUCCESS : UR_STALL, NULL, 0);
}

static void on_in_ready(void *ctx)
{
    (void)ctx;
    /* Serviced from usbredir_dev_tick(), outside the guest MMIO write. */
}

/* ---------- receive ---------- */

static void handle_control(uint64_t id, const uint8_t *p, uint32_t len)
{
    uint8_t setup[8], in[512];
    uint16_t in_len = 0;
    int rc;

    if (len < 10)
        return;
    if (!g.connected) {
        reply_control(id, p, UR_IOERROR, NULL, 0);
        return;
    }
    setup[0] = p[2];            /* bmRequestType */
    setup[1] = p[1];            /* bRequest */
    memcpy(setup + 2, p + 4, 6);
    rc = usbf_host_control(g.u, setup, in, &in_len);
    if (rc == USBF_CTRL_PENDING) {
        g.ctrl_active = true;
        g.ctrl_id = id;
        memcpy(g.ctrl_hdr, p, 10);
        return;
    }
    reply_control(id, p, rc == USBF_CTRL_OK ? UR_SUCCESS : UR_STALL,
        in, in_len);
}

static void handle_bulk(uint64_t id, const uint8_t *p, uint32_t len)
{
    size_t hl = bulk_hdr_len();
    uint8_t ep;
    uint32_t blen;
    ur_xfer_t *x;

    if (len < hl)
        return;
    ep = p[0];
    blen = le16(p + 2);
    if (hl == 10u)
        blen |= (uint32_t)le16(p + 8) << 16;
    if (!g.connected) {
        reply_bulk(id, ep, UR_IOERROR, NULL, 0);
        return;
    }
    if (ep != g.ep_bulk_in && ep != g.ep_bulk_out) {
        reply_bulk(id, ep, UR_INVAL, NULL, 0);
        return;
    }
    if (!(ep & 0x80u))
        blen = len - (uint32_t)hl;
    x = queue_push((ep & 0x80u) ? &g.bulk_in : &g.bulk_out);
    if (!x) {
        ur_trace("bulk queue full for ep 0x%02x", ep);
        reply_bulk(id, ep, UR_IOERROR, NULL, 0);
        return;
    }
    x->id = id;
    x->ep = ep;
    x->len = blen;
    x->data = malloc(blen ? blen : 1u);
    if (!x->data) {
        queue_remove((ep & 0x80u) ? &g.bulk_in : &g.bulk_out, id);
        reply_bulk(id, ep, UR_IOERROR, NULL, 0);
        return;
    }
    if (!(ep & 0x80u))
        memcpy(x->data, p + hl, blen);
}

static void handle_cancel(uint64_t id)
{
    int ep;

    if (g.ctrl_active && g.ctrl_id == id) {
        /* The guest still owns the SETUP; its completion is ignored. */
        g.ctrl_active = false;
        reply_control(id, g.ctrl_hdr, UR_CANCELLED, NULL, 0);
        return;
    }
    if ((ep = queue_remove(&g.bulk_in, id)) >= 0 ||
        (ep = queue_remove(&g.bulk_out, id)) >= 0)
        reply_bulk(id, (uint8_t)ep, UR_CANCELLED, NULL, 0);
}

static void handle_packet(uint32_t type, uint64_t id, const uint8_t *p,
    uint32_t len)
{
    uint8_t setup_rc;

    tee_packet("H>D", type, id, p, len > 16u ? 16u : len,
        len > 16u ? p + 16 : NULL, len > 16u ? len - 16u : 0);

    switch (type) {
    case UR_HELLO:
        if (len < 64)
            return;
        g.peer_caps = len >= 68 ? le32(p + 64) : 0;
        g.peer_hello = true;
        fprintf(stderr, "[USB_REDIR] peer hello '%.64s' caps=0x%08x\n",
            (const char *)p, g.peer_caps);
        break;
    case UR_RESET:
        if (g.connected) {
            drop_transfers();
            bus_reset();
        }
        break;
    case UR_SET_CONFIGURATION:
        if (len < 1)
            return;
        if (!g.connected) {
            send_status2(UR_CONFIGURATION_STATUS, id, UR_IOERROR, 0);
            return;
        }
        setup_rc = standard_request(0x00, 0x09, p[0], 0) == USBF_CTRL_OK ?
            UR_SUCCESS : UR_STALL;
        send_status2(UR_CONFIGURATION_STATUS, id, setup_rc, g.u->config);
        if (setup_rc == UR_SUCCESS)
            send_interface_and_ep_info();
        break;
    case UR_GET_CONFIGURATION:
        send_status2(UR_CONFIGURATION_STATUS, id,
            g.connected ? UR_SUCCESS : UR_IOERROR,
            g.connected ? g.u->config : 0);
        break;
    case UR_SET_ALT_SETTING: {
        uint8_t h[3];

        if (len < 2)
            return;
        h[0] = !g.connected ? UR_IOERROR :
            standard_request(0x01, 0x0B, p[1], p[0]) == USBF_CTRL_OK ?
            UR_SUCCESS : UR_STALL;
        h[1] = p[0];
        h[2] = h[0] == UR_SUCCESS ? p[1] : 0;
        send_packet(UR_ALT_SETTING_STATUS, id, h, sizeof(h), NULL, 0);
        break;
    }
    case UR_GET_ALT_SETTING: {
        uint8_t h[3] = { g.connected ? UR_SUCCESS : UR_IOERROR,
                         len ? p[0] : 0, 0 };
        send_packet(UR_ALT_SETTING_STATUS, id, h, sizeof(h), NULL, 0);
        break;
    }
    case UR_START_INTERRUPT_RECEIVING:
        if (len < 1)
            return;
        if (g.connected && p[0] == g.ep_intr_in && p[0] != 0) {
            g.intr_receiving = true;
            send_status2(UR_INTERRUPT_RECEIVING_STATUS, id, UR_SUCCESS, p[0]);
        } else {
            send_status2(UR_INTERRUPT_RECEIVING_STATUS, id, UR_INVAL, p[0]);
        }
        break;
    case UR_STOP_INTERRUPT_RECEIVING:
        if (len < 1)
            return;
        g.intr_receiving = false;
        send_status2(UR_INTERRUPT_RECEIVING_STATUS, id, UR_SUCCESS, p[0]);
        break;
    case UR_CANCEL_DATA_PACKET:
        handle_cancel(id);
        break;
    case UR_CONTROL_PACKET:
        handle_control(id, p, len);
        break;
    case UR_BULK_PACKET:
        handle_bulk(id, p, len);
        break;
    case UR_INTERRUPT_PACKET: {
        /* No interrupt OUT endpoint on this device. */
        uint8_t h[4] = { len ? p[0] : 0, UR_INVAL, 0, 0 };
        send_packet(UR_INTERRUPT_PACKET, id, h, sizeof(h), NULL, 0);
        break;
    }
    case UR_FILTER_REJECT:
    case UR_FILTER_FILTER:
    case UR_DEVICE_DISCONNECT_ACK:
        break;
    default:
        /* Iso, bulk streams and bulk receiving are not advertised. */
        ur_trace("ignoring packet type %u len %u", type, len);
        break;
    }
}

static void peer_lost(const char *why)
{
    fprintf(stderr, "[USB_REDIR] host peer gone (%s)\n", why);
    device_disconnect(false);
    host_chardev_drop_peer(&g.ch);
    g.peer_hello = false;
    g.peer_caps = 0;
    g.rx_len = 0;
    g.tx_len = 0;
}

static void flush_tx(void)
{
    size_t off = 0;

    while (off < g.tx_len) {
        ssize_t n = host_chardev_write(&g.ch, g.tx + off, g.tx_len - off);

        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && host_chardev_errno_would_block())
            break;
        memmove(g.tx, g.tx + off, g.tx_len - off);
        g.tx_len -= off;
        peer_lost(n < 0 ? strerror(errno) : "write returned 0");
        return;
    }
    memmove(g.tx, g.tx + off, g.tx_len - off);
    g.tx_len -= off;
}

static void drain_rx(void)
{
    for (;;) {
        ssize_t n;

        if (!buf_reserve(&g.rx, &g.rx_cap, g.rx_len + 4096u))
            return;
        n = host_chardev_read(&g.ch, g.rx + g.rx_len, g.rx_cap - g.rx_len);
        if (n > 0) {
            g.rx_len += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && host_chardev_errno_would_block())
            break;
        peer_lost(n == 0 ? "EOF" : strerror(errno));
        return;
    }

    size_t pos = 0;
    while (g.ch.fd >= 0) {
        size_t hl = hdr_len();
        uint32_t type, len;
        uint64_t id;

        if (g.rx_len - pos < hl)
            break;
        type = le32(g.rx + pos);
        len = le32(g.rx + pos + 4);
        id = le32(g.rx + pos + 8);
        if (hl == 16u)
            id |= (uint64_t)le32(g.rx + pos + 12) << 32;
        if (len > UR_MAX_PAYLOAD) {
            peer_lost("oversized packet");
            return;
        }
        if (g.rx_len - pos < hl + len)
            break;
        handle_packet(type, id, g.rx + pos + hl, len);
        pos += hl + len;
    }
    if (g.ch.fd < 0)
        return;
    memmove(g.rx, g.rx + pos, g.rx_len - pos);
    g.rx_len -= pos;
}

/* ---------- public API ---------- */

bool usbredir_dev_configure(const usbredir_dev_config_t *cfg)
{
    if (!cfg || g.armed)
        return false;
    host_chardev_init(&g.ch, "USB_REDIR", cfg->trace);
    g.trace = cfg->trace;
    if (cfg->tee_path) {
        g.tee = fopen(cfg->tee_path, "w");
        if (!g.tee) {
            fprintf(stderr, "[USB_REDIR] open tee '%s': %s\n",
                cfg->tee_path, strerror(errno));
            return false;
        }
        setvbuf(g.tee, NULL, _IOLBF, 0);
        fprintf(g.tee, "# usbredir tee; mono_ns dir type id hdr: bytes "
            "[data[n]: bytes]; D>H = BE-300 to host\n");
    }
    if (!host_chardev_open(&g.ch, cfg->kind, cfg->target)) {
        host_chardev_close(&g.ch);
        if (g.tee) {
            fclose(g.tee);
            g.tee = NULL;
        }
        return false;
    }
    g.armed = true;
    return true;
}

void usbredir_dev_bind(usbf_state_t *u)
{
    g.u = u;
    if (!u)
        return;
    u->in_ready = on_in_ready;
    u->ctrl_done = on_ctrl_done;
    u->host_ctx = NULL;
}

bool usbredir_dev_active(void)
{
    return g.armed;
}

void usbredir_dev_set_present(bool present)
{
    if (g.present == present)
        return;
    g.present = present;
    ur_trace("cable %s", present ? "present" : "removed");
}

void usbredir_dev_tick(void)
{
    if (!g.armed)
        return;
    host_chardev_tick(&g.ch);
    if (!host_chardev_ready(&g.ch)) {
        if (g.connected)
            device_disconnect(false);
        return;
    }
    if (host_chardev_take_new_peer(&g.ch)) {
        g.peer_hello = false;
        g.peer_caps = 0;
        g.rx_len = 0;
        g.tx_len = 0;
        send_hello();
    }
    drain_rx();
    if (g.ch.fd < 0)
        return;
    update_connection();
    service_endpoints();
    flush_tx();
}

void usbredir_dev_shutdown(void)
{
    if (!g.armed)
        return;
    if (host_chardev_ready(&g.ch)) {
        device_disconnect(true);
        flush_tx();
    }
    drop_transfers();
    host_chardev_close(&g.ch);
    if (g.tee) {
        fclose(g.tee);
        g.tee = NULL;
    }
    free(g.rx);
    free(g.tx);
    memset(&g, 0, sizeof(g));
}
