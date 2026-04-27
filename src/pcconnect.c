#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pcconnect.h"

#define PCC_RX_CAP 4096u
#define PCC_TX_FRAME_CAP 512u
#define PCC_NS_PER_MS 1000000ull
#define PCC_DEFAULT_POST_READY_DELAY_MS 4u
#define PCC_MAX_POST_READY_DELAY_MS 5000u
#define PCC_DEFAULT_UART_NAME "vrc4173siu"
typedef enum {
    PCC_PENDING_NONE = 0,
    PCC_PENDING_SYNC_ECHO,
    PCC_PENDING_PROBE_FRAME,
} pcconnect_pending_action_t;

typedef struct {
    bool enabled;
    bool trace;
    bool cable_connected;
    bool guest_uart_ready;
    bool wake_queued;
    bool guest_ready_seen;
    bool probe_frame_queued;
    bool time_frame_queued;
    const char *uart_name;
    int guest_uart_divisor;
    uint8_t guest_uart_lcr;
    uint8_t guest_uart_mcr;
    uint8_t guest_uart_ier;
    unsigned guest_sync_seen;
    unsigned post_ready_sync_seen;
    unsigned post_ready_delay_ms;
    uint8_t rx[PCC_RX_CAP];
    size_t head;
    size_t tail;
    size_t count;
    uint8_t tx_frame[PCC_TX_FRAME_CAP];
    size_t tx_frame_len;
    size_t tx_frame_expected;
    pcconnect_pending_action_t pending_action;
    uint64_t pending_due_ns;
    void (*rx_ready_cb)(void *opaque);
    void *rx_ready_opaque;
} pcconnect_state_t;

static pcconnect_state_t g_pcc;

static bool env_enabled(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] != '\0' && strcmp(v, "0") != 0;
}

static const char *env_value(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] != '\0' ? v : NULL;
}

static unsigned env_uint(const char *name, unsigned fallback, unsigned max)
{
    const char *v = getenv(name);
    char *end = NULL;
    unsigned long parsed;

    if (!v || v[0] == '\0')
        return fallback;

    parsed = strtoul(v, &end, 0);
    if (end == v || *end != '\0' || parsed > max)
        return fallback;

    return (unsigned)parsed;
}

static void trace_byte(const char *dir, uint8_t byte)
{
    if (!g_pcc.trace)
        return;

    fprintf(stderr, "[PC_CONNECT] %s 0x%02x", dir, byte);
    if (isprint(byte))
        fprintf(stderr, " '%c'", byte);
    fputc('\n', stderr);
}

static void trace_message(const char *msg)
{
    if (g_pcc.trace)
        fprintf(stderr, "[PC_CONNECT] %s\n", msg);
}

static uint64_t host_monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;

    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void queue_host_byte(uint8_t byte)
{
    bool was_empty;

    if (!g_pcc.enabled)
        return;

    was_empty = g_pcc.count == 0;
    if (g_pcc.count == PCC_RX_CAP) {
        g_pcc.tail = (g_pcc.tail + 1u) % PCC_RX_CAP;
        g_pcc.count--;
    }

    g_pcc.rx[g_pcc.head] = byte;
    g_pcc.head = (g_pcc.head + 1u) % PCC_RX_CAP;
    g_pcc.count++;
    trace_byte("host->guest", byte);

    if (was_empty && g_pcc.rx_ready_cb)
        g_pcc.rx_ready_cb(g_pcc.rx_ready_opaque);
}

static void queue_host_bytes(const uint8_t *bytes, size_t len)
{
    for (size_t i = 0; i < len; i++)
        queue_host_byte(bytes[i]);
}

static void clear_rx_queue(void)
{
    g_pcc.head = 0;
    g_pcc.tail = 0;
    g_pcc.count = 0;
}

static void put_be16(uint8_t *p, unsigned value)
{
    p[0] = (uint8_t)((value >> 8) & 0xffu);
    p[1] = (uint8_t)(value & 0xffu);
}

static uint16_t sum_bytes(const uint8_t *bytes, size_t len)
{
    uint16_t sum = 0;

    for (size_t i = 0; i < len; i++)
        sum = (uint16_t)(sum + bytes[i]);

    return sum;
}

static size_t build_command_frame(uint8_t *frame, size_t cap, uint8_t seq,
                                  uint8_t opcode, const uint8_t *payload,
                                  size_t payload_len)
{
    size_t len = 10u + payload_len;
    uint16_t payload_sum;
    uint16_t header_sum;

    if (cap < len)
        return 0;

    frame[0] = 0x10;
    frame[1] = seq;
    put_be16(&frame[2], (unsigned)len);
    frame[4] = 0x03;
    frame[5] = opcode;
    memset(&frame[6], 0, 4);
    if (payload_len)
        memcpy(&frame[10], payload, payload_len);

    payload_sum = sum_bytes(&frame[10], payload_len);
    put_be16(&frame[6], payload_sum);
    header_sum = sum_bytes(frame, 8);
    put_be16(&frame[8], header_sum);

    return len;
}

static void queue_wake_sequence(void)
{
    static const uint8_t wake[] = {
        0x41, 0x54, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00,
        0x55, 0x55,
        0x41, 0x54, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00,
        0x55, 0x55,
    };

    queue_host_bytes(wake, sizeof(wake));
}

static void queue_probe_frame(void)
{
    uint8_t frame[10];
    size_t len = build_command_frame(frame, sizeof(frame), 0x01, 0x2d,
        NULL, 0);

    trace_message("queue PC Connect probe frame opcode=0x2d");
    queue_host_bytes(frame, len);
}

static void queue_set_system_time_frame(uint8_t seq)
{
    uint8_t frame[26];
    uint8_t payload[16];
    size_t len;
    time_t now = time(NULL);
    struct tm tm_utc;

#if defined(_WIN32)
    gmtime_s(&tm_utc, &now);
#else
    gmtime_r(&now, &tm_utc);
#endif

    /*
     * PC Connect's WilSetSystemTime frame uses big-endian SYSTEMTIME fields.
     * Setup.exe imports Win32 GetSystemTime, so the host-side value is UTC.
     */
    put_be16(&payload[0], (unsigned)tm_utc.tm_year + 1900u);
    put_be16(&payload[2], (unsigned)tm_utc.tm_mon + 1u);
    put_be16(&payload[4], (unsigned)tm_utc.tm_wday);
    put_be16(&payload[6], (unsigned)tm_utc.tm_mday);
    put_be16(&payload[8], (unsigned)tm_utc.tm_hour);
    put_be16(&payload[10], (unsigned)tm_utc.tm_min);
    put_be16(&payload[12], (unsigned)tm_utc.tm_sec);
    put_be16(&payload[14], 0);

    if (g_pcc.trace) {
        fprintf(stderr,
            "[PC_CONNECT] queue WilSetSystemTime seq=0x%02x UTC "
            "%04u-%02u-%02u %02u:%02u:%02u\n",
            seq,
            (unsigned)tm_utc.tm_year + 1900u,
            (unsigned)tm_utc.tm_mon + 1u,
            (unsigned)tm_utc.tm_mday,
            (unsigned)tm_utc.tm_hour,
            (unsigned)tm_utc.tm_min,
            (unsigned)tm_utc.tm_sec);
    }

    len = build_command_frame(frame, sizeof(frame), seq, 0x49,
        payload, sizeof(payload));
    queue_host_bytes(frame, len);
}

static void maybe_queue_probe_frame(void)
{
    if (g_pcc.probe_frame_queued)
        return;

    g_pcc.probe_frame_queued = true;
    queue_probe_frame();
}

static void maybe_queue_time_frame(uint8_t seq)
{
    if (g_pcc.time_frame_queued)
        return;

    g_pcc.time_frame_queued = true;
    queue_set_system_time_frame(seq);
}

static void reset_guest_frame_parser(void)
{
    g_pcc.tx_frame_len = 0;
    g_pcc.tx_frame_expected = 0;
}

static void arm_pending_host_action(pcconnect_pending_action_t action,
    unsigned delay_ms)
{
    if (g_pcc.pending_action != PCC_PENDING_NONE)
        return;

    g_pcc.pending_action = action;
    g_pcc.pending_due_ns = host_monotonic_ns() +
        (uint64_t)delay_ms * PCC_NS_PER_MS;

    if (g_pcc.trace) {
        const char *name = action == PCC_PENDING_SYNC_ECHO ?
            "post-ready sync echo" : "PC Connect probe";
        fprintf(stderr,
            "[PC_CONNECT] arm delayed %s in %u ms\n",
            name, delay_ms);
    }
}

static void service_pending_host_action(bool force)
{
    pcconnect_pending_action_t action;

    if (g_pcc.pending_action == PCC_PENDING_NONE)
        return;

    if (!force && host_monotonic_ns() < g_pcc.pending_due_ns)
        return;

    action = g_pcc.pending_action;
    g_pcc.pending_action = PCC_PENDING_NONE;
    g_pcc.pending_due_ns = 0;

    switch (action) {
    case PCC_PENDING_SYNC_ECHO:
        queue_host_byte(0x55);
        break;
    case PCC_PENDING_PROBE_FRAME:
        clear_rx_queue();
        trace_message("post-ready sync complete; queue PC Connect probe");
        maybe_queue_probe_frame();
        break;
    case PCC_PENDING_NONE:
        break;
    }
}

static void handle_guest_response_frame(void)
{
    uint8_t seq;
    uint8_t opcode;

    if (g_pcc.tx_frame_len < 10u || g_pcc.tx_frame[0] != 0x11)
        return;

    seq = g_pcc.tx_frame[1];
    opcode = g_pcc.tx_frame[5];

    if (g_pcc.trace) {
        fprintf(stderr,
            "[PC_CONNECT] guest response frame seq=0x%02x "
            "opcode=0x%02x len=%zu\n",
            seq, opcode, g_pcc.tx_frame_len);
    }

    if (seq == 0x01 && opcode == 0x2d)
        maybe_queue_time_frame(0x02);
}

static void feed_guest_frame_byte(uint8_t byte)
{
    if (g_pcc.tx_frame_len == 0) {
        if (byte == 0x55)
            return;
        if (byte != 0x11)
            return;
    }

    if (g_pcc.tx_frame_len == PCC_TX_FRAME_CAP) {
        reset_guest_frame_parser();
        return;
    }

    g_pcc.tx_frame[g_pcc.tx_frame_len++] = byte;

    if (g_pcc.tx_frame_len == 4u) {
        g_pcc.tx_frame_expected =
            ((size_t)g_pcc.tx_frame[2] << 8) | g_pcc.tx_frame[3];
        if (g_pcc.tx_frame_expected < 10u ||
            g_pcc.tx_frame_expected > PCC_TX_FRAME_CAP) {
            reset_guest_frame_parser();
            return;
        }
    }

    if (g_pcc.tx_frame_expected != 0 &&
        g_pcc.tx_frame_len == g_pcc.tx_frame_expected) {
        handle_guest_response_frame();
        reset_guest_frame_parser();
    }
}

static void maybe_queue_wake_sequence(void)
{
    if (!g_pcc.enabled || !g_pcc.cable_connected ||
        !g_pcc.guest_uart_ready || g_pcc.wake_queued)
        return;

    g_pcc.wake_queued = true;
    if (g_pcc.trace) {
        fprintf(stderr,
            "[PC_CONNECT] guest UART %s ready lcr=0x%02x mcr=0x%02x "
            "ier=0x%02x divisor=%d; queue wake sequence\n",
            g_pcc.uart_name ? g_pcc.uart_name : "",
            g_pcc.guest_uart_lcr, g_pcc.guest_uart_mcr,
            g_pcc.guest_uart_ier, g_pcc.guest_uart_divisor);
    }
    queue_wake_sequence();
}

void pcconnect_configure(bool enabled)
{
    memset(&g_pcc, 0, sizeof(g_pcc));
    g_pcc.enabled = enabled;
    g_pcc.trace = enabled && env_enabled("BE300_PCC_TRACE");
    g_pcc.uart_name = env_value("BE300_PCC_UART");
    g_pcc.post_ready_delay_ms = env_uint("BE300_PCC_POST_READY_DELAY_MS",
        PCC_DEFAULT_POST_READY_DELAY_MS, PCC_MAX_POST_READY_DELAY_MS);
    if (!g_pcc.uart_name)
        g_pcc.uart_name = PCC_DEFAULT_UART_NAME;

    if (!enabled)
        return;

    if (g_pcc.trace) {
        fprintf(stderr,
            "[PC_CONNECT] experimental serial peer enabled on %s; "
            "post-ready delay=%u ms; waiting for cable connect and guest "
            "sync before WilSetSystemTime\n",
            g_pcc.uart_name, g_pcc.post_ready_delay_ms);
    }
}

void pcconnect_set_rx_ready_callback(void (*cb)(void *opaque), void *opaque)
{
    g_pcc.rx_ready_cb = cb;
    g_pcc.rx_ready_opaque = opaque;
}

void pcconnect_set_cable_connected(bool connected)
{
    if (!g_pcc.enabled)
        return;

    if (!connected)
        g_pcc.guest_uart_ready = false;

    if (g_pcc.cable_connected == connected)
        return;

    g_pcc.cable_connected = connected;
    clear_rx_queue();
    g_pcc.guest_sync_seen = 0;
    g_pcc.post_ready_sync_seen = 0;
    g_pcc.wake_queued = false;
    g_pcc.guest_ready_seen = false;
    g_pcc.probe_frame_queued = false;
    g_pcc.time_frame_queued = false;
    g_pcc.pending_action = PCC_PENDING_NONE;
    g_pcc.pending_due_ns = 0;
    reset_guest_frame_parser();

    if (g_pcc.trace) {
        fprintf(stderr, "[PC_CONNECT] cable %s\n",
            connected ? "connected" : "disconnected");
    }

    if (connected) {
        if (g_pcc.guest_uart_ready)
            maybe_queue_wake_sequence();
        else
            trace_message("waiting for guest UART open before wake sequence");
    }
}

bool pcconnect_ns16550_claims(const char *name)
{
    if (!g_pcc.enabled || !name)
        return false;

    if (strcmp(g_pcc.uart_name, "auto") == 0)
        return strcmp(name, "siu") == 0 || strcmp(name, "vrc4173siu") == 0;

    return strcmp(name, g_pcc.uart_name) == 0;
}

bool pcconnect_cable_connected(void)
{
    return g_pcc.enabled && g_pcc.cable_connected;
}

bool pcconnect_guest_uart_ready(void)
{
    return g_pcc.enabled && g_pcc.guest_uart_ready;
}

bool pcconnect_trace_enabled(void)
{
    return g_pcc.trace;
}

void pcconnect_note_uart_config(const char *name, uint8_t lcr, uint8_t mcr,
    uint8_t ier, int divisor, int dlab)
{
    if (!g_pcc.enabled)
        return;

    if (!pcconnect_ns16550_claims(name))
        return;

    /*
     * The PC-side PortMon captures open the link at 115200 8N1, then send
     * the AT/0x55 wake train.  The guest-side driver programs a board-specific
     * divisor, so don't infer host baud from the generic ns16550 divisor math.
     * The real BE-300 PC Connect serial path is the custom VRC4173 companion
     * SIU (hardware.txt:8, 190-191).  serial.dll first touches that UART in
     * 8N1 with MCR clear during driver initialization, then the later COM
     * open path asserts DTR/RTS.  Wait for DTR/RTS so the host wake sequence
     * is not injected while the driver is still only probing the port.
     */
    if (dlab || (lcr & 0x7f) != 0x03 || (mcr & 0x03) == 0)
        return;

    g_pcc.guest_uart_lcr = lcr;
    g_pcc.guest_uart_mcr = mcr;
    g_pcc.guest_uart_ier = ier;
    g_pcc.guest_uart_divisor = divisor;
    if (!g_pcc.guest_uart_ready && g_pcc.trace) {
        fprintf(stderr,
            "[PC_CONNECT] guest UART %s asserted DTR/RTS in 8N1 "
            "divisor=%d; ready for cable edge\n",
            name ? name : "", divisor);
    }
    g_pcc.guest_uart_ready = true;
    maybe_queue_wake_sequence();
}

void pcconnect_trace_uart_access(const char *name, bool claimed,
    bool write, unsigned reg, uint8_t value, uint32_t pc,
    uint8_t ier, uint8_t iir, uint8_t lcr, uint8_t mcr,
    uint8_t lsr, uint8_t msr, int dlab, int divisor)
{
    if (!g_pcc.trace)
        return;

    fprintf(stderr,
        "[PC_CONNECT_UART] %s %s reg=%u val=0x%02x pc=0x%08x "
        "claimed=%u ier=0x%02x iir=0x%02x lcr=0x%02x mcr=0x%02x "
        "lsr=0x%02x msr=0x%02x dlab=%d divisor=%d\n",
        name ? name : "", write ? "write" : "read", reg, value, pc,
        claimed ? 1u : 0u, ier, iir, lcr, mcr, lsr, msr, dlab, divisor);
}

bool pcconnect_uart_rx_available(void)
{
    if (!g_pcc.enabled || !g_pcc.cable_connected)
        return false;

    service_pending_host_action(false);
    return g_pcc.count > 0;
}

int pcconnect_uart_rx_pop(void)
{
    uint8_t byte;

    if (!pcconnect_uart_rx_available())
        return -1;

    byte = g_pcc.rx[g_pcc.tail];
    g_pcc.tail = (g_pcc.tail + 1u) % PCC_RX_CAP;
    g_pcc.count--;
    trace_byte("guest read", byte);
    return byte;
}

void pcconnect_uart_tx_byte(uint8_t byte)
{
    if (!g_pcc.enabled)
        return;

    if (!g_pcc.cable_connected)
        return;

    trace_byte("guest->host", byte);

    if (!g_pcc.guest_ready_seen) {
        if (byte == 0x55) {
            g_pcc.guest_sync_seen++;
            if (g_pcc.guest_sync_seen <= 32u)
                queue_host_byte(0x55);
        /*
         * The guest also emits printable serial status text on this UART
         * (for example "Serial port open").  The captured PC Connect ready
         * byte appears only after the wake phase has produced sync bytes, so
         * require at least one 0x55 before treating 0x20 as protocol state.
         */
        } else if (byte == 0x20 && g_pcc.guest_sync_seen != 0) {
            trace_message(
                "guest ready byte 0x20 received; waiting for post-ready sync");
            g_pcc.guest_ready_seen = true;
            g_pcc.post_ready_sync_seen = 0;
            clear_rx_queue();
            reset_guest_frame_parser();
        }
        return;
    }

    if (!g_pcc.probe_frame_queued && byte == 0x55) {
        service_pending_host_action(false);
        if (g_pcc.count > 0) {
            trace_message("discard stale post-ready sync echo before next phase");
            clear_rx_queue();
        }
        if (g_pcc.pending_action != PCC_PENDING_NONE)
            return;

        g_pcc.post_ready_sync_seen++;
        if (g_pcc.post_ready_sync_seen <= 2u) {
            arm_pending_host_action(PCC_PENDING_SYNC_ECHO,
                g_pcc.post_ready_delay_ms);
        } else if (g_pcc.post_ready_sync_seen == 3u) {
            arm_pending_host_action(PCC_PENDING_PROBE_FRAME,
                g_pcc.post_ready_delay_ms);
        }
        return;
    }

    feed_guest_frame_byte(byte);
}
