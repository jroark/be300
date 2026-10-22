/*
 * pcconnect_bridge — transparent serial bridge between an emulated SIU
 * UART (VR4131 main "siu" at PA 0x0F000800, or VRC4173 companion
 * "vrc4173siu" at PA 0xAA008680) and a host chardev (TCP socket / Unix
 * socket / PTY).
 *
 * Two bridge slots are maintained, indexed by pcc_bridge_uart_id_t. The
 * legacy --pcconnect-bridge flag configures the VRC4173 slot with
 * wince_mode=true (cable lifecycle, PCConnect.exe polling-idle drop,
 * CommMode latch poke). --serial0-bridge / --serial1-bridge configure
 * either slot with wince_mode=false for plain Linux serial console use.
 *
 * This models the dock's RS-232 cable: WinCE drives a plain 115200 8N1
 * UART, so the bridge is a byte pipe and real PCConnect.exe on a UTM Windows
 * VM (serial port on a chardev) drives the sync handshake at the other end.
 * The USB cable does not use the SIU; see usbredir_dev.c.  The host socket /
 * PTY layer lives in host_chardev.c.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include "win32_compat.h"
#endif

#include "be300.h"
#include "host_time.h"
#include "pcconnect_bridge.h"
#include "pcconnect_ring.h"

#define PCC_PREOPEN_FIFO_CAP 64u
#define PCC_GUEST_RX_FIFO_CAP 16u
#define PCC_RX_PACE_BACKLOG_THRESHOLD 512u
/*
 * Guest-to-host queue. The emulated SIU reports THRE as always ready, and
 * serial.dll on the companion SIU sends one byte per TX interrupt, so the
 * guest can queue a frame far faster than the paced drain sends it. A
 * PCConnect frame's length field is 16 bits (frames are at most 64 KB), so
 * hold several whole frames instead of overwriting unsent bytes.
 */
#define PCC_TX_QUEUE_CAP (256u * 1024u)

typedef struct pcc_br_state {
    bool armed;
    bool trace;
    bool wince_mode;
    pcc_bridge_uart_id_t uart_id;
    pcc_bridge_kind_t kind;
    char *target;
    const char *uart_name;          /* points into name_storage[] */
    char name_storage[32];

    /* Host side: socket / PTY peer, listener and redial state. */
    host_chardev_t ch;

    /* Physical cable plus guest-visible serial data path readiness. */
    bool cable_connected;
    bool guest_link_connected;
    bool guest_uart_ready;
    /* Guest has configured the UART at least once since the last dock edge
     * or guest reset. Until then only a UART-sized tail of host bytes is
     * retained; afterwards a transient DLAB/LCR reprogram pauses delivery
     * without discarding bytes already on the wire. */
    bool guest_uart_configured;
    bool drop_initial_rx_idle;
    uint8_t guest_uart_lcr;
    uint8_t guest_uart_mcr;
    uint8_t guest_uart_ier;
    int guest_uart_divisor;
    uint32_t baud;

    pcc_ring_t rx_ring;     /* host -> guest */
    pcc_ring_t tx_ring;     /* guest -> host */

    /* G->H baud-rate throttle. 0 = unlimited. Otherwise wait at least
     * tx_ns_per_byte between successive bytes so PCConnect sees inter-byte
     * gaps similar to real serial. */
    uint64_t tx_ns_per_byte;
    uint64_t tx_next_due_ns;

    /* H->G inter-byte throttle for large host bursts. Same configured rate as
     * TX. Normal startup/poll frames remain FIFO-driven; when rx_pace_active
     * is armed, rx_pre_ring drains to rx_ring at serial cadence. */
    uint64_t rx_ns_per_byte;
    uint64_t rx_next_due_ns;
    bool rx_pace_active;
    pcc_ring_t rx_pre_ring;     /* TCP-arrived bytes pending release to rx_ring */

    void (*rx_ready_cb)(void *);
    void *rx_ready_opaque;

    /* tee log */
    FILE *tee;
    char *tee_path;
} pcc_br_state_t;

static pcc_br_state_t bridges[PCC_UART_COUNT];

static const char *default_uart_name_for(pcc_bridge_uart_id_t id)
{
    switch (id) {
    case PCC_UART_VR4131_SIU:  return "siu";
    case PCC_UART_VRC4173_SIU: return "vrc4173siu";
    default:                   return "";
    }
}

static pcc_br_state_t *bridge_by_id(pcc_bridge_uart_id_t id)
{
    if ((unsigned)id >= PCC_UART_COUNT)
        return NULL;
    return &bridges[id];
}

static pcc_br_state_t *bridge_by_name(const char *name)
{
    if (!name)
        return NULL;
    for (int i = 0; i < PCC_UART_COUNT; i++) {
        pcc_br_state_t *s = &bridges[i];
        if (!s->armed)
            continue;
        if (s->uart_name && strcmp(s->uart_name, name) == 0)
            return s;
    }
    return NULL;
}

static pcc_br_state_t *wince_bridge(void)
{
    for (int i = 0; i < PCC_UART_COUNT; i++) {
        pcc_br_state_t *s = &bridges[i];
        if (s->armed && s->wince_mode)
            return s;
    }
    return NULL;
}

/* ---------- helpers ---------- */

static const char *bridge_tag(const pcc_br_state_t *s)
{
    /* serial0 / serial1 for stderr/tee logs; legacy name for WinCE bridge. */
    if (s->wince_mode)
        return "PC_CONNECT_BRIDGE";
    switch (s->uart_id) {
    case PCC_UART_VR4131_SIU:  return "SERIAL0_BRIDGE";
    case PCC_UART_VRC4173_SIU: return "SERIAL1_BRIDGE";
    default:                   return "SERIAL_BRIDGE";
    }
}

static void tee_write_dir(pcc_br_state_t *s, const char *tag,
    const uint8_t *buf, size_t len)
{
    if (!s->tee || len == 0)
        return;
    fprintf(s->tee, "%llu %s %zu:", (unsigned long long)be300_host_monotonic_ns(), tag, len);
    for (size_t i = 0; i < len; i++)
        fprintf(s->tee, " %02x", buf[i]);
    fputc('\n', s->tee);
    fflush(s->tee);
}

static void bridge_trace(pcc_br_state_t *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void bridge_trace(pcc_br_state_t *s, const char *fmt, ...)
{
    if (!s->trace)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", bridge_tag(s));
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static bool queue_host_rx_pre(pcc_br_state_t *s, uint8_t byte)
{
    if (!s->guest_uart_configured &&
        pcc_ring_count(&s->rx_pre_ring) >= PCC_PREOPEN_FIFO_CAP)
        (void)pcc_ring_pop(&s->rx_pre_ring);
    if (pcc_ring_is_full(&s->rx_pre_ring))
        return false;
    pcc_ring_push(&s->rx_pre_ring, byte);
    return true;
}

static void clear_serial_queues(pcc_br_state_t *s)
{
    pcc_ring_clear(&s->rx_ring);
    pcc_ring_clear(&s->rx_pre_ring);
    pcc_ring_clear(&s->tx_ring);
    s->rx_next_due_ns = 0;
    s->tx_next_due_ns = 0;
    s->rx_pace_active = false;
}

static void arm_initial_rx_idle_drop(pcc_br_state_t *s)
{
    if (!s->wince_mode)
        return;
    s->drop_initial_rx_idle = true;
}

/* ---------- config / spec parsing ---------- */

bool pcconnect_bridge_parse_spec(const char *spec, pcc_bridge_config_t *out)
{
    if (!spec || !out)
        return false;
    memset(out, 0, sizeof(*out));
    return host_chardev_parse_spec(spec, &out->kind, &out->target);
}

/* ---------- I/O helpers ---------- */

static void on_peer_gone(pcc_br_state_t *s, const char *why)
{
    if (s->ch.fd < 0)
        return;
    bridge_trace(s, "peer gone (%s); closing fd %lld; cable remains %s",
        why, (long long)s->ch.fd,
        s->cable_connected ? "connected" : "disconnected");
    host_chardev_drop_peer(&s->ch);
    /*
     * Cable presence is a cradle state, not the lifetime of a TCP/Unix/PTY
     * peer. A 2026-05-06 PortMon connect-only capture of PCConnect.exe and
     * PCLSTART.exe showed repeated COM open/close and purge cycles during
     * startup; bytes sent while no host endpoint is attached should not be
     * replayed into the next open. Drop transient FIFOs but leave the
     * docked level intact so the guest remains connected.
     */
    clear_serial_queues(s);
}

/*
 * True once the host stream is usable.  A newly accepted or connected peer
 * starts with empty serial queues.
 */
static bool bridge_finish_connect(pcc_br_state_t *s)
{
    bool ready = host_chardev_ready(&s->ch);

    if (host_chardev_take_new_peer(&s->ch))
        clear_serial_queues(s);
    return ready;
}

static void bridge_note_rx_released(pcc_br_state_t *s, bool was_empty,
    bool released)
{
    /* rx_ready_cb is the WinCE CommMode latch poke (be300_pcconnect_irq_update);
     * for non-WinCE bridges, ns16550's polling tick raises the plain IRQ
     * directly via INTERRUPT_ASSERT, so the callback is unwanted. */
    if (!s->wince_mode)
        return;
    if (released && was_empty && s->rx_ready_cb)
        s->rx_ready_cb(s->rx_ready_opaque);
}

static size_t bridge_guest_rx_space(pcc_br_state_t *s)
{
    size_t count = pcc_ring_count(&s->rx_ring);

    if (count >= PCC_GUEST_RX_FIFO_CAP)
        return 0;
    return PCC_GUEST_RX_FIFO_CAP - count;
}

static bool bridge_guest_rx_push(pcc_br_state_t *s, uint8_t byte)
{
    if (bridge_guest_rx_space(s) == 0)
        return false;
    pcc_ring_push(&s->rx_ring, byte);
    return true;
}

static bool bridge_data_path_connected(const pcc_br_state_t *s)
{
    return s->cable_connected && s->guest_link_connected;
}

/* Move bytes from rx_pre_ring (TCP arrival queue) into rx_ring
 * (guest-visible queue), pacing at rx_ns_per_byte when requested. The
 * companion wake is an empty->non-empty edge; once RX is already pending,
 * additional host poll bytes should not generate a CommMode IRQ storm. */
static void bridge_release_rx_paced(pcc_br_state_t *s)
{
    bool was_empty;
    bool released = false;
    bool pace;

    if (!bridge_data_path_connected(s) || !s->guest_uart_ready)
        return;

    if (s->rx_ns_per_byte != 0 &&
        pcc_ring_count(&s->rx_pre_ring) >= PCC_RX_PACE_BACKLOG_THRESHOLD)
        s->rx_pace_active = true;

    pace = s->rx_ns_per_byte != 0 && s->rx_pace_active;
    if (!pace) {
        if (pcc_ring_is_empty(&s->rx_pre_ring))
            return;
        was_empty = pcc_ring_is_empty(&s->rx_ring);
        while (!pcc_ring_is_empty(&s->rx_pre_ring) &&
               bridge_guest_rx_space(s) > 0) {
            int b = pcc_ring_pop(&s->rx_pre_ring);
            if (b < 0) break;
            released |= bridge_guest_rx_push(s, (uint8_t)b);
        }
        bridge_note_rx_released(s, was_empty, released);
        return;
    }

    if (pcc_ring_is_empty(&s->rx_pre_ring))
        return;
    was_empty = pcc_ring_is_empty(&s->rx_ring);
    uint64_t now = be300_host_monotonic_ns();
    if (s->rx_next_due_ns == 0 || s->rx_next_due_ns + 100000000ull < now)
        s->rx_next_due_ns = now;
    while (!pcc_ring_is_empty(&s->rx_pre_ring) && now >= s->rx_next_due_ns &&
           bridge_guest_rx_space(s) > 0) {
        int b = pcc_ring_pop(&s->rx_pre_ring);
        if (b < 0) break;
        released |= bridge_guest_rx_push(s, (uint8_t)b);
        s->rx_next_due_ns += s->rx_ns_per_byte;
        now = be300_host_monotonic_ns();
    }
    bridge_note_rx_released(s, was_empty, released);
    if (pcc_ring_is_empty(&s->rx_pre_ring))
        s->rx_pace_active = false;
}

static void bridge_drain_rx(pcc_br_state_t *s)
{
    if (s->ch.fd < 0 || !bridge_finish_connect(s)) {
        bridge_release_rx_paced(s);
        return;
    }

    uint8_t scratch[1024];
    bridge_release_rx_paced(s);
    for (;;) {
        size_t cap = pcc_ring_space(&s->rx_pre_ring);
        if (cap == 0)
            break;
        if (cap > sizeof(scratch))
            cap = sizeof(scratch);
        ssize_t n = host_chardev_read(&s->ch, scratch, cap);
        if (n > 0) {
            size_t off = 0;
            size_t len = (size_t)n;

            if (!bridge_data_path_connected(s)) {
                /*
                 * PCConnect may poll the VM's serial port before the BE-300 is
                 * guest-visible as docked. Real serial hardware has only a
                 * small UART FIFO, and serial.dll clears it on COM open; do
                 * not replay an unbounded pre-dock or reset-time polling
                 * backlog into AtPcCnct.
                 */
                tee_write_dir(s, "H>G:drop", scratch, (size_t)n);
                continue;
            }
            if (s->drop_initial_rx_idle) {
                /*
                 * The PC side may have been polling its COM port before the
                 * BE-300 dock edge. Real cable insertion does not replay those
                 * pre-connect serial-idle marks into the guest UART; expose
                 * the next non-idle poll burst instead.
                 */
                while (off < len && scratch[off] == 0x55)
                    off++;
                if (off > 0)
                    tee_write_dir(s, "H>G:drop-idle", scratch, off);
                if (off == len)
                    continue;
                s->drop_initial_rx_idle = false;
            }
            for (size_t i = off; i < len; i++) {
                if (!queue_host_rx_pre(s, scratch[i])) {
                    bridge_trace(s, "host RX backlog full; applying fd backpressure");
                    break;
                }
            }
            tee_write_dir(s, s->guest_uart_ready ? "H>G" : "H>G:queued",
                &scratch[off], len - off);
            continue;
        }
        if (n == 0) {
            if (s->kind == PCC_BR_PTY) {
                on_peer_gone(s, "PTY EOF");
                host_chardev_reopen_pty(&s->ch);
            } else {
                on_peer_gone(s, "EOF");
            }
            break;
        }
        if (host_chardev_errno_would_block())
            break;
        if (errno == EINTR)
            continue;
        on_peer_gone(s, strerror(errno));
        break;
    }
    bridge_release_rx_paced(s);
}

static void bridge_drain_tx(pcc_br_state_t *s)
{
    if (s->ch.fd < 0) {
        pcc_ring_clear(&s->tx_ring);
        return;
    }
    if (!bridge_finish_connect(s))
        return;

    /* Baud-throttled path: emit one byte per tx_ns_per_byte interval. */
    if (s->tx_ns_per_byte > 0) {
        uint64_t now = be300_host_monotonic_ns();
        if (s->tx_next_due_ns == 0)
            s->tx_next_due_ns = now;
        while (!pcc_ring_is_empty(&s->tx_ring) && now >= s->tx_next_due_ns) {
            /* Pop only once the byte is written: EINTR retries it, and a
             * short or would-block write leaves it queued. */
            uint8_t byte = (uint8_t)pcc_ring_peek(&s->tx_ring);
            ssize_t n = host_chardev_write(&s->ch, &byte, 1);
            if (n == 1) {
                (void)pcc_ring_pop(&s->tx_ring);
                s->tx_next_due_ns += s->tx_ns_per_byte;
                if (s->tx_next_due_ns + 10ull * s->tx_ns_per_byte < now)
                    s->tx_next_due_ns = now;
                now = be300_host_monotonic_ns();
                continue;
            }
            if (n < 0) {
                if (host_chardev_errno_would_block())
                    return;
                if (errno == EINTR)
                    continue;
                on_peer_gone(s, strerror(errno));
                pcc_ring_clear(&s->tx_ring);
                return;
            }
            return;
        }
        return;
    }

    /* Unthrottled bulk path. */
    while (!pcc_ring_is_empty(&s->tx_ring)) {
        size_t count = pcc_ring_count(&s->tx_ring);
        size_t contig = s->tx_ring.cap - s->tx_ring.tail;
        if (contig > count)
            contig = count;
        ssize_t n = host_chardev_write(&s->ch, &s->tx_ring.buf[s->tx_ring.tail],
            contig);
        if (n > 0) {
            s->tx_ring.tail = (s->tx_ring.tail + (size_t)n) % s->tx_ring.cap;
            s->tx_ring.count -= (size_t)n;
            continue;
        }
        if (n < 0) {
            if (host_chardev_errno_would_block())
                return;
            if (errno == EINTR)
                continue;
            on_peer_gone(s, strerror(errno));
            pcc_ring_clear(&s->tx_ring);
            return;
        }
        return;
    }
}

static bool bridge_guest_tx_open(const pcc_br_state_t *s)
{
    return s && bridge_data_path_connected(s) && s->ch.fd >= 0 &&
        !s->ch.connecting;
}

static void bridge_queue_guest_tx(pcc_br_state_t *s, uint8_t byte)
{
    if (!bridge_guest_tx_open(s)) {
        if (s && s->tee)
            tee_write_dir(s, "G>H:drop", &byte, 1);
        return;
    }
    if (pcc_ring_is_full(&s->tx_ring)) {
        /*
         * More than PCC_TX_QUEUE_CAP unsent bytes: drop the new byte
         * visibly rather than overwrite bytes not yet on the wire.
         */
        bridge_trace(s, "guest TX overflow; dropping byte 0x%02x", byte);
        tee_write_dir(s, "G>H:overflow", &byte, 1);
        return;
    }
    pcc_ring_push(&s->tx_ring, byte);
    tee_write_dir(s, "G>H", &byte, 1);
}

static void bridge_tick_one(pcc_br_state_t *s)
{
    if (!s->armed)
        return;
    host_chardev_tick(&s->ch);
    if (host_chardev_take_new_peer(&s->ch))
        clear_serial_queues(s);
    bridge_drain_tx(s);
    bridge_drain_rx(s);
}

static void bridge_free_rings(pcc_br_state_t *s)
{
    pcc_ring_free(&s->rx_ring);
    pcc_ring_free(&s->rx_pre_ring);
    pcc_ring_free(&s->tx_ring);
}

static void bridge_shutdown_one(pcc_br_state_t *s)
{
    if (s->tee) {
        fflush(s->tee);
        fclose(s->tee);
        s->tee = NULL;
    }
    host_chardev_close(&s->ch);
    free(s->target);
    s->target = NULL;
    free(s->tee_path);
    s->tee_path = NULL;
    bridge_free_rings(s);
    s->armed = false;
}

/* ---------- public API ---------- */

bool pcconnect_bridge_configure(const pcc_bridge_config_t *cfg)
{
    pcc_br_state_t *s;
    const char *name;

    if (!cfg)
        return false;
    s = bridge_by_id(cfg->uart_id);
    if (!s) {
        fprintf(stderr, "pcconnect-bridge: bad uart_id %d\n",
            (int)cfg->uart_id);
        return false;
    }
    if (s->armed) {
        fprintf(stderr, "pcconnect-bridge: %s already configured\n",
            bridge_tag(s));
        return false;
    }

    memset(s, 0, sizeof(*s));
    if (!pcc_ring_init(&s->rx_ring, PCC_RING_CAP) ||
        !pcc_ring_init(&s->rx_pre_ring, PCC_RING_CAP) ||
        !pcc_ring_init(&s->tx_ring, PCC_TX_QUEUE_CAP)) {
        fprintf(stderr, "pcconnect-bridge: oom\n");
        bridge_free_rings(s);
        return false;
    }
    s->uart_id = cfg->uart_id;
    s->wince_mode = cfg->wince_mode;
    host_chardev_init(&s->ch, bridge_tag(s), cfg->trace);

    s->kind = cfg->kind;
    s->target = cfg->target ? strdup(cfg->target) : NULL;
    name = cfg->uart_name ? cfg->uart_name : default_uart_name_for(cfg->uart_id);
    strncpy(s->name_storage, name, sizeof(s->name_storage) - 1);
    s->name_storage[sizeof(s->name_storage) - 1] = '\0';
    s->uart_name = s->name_storage;
    s->trace = cfg->trace;
    s->baud = cfg->baud;
    if (cfg->baud > 0) {
        /* 8N1 framing = 10 bit-times per byte. */
        s->tx_ns_per_byte = 10000000000ull / (uint64_t)cfg->baud;
    } else {
        s->tx_ns_per_byte = 0;
    }
    s->rx_ns_per_byte = cfg->baud > 0 ?
        10000000000ull / (uint64_t)cfg->baud : 0;
    s->tx_next_due_ns = 0;
    s->rx_next_due_ns = 0;
    s->rx_pace_active = false;

    if (cfg->tee_path) {
        s->tee = fopen(cfg->tee_path, "w");
        if (!s->tee) {
            fprintf(stderr, "pcconnect-bridge: open tee '%s': %s\n",
                    cfg->tee_path, strerror(errno));
            free(s->target);
            s->target = NULL;
            bridge_free_rings(s);
            return false;
        }
        s->tee_path = strdup(cfg->tee_path);
        setvbuf(s->tee, NULL, _IOLBF, 0);
        fprintf(s->tee,
            "# pcconnect-bridge tee log; mono_ns dir count: hex bytes\n");
        fflush(s->tee);
    }

    bool ok = host_chardev_open(&s->ch, s->kind, s->target);
    if (!ok) {
        host_chardev_close(&s->ch);
        if (s->tee) { fclose(s->tee); s->tee = NULL; }
        free(s->target); s->target = NULL;
        free(s->tee_path); s->tee_path = NULL;
        bridge_free_rings(s);
        return false;
    }

    /*
     * For Linux-mode bridges, treat the cable as permanently connected
     * and the guest UART as ready immediately. WinCE-mode bridges rely on
     * be300_pcconnect_raise_dock_edge() / serial.dll's COM open to set
     * these later.
     */
    if (!s->wince_mode) {
        s->cable_connected = true;
        s->guest_link_connected = true;
        s->guest_uart_ready = true;
        s->guest_uart_configured = true;
    }

    s->armed = true;
    bridge_trace(s, "armed kind=%d target=%s uart=%s tee=%s wince=%d",
          (int)s->kind, s->target ? s->target : "(null)",
          s->uart_name, s->tee_path ? s->tee_path : "(none)",
          s->wince_mode ? 1 : 0);
    return true;
}

void pcconnect_bridge_tick(void)
{
    for (int i = 0; i < PCC_UART_COUNT; i++)
        bridge_tick_one(&bridges[i]);
}

void pcconnect_bridge_shutdown(void)
{
    for (int i = 0; i < PCC_UART_COUNT; i++)
        bridge_shutdown_one(&bridges[i]);
}

/* ---------- legacy WinCE-bridge entry points (PCC_UART_VRC4173_SIU,
 *            wince_mode=true) ---------- */

bool pcconnect_bridge_ns16550_claims(const char *name)
{
    pcc_br_state_t *s = wince_bridge();
    if (!s || !name)
        return false;
    return strcmp(name, s->uart_name) == 0;
}

/*
 * The UART register accessors below run on guest LSR/RBR reads, so they
 * only release bytes already received; the socket is read once per batch
 * by pcconnect_bridge_tick().
 */
bool pcconnect_bridge_uart_rx_available(void)
{
    pcc_br_state_t *s = wince_bridge();
    if (!s)
        return false;
    bridge_release_rx_paced(s);
    if (!bridge_data_path_connected(s) || !s->guest_uart_ready)
        return false;
    return !pcc_ring_is_empty(&s->rx_ring);
}

size_t pcconnect_bridge_uart_rx_count(void)
{
    pcc_br_state_t *s = wince_bridge();
    if (!s)
        return 0;
    bridge_release_rx_paced(s);
    if (!bridge_data_path_connected(s) || !s->guest_uart_ready)
        return 0;
    return pcc_ring_count(&s->rx_ring);
}

int pcconnect_bridge_uart_rx_pop(void)
{
    pcc_br_state_t *s = wince_bridge();
    if (!s)
        return -1;
    if (!pcconnect_bridge_uart_rx_available())
        return -1;
    return pcc_ring_pop(&s->rx_ring);
}

/*
 * FCR RCV_RST (VR4131 UM §20.2.7 SIUFC) resets the receive FIFO only.
 * Host bytes still queued in rx_pre_ring model data that has not reached
 * the BE-300's RX pin yet; a real FIFO reset cannot discard those, so keep
 * them and let them arrive after the reset.
 */
static void bridge_guest_rx_fifo_reset(pcc_br_state_t *s)
{
    if (!s)
        return;
    if (!pcc_ring_is_empty(&s->rx_ring))
        bridge_trace(s, "guest RX FIFO reset; discarding %zu unread byte(s)",
            pcc_ring_count(&s->rx_ring));
    pcc_ring_clear(&s->rx_ring);
}

void pcconnect_bridge_uart_rx_clear(void)
{
    bridge_guest_rx_fifo_reset(wince_bridge());
}

void pcconnect_bridge_uart_tx_byte(uint8_t byte)
{
    bridge_queue_guest_tx(wince_bridge(), byte);
}

uint32_t pcconnect_bridge_uart_baud(void)
{
    pcc_br_state_t *s = wince_bridge();
    if (!s)
        return 0;
    return s->baud != 0 ? s->baud : 115200u;
}

void pcconnect_bridge_set_cable_connected(bool connected)
{
    pcc_br_state_t *s = wince_bridge();
    bool changed;

    if (!s)
        return;

    if (!connected) {
        if (!s->cable_connected && !s->guest_link_connected)
            return;
        s->cable_connected = false;
        s->guest_link_connected = false;
        s->guest_uart_ready = false;
        s->guest_uart_configured = false;
        clear_serial_queues(s);
        arm_initial_rx_idle_drop(s);
        bridge_trace(s, "cable disconnected");
        return;
    }

    changed = !s->cable_connected || !s->guest_link_connected;
    s->cable_connected = true;
    s->guest_link_connected = true;
    if (changed) {
        arm_initial_rx_idle_drop(s);
        bridge_release_rx_paced(s);
        bridge_trace(s, "cable connected");
    }
}

void pcconnect_bridge_reset_guest_serial(void)
{
    pcc_br_state_t *s = wince_bridge();
    if (!s)
        return;
    s->guest_link_connected = false;
    s->guest_uart_ready = false;
    s->guest_uart_configured = false;
    clear_serial_queues(s);
    arm_initial_rx_idle_drop(s);
    bridge_trace(s, "guest serial reset; cable %s",
        s->cable_connected ? "connected" : "disconnected");
}

void pcconnect_bridge_set_rx_ready_callback(void (*cb)(void *opaque),
    void *opaque)
{
    pcc_br_state_t *s = wince_bridge();
    if (!s) {
        /* Stash on the VRC4173 slot anyway so a later pcconnect configure
         * picks it up if armed later in the boot sequence. */
        s = bridge_by_id(PCC_UART_VRC4173_SIU);
        if (!s)
            return;
    }
    s->rx_ready_cb = cb;
    s->rx_ready_opaque = opaque;
}

void pcconnect_bridge_note_uart_config(const char *name, uint8_t lcr,
    uint8_t mcr, uint8_t ier, int divisor, int dlab)
{
    pcc_br_state_t *s = wince_bridge();
    bool ready;

    if (!s)
        return;
    if (!pcconnect_bridge_ns16550_claims(name))
        return;
    /*
     * The receiver is usable once the guest has selected 8N1 and left the
     * divisor latch. While the guest is not ready, bridge_drain_rx retains
     * only a small UART-sized tail of the PC's polling stream so opening COM
     * cannot replay an unbounded reset-time backlog.
     */
    ready = !dlab && (lcr & 0x7f) == 0x03;
    if (!ready) {
        if (s->guest_uart_ready) {
            bridge_trace(s, "guest UART %s receiver not ready lcr=0x%02x mcr=0x%02x divisor=%d",
                  name ? name : "", lcr, mcr, divisor);
        }
        s->guest_uart_ready = false;
        return;
    }
    s->guest_uart_lcr = lcr;
    s->guest_uart_mcr = mcr;
    s->guest_uart_ier = ier;
    s->guest_uart_divisor = divisor;
    if (!s->guest_uart_ready) {
        bridge_trace(s, "guest UART %s configured 8N1 divisor=%d",
              name ? name : "", divisor);
    }
    s->guest_uart_ready = true;
    s->guest_uart_configured = true;
    bridge_release_rx_paced(s);
}

bool pcconnect_bridge_cable_connected(void)
{
    pcc_br_state_t *s = wince_bridge();
    return s && bridge_data_path_connected(s);
}

bool pcconnect_bridge_trace_enabled(void)
{
    pcc_br_state_t *s = wince_bridge();
    return s && s->trace;
}

/* ---------- multi-UART serial bridge API (any wince_mode) ---------- */

bool serial_bridge_ns16550_claims(const char *name)
{
    return bridge_by_name(name) != NULL;
}

size_t serial_bridge_uart_rx_count(const char *name)
{
    pcc_br_state_t *s = bridge_by_name(name);
    if (!s)
        return 0;
    bridge_release_rx_paced(s);
    if (!bridge_data_path_connected(s) || !s->guest_uart_ready)
        return 0;
    return pcc_ring_count(&s->rx_ring);
}

int serial_bridge_uart_rx_pop(const char *name)
{
    pcc_br_state_t *s = bridge_by_name(name);
    if (!s)
        return -1;
    bridge_release_rx_paced(s);
    if (!bridge_data_path_connected(s) || !s->guest_uart_ready)
        return -1;
    if (pcc_ring_is_empty(&s->rx_ring))
        return -1;
    return pcc_ring_pop(&s->rx_ring);
}

void serial_bridge_uart_rx_clear(const char *name)
{
    bridge_guest_rx_fifo_reset(bridge_by_name(name));
}

void serial_bridge_uart_tx_byte(const char *name, uint8_t byte)
{
    bridge_queue_guest_tx(bridge_by_name(name), byte);
}
