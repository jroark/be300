/*
 * pcconnect_bridge — transparent serial bridge between the emulated
 * VRC4173 SIU UART and a host chardev (TCP socket / Unix socket / PTY).
 * Pairs with --pcconnect-bridge in src/main.c. Mutually exclusive with
 * the time-sync synthesizer in src/pcconnect.c.
 *
 * On the BE-300, the dock cradle wraps a USB-to-UART bridge IC. WinCE
 * drives a plain 115200 8N1 UART, and Windows on the PC sees a USB→Serial
 * COM port. So this bridge is a dumb byte pipe: real PCConnect.exe on a
 * UTM Windows VM (configured with `usb-serial` chardev) drives the
 * full sync handshake at the other end.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include "be300.h"
#include "pcconnect_bridge.h"
#include "pcconnect_ring.h"

#define PCC_BR_DEFAULT_UART "vrc4173siu"

typedef struct {
    bool armed;
    bool trace;
    pcc_bridge_kind_t kind;
    char *target;
    const char *uart_name;

    /* Listener / connected fd. listen_fd is -1 unless in *_LISTEN mode
     * with no client yet. fd is -1 unless a peer is currently attached. */
    int listen_fd;
    int fd;

    /* Saved sockaddr for tcp-connect (re-dial on disconnect). */
    char *connect_host;
    int connect_port;

    /* Saved path for unix-connect / unix-listen. */
    char *unix_path;

    /* Cable / readiness mirrored from set_cable_connected. */
    bool cable_connected;
    bool guest_uart_ready;
    uint8_t guest_uart_lcr;
    uint8_t guest_uart_mcr;
    uint8_t guest_uart_ier;
    int guest_uart_divisor;
    uint32_t baud;

    pcc_ring_t rx_ring;     /* host -> guest */
    pcc_ring_t tx_ring;     /* guest -> host */

    /* Reconnect throttle for TCP/Unix connect modes. */
    uint64_t next_reconnect_ns;

    /* G->H baud-rate throttle. 0 = unlimited. Otherwise wait at least
     * tx_ns_per_byte between successive bytes so PCConnect sees inter-byte
     * gaps similar to real serial. */
    uint64_t tx_ns_per_byte;
    uint64_t tx_next_due_ns;

    /* H->G inter-byte throttle. Same configured rate as TX. Drains TCP
     * bytes from rx_pre_ring to rx_ring at most one byte every
     * rx_ns_per_byte interval, so AtPcCnct's RAPI code-upload sees bytes
     * arrive at real-serial cadence (one IRQ per byte) instead of all
     * at once in a single TCP batch. */
    uint64_t rx_ns_per_byte;
    uint64_t rx_next_due_ns;
    pcc_ring_t rx_pre_ring;     /* TCP-arrived bytes pending release to rx_ring */

    void (*rx_ready_cb)(void *);
    void *rx_ready_opaque;

    /* tee log */
    FILE *tee;
    char *tee_path;
} pcc_br_state_t;

static pcc_br_state_t g;

/* ---------- helpers ---------- */

static uint64_t mono_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int fdf = fcntl(fd, F_GETFD, 0);
    if (fdf >= 0)
        fcntl(fd, F_SETFD, fdf | FD_CLOEXEC);
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void tee_write_dir(const char *tag, const uint8_t *buf, size_t len)
{
    if (!g.tee || len == 0)
        return;
    fprintf(g.tee, "%llu %s %zu:", (unsigned long long)mono_ns(), tag, len);
    for (size_t i = 0; i < len; i++)
        fprintf(g.tee, " %02x", buf[i]);
    fputc('\n', g.tee);
    fflush(g.tee);
}

static void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void trace(const char *fmt, ...)
{
    if (!g.trace)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[PC_CONNECT_BRIDGE] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void queue_host_rx_pre(uint8_t byte)
{
    pcc_ring_push(&g.rx_pre_ring, byte);
}

/* ---------- config / spec parsing ---------- */

bool pcconnect_bridge_parse_spec(const char *spec, pcc_bridge_config_t *out)
{
    if (!spec || !out)
        return false;
    memset(out, 0, sizeof(*out));

    if (strncmp(spec, "tcp:", 4) == 0) {
        out->kind = PCC_BR_TCP_CONNECT;
        out->target = spec + 4;
        if (!strchr(out->target, ':')) {
            fprintf(stderr,
                "pcconnect-bridge: tcp: spec needs host:port (got '%s')\n",
                out->target);
            return false;
        }
    } else if (strncmp(spec, "tcp-listen:", 11) == 0) {
        out->kind = PCC_BR_TCP_LISTEN;
        out->target = spec + 11;
    } else if (strncmp(spec, "unix:", 5) == 0) {
        out->kind = PCC_BR_UNIX_CONNECT;
        out->target = spec + 5;
    } else if (strncmp(spec, "unix-listen:", 12) == 0) {
        out->kind = PCC_BR_UNIX_LISTEN;
        out->target = spec + 12;
    } else if (strcmp(spec, "pty:auto") == 0) {
        out->kind = PCC_BR_PTY;
        out->target = "auto";
    } else {
        fprintf(stderr,
            "pcconnect-bridge: unknown spec '%s' (expected tcp:HOST:PORT, "
            "tcp-listen:PORT[@ADDR], unix:/PATH, unix-listen:/PATH, pty:auto)\n",
            spec);
        return false;
    }

    return true;
}

/* ---------- backend openers ---------- */

/* Parses "host:port" (or ":port" for any-host listen). On success returns
 * true and fills *port_out, *host_out (host_out borrows from spec). */
static bool split_host_port(const char *spec, const char **host_out,
    int *port_out)
{
    if (!spec)
        return false;
    /* Find rightmost ':' so IPv6 brackets aren't required for plain v4. */
    const char *colon = strrchr(spec, ':');
    if (!colon)
        return false;
    const char *host = spec;
    if (colon == spec)
        host = NULL;
    /* If only ":port" after stripping a leading ":" (already handled), or
     * no host given, host=NULL means listen on any interface. */
    if (colon - spec == 0)
        host = NULL;
    char *end = NULL;
    long port = strtol(colon + 1, &end, 10);
    if (!end || *end != '\0' || port <= 0 || port > 65535)
        return false;
    if (host_out)
        *host_out = host;
    if (port_out)
        *port_out = (int)port;
    return true;
}

static bool open_tcp_connect(const char *spec)
{
    /* spec = "HOST:PORT" or "[v6]:PORT" — strrchr handles plain v4 cleanly. */
    const char *host_in = NULL;
    int port = 0;
    if (!split_host_port(spec, &host_in, &port) || !host_in) {
        fprintf(stderr,
            "pcconnect-bridge: tcp: spec needs HOST:PORT (got '%s')\n", spec);
        return false;
    }
    /* copy host into stable buffer; "host_in" is a slice into spec. */
    size_t host_len = (size_t)(strrchr(spec, ':') - host_in);
    char *host = malloc(host_len + 1);
    if (!host) {
        fprintf(stderr, "pcconnect-bridge: oom\n");
        return false;
    }
    memcpy(host, host_in, host_len);
    host[host_len] = '\0';

    g.connect_host = host;
    g.connect_port = port;

    /* Try once now; tick() will retry if this fails. */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        fprintf(stderr, "pcconnect-bridge: socket: %s\n", strerror(errno));
        return false;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    set_nonblocking(s);

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        fprintf(stderr,
            "pcconnect-bridge: inet_pton('%s') failed; only IPv4 dotted-quad "
            "supported (use 127.0.0.1 etc.)\n", host);
        close(s);
        return false;
    }
    int rc = connect(s, (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0 && errno != EINPROGRESS && errno != EALREADY) {
        fprintf(stderr,
            "pcconnect-bridge: tcp connect %s:%d failed (%s); will retry\n",
            host, port, strerror(errno));
        close(s);
        g.next_reconnect_ns = mono_ns() + 500000000ull;
        return true;  /* arm; tick() will retry */
    }
    g.fd = s;
    fprintf(stderr, "[PC_CONNECT_BRIDGE] tcp connected to %s:%d\n", host, port);
    return true;
}

static bool open_tcp_listen(const char *spec)
{
    /* spec = "PORT" or "PORT@ADDR" */
    char *target = strdup(spec);
    if (!target) return false;
    char *at = strchr(target, '@');
    const char *bind_addr = "127.0.0.1";
    if (at) {
        *at = '\0';
        bind_addr = at + 1;
    }
    char *end = NULL;
    long port = strtol(target, &end, 10);
    if (!end || *end != '\0' || port <= 0 || port > 65535) {
        fprintf(stderr,
            "pcconnect-bridge: tcp-listen: spec needs PORT[@ADDR] (got '%s')\n",
            spec);
        free(target);
        return false;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        fprintf(stderr, "pcconnect-bridge: socket: %s\n", strerror(errno));
        free(target);
        return false;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    set_nonblocking(s);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_addr, &sa.sin_addr) != 1) {
        fprintf(stderr,
            "pcconnect-bridge: tcp-listen bind addr '%s' must be IPv4\n",
            bind_addr);
        close(s);
        free(target);
        return false;
    }
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "pcconnect-bridge: bind %s:%ld: %s\n",
                bind_addr, port, strerror(errno));
        close(s);
        free(target);
        return false;
    }
    if (listen(s, 1) != 0) {
        fprintf(stderr, "pcconnect-bridge: listen: %s\n", strerror(errno));
        close(s);
        free(target);
        return false;
    }
    g.listen_fd = s;
    fprintf(stderr,
        "[PC_CONNECT_BRIDGE] tcp listening on %s:%ld; awaiting peer\n",
        bind_addr, port);
    free(target);
    return true;
}

static bool open_unix_connect(const char *path)
{
    g.unix_path = strdup(path);
    if (!g.unix_path) return false;
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
        fprintf(stderr, "pcconnect-bridge: socket(AF_UNIX): %s\n",
                strerror(errno));
        return false;
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    set_nonblocking(s);
    struct sockaddr_un sa = {0};
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path)) {
        fprintf(stderr,
            "pcconnect-bridge: unix path too long (max %zu): '%s'\n",
            sizeof(sa.sun_path) - 1, path);
        close(s);
        return false;
    }
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    int rc = connect(s, (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0 && errno != EINPROGRESS) {
        fprintf(stderr,
            "pcconnect-bridge: unix connect '%s' failed (%s); will retry\n",
            path, strerror(errno));
        close(s);
        g.next_reconnect_ns = mono_ns() + 500000000ull;
        return true;
    }
    g.fd = s;
    fprintf(stderr, "[PC_CONNECT_BRIDGE] unix connected to %s\n", path);
    return true;
}

static bool open_unix_listen(const char *path)
{
    g.unix_path = strdup(path);
    if (!g.unix_path) return false;
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
        fprintf(stderr, "pcconnect-bridge: socket(AF_UNIX): %s\n",
                strerror(errno));
        return false;
    }
    set_nonblocking(s);
    struct sockaddr_un sa = {0};
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path)) {
        fprintf(stderr,
            "pcconnect-bridge: unix path too long (max %zu): '%s'\n",
            sizeof(sa.sun_path) - 1, path);
        close(s);
        return false;
    }
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    /* Best-effort unlink stale socket file. */
    unlink(path);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "pcconnect-bridge: bind unix '%s': %s\n",
                path, strerror(errno));
        close(s);
        return false;
    }
    if (listen(s, 1) != 0) {
        fprintf(stderr, "pcconnect-bridge: listen: %s\n", strerror(errno));
        close(s);
        return false;
    }
    g.listen_fd = s;
    fprintf(stderr,
        "[PC_CONNECT_BRIDGE] unix listening on %s; awaiting peer\n", path);
    return true;
}

static void try_accept(void)
{
    if (g.listen_fd < 0 || g.fd >= 0)
        return;
    int s = accept(g.listen_fd, NULL, NULL);
    if (s < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;
        fprintf(stderr, "pcconnect-bridge: accept: %s\n", strerror(errno));
        return;
    }
    set_nonblocking(s);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    g.fd = s;
    trace("peer accepted on fd=%d", s);
}

static void try_reconnect(void)
{
    if (g.fd >= 0)
        return;
    uint64_t now = mono_ns();
    if (now < g.next_reconnect_ns)
        return;
    g.next_reconnect_ns = now + 500000000ull;  /* 500 ms throttle */
    if (g.kind == PCC_BR_TCP_CONNECT) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return;
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        set_nonblocking(s);
        struct sockaddr_in sa = {0};
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)g.connect_port);
        inet_pton(AF_INET, g.connect_host, &sa.sin_addr);
        int rc = connect(s, (struct sockaddr *)&sa, sizeof(sa));
        if (rc == 0 || (rc < 0 && errno == EINPROGRESS)) {
            g.fd = s;
            trace("tcp reconnect to %s:%d in progress", g.connect_host,
                  g.connect_port);
        } else {
            close(s);
        }
    } else if (g.kind == PCC_BR_UNIX_CONNECT) {
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s < 0) return;
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        set_nonblocking(s);
        struct sockaddr_un sa = {0};
        sa.sun_family = AF_UNIX;
        strncpy(sa.sun_path, g.unix_path, sizeof(sa.sun_path) - 1);
        int rc = connect(s, (struct sockaddr *)&sa, sizeof(sa));
        if (rc == 0 || (rc < 0 && errno == EINPROGRESS)) {
            g.fd = s;
            trace("unix reconnect to %s in progress", g.unix_path);
        } else {
            close(s);
        }
    }
}

static bool open_pty(void)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        fprintf(stderr, "pcconnect-bridge: posix_openpt: %s\n",
                strerror(errno));
        return false;
    }
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        fprintf(stderr, "pcconnect-bridge: grantpt/unlockpt: %s\n",
                strerror(errno));
        close(master);
        return false;
    }
    const char *slave = ptsname(master);
    if (!slave) {
        fprintf(stderr, "pcconnect-bridge: ptsname returned NULL\n");
        close(master);
        return false;
    }
    set_nonblocking(master);
    g.fd = master;
    fprintf(stderr, "[PC_CONNECT_BRIDGE] pty slave=%s\n", slave);
    fprintf(stderr, "[PC_CONNECT_BRIDGE]   attach with: screen %s 115200,cs8\n",
            slave);
    return true;
}

/* ---------- I/O helpers ---------- */

static void on_peer_gone(const char *why)
{
    if (g.fd < 0)
        return;
    trace("peer gone (%s); closing fd %d", why, g.fd);
    close_fd(&g.fd);
    /* Cable stays connected; PTY-style backends can re-open transparently
     * when a new client attaches. Listen-mode backends will re-accept in tick. */
}

static void bridge_note_rx_released(bool was_empty, bool released)
{
    if (released && was_empty && g.rx_ready_cb)
        g.rx_ready_cb(g.rx_ready_opaque);
}

static void bridge_guest_rx_push(uint8_t byte)
{
    pcc_ring_push(&g.rx_ring, byte);
}

/* Move bytes from rx_pre_ring (TCP arrival queue) into rx_ring
 * (guest-visible queue), pacing at rx_ns_per_byte when requested. The
 * companion wake is an empty->non-empty edge; once RX is already pending,
 * additional host poll bytes should not generate a CommMode IRQ storm. */
static void bridge_release_rx_paced(void)
{
    bool was_empty;
    bool released = false;

    if (!g.cable_connected || !g.guest_uart_ready)
        return;

    if (g.rx_ns_per_byte == 0) {
        /* Unthrottled: flush pre_ring straight to rx_ring in one shot. */
        if (pcc_ring_is_empty(&g.rx_pre_ring))
            return;
        was_empty = pcc_ring_is_empty(&g.rx_ring);
        while (!pcc_ring_is_empty(&g.rx_pre_ring) &&
               pcc_ring_count(&g.rx_ring) < PCC_RING_CAP) {
            int b = pcc_ring_pop(&g.rx_pre_ring);
            if (b < 0) break;
            bridge_guest_rx_push((uint8_t)b);
            released = true;
        }
        bridge_note_rx_released(was_empty, released);
        return;
    }

    if (pcc_ring_is_empty(&g.rx_pre_ring))
        return;
    was_empty = pcc_ring_is_empty(&g.rx_ring);
    uint64_t now = mono_ns();
    if (g.rx_next_due_ns == 0 || g.rx_next_due_ns + 100000000ull < now)
        g.rx_next_due_ns = now;
    while (!pcc_ring_is_empty(&g.rx_pre_ring) && now >= g.rx_next_due_ns &&
           pcc_ring_count(&g.rx_ring) < PCC_RING_CAP) {
        int b = pcc_ring_pop(&g.rx_pre_ring);
        if (b < 0) break;
        bridge_guest_rx_push((uint8_t)b);
        released = true;
        g.rx_next_due_ns += g.rx_ns_per_byte;
        now = mono_ns();
    }
    bridge_note_rx_released(was_empty, released);
}

static void bridge_drain_rx(void)
{
    if (g.fd < 0) {
        /* Even with no fd, keep paced release flowing for any pending bytes. */
        bridge_release_rx_paced();
        return;
    }

    uint8_t scratch[1024];
    for (;;) {
        size_t cap = PCC_RING_CAP - pcc_ring_count(&g.rx_pre_ring);
        if (cap == 0)
            break;
        if (cap > sizeof(scratch))
            cap = sizeof(scratch);
        ssize_t n = read(g.fd, scratch, cap);
        if (n > 0) {
            if (!g.cable_connected) {
                /*
                 * PCConnect may poll the VM's serial port before the BE-300
                 * is docked. Real disconnected serial pins do not buffer the
                 * PC's polling stream for later replay, so consume those
                 * bytes from the host fd until the cable edge is presented.
                 */
                tee_write_dir("H>G:drop", scratch, (size_t)n);
                continue;
            }
            for (ssize_t i = 0; i < n; i++)
                queue_host_rx_pre(scratch[i]);
            /*
             * Once the cable is inserted, the PC's serial stream is present
             * on the dock RX pin. Preserve the short post-dock polling prefix
             * until serial.dll finishes programming 8N1 and drains the UART.
             */
            tee_write_dir(g.guest_uart_ready ? "H>G" : "H>G:queued",
                scratch, (size_t)n);
            continue;
        }
        if (n == 0) {
            /* EOF on socket = peer hangup. PTYs return EAGAIN with no slave,
             * so a 0 return from a PTY master here is unusual but treat it
             * the same way: drop the fd; let the kind-specific reopen path
             * decide. For PTY we re-open the master because the slave path
             * can't be reused. */
            if (g.kind == PCC_BR_PTY) {
                on_peer_gone("PTY EOF");
                /* re-open master so a new screen session can attach */
                open_pty();
            } else {
                on_peer_gone("EOF");
            }
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        if (errno == EINTR)
            continue;
        on_peer_gone(strerror(errno));
        break;
    }
    /* After draining TCP, release queued bytes to the guest at the
     * configured baud rate. */
    bridge_release_rx_paced();
}

static void bridge_drain_tx(void)
{
    if (g.fd < 0)
        return;

    /* Baud-throttled path: emit one byte per tx_ns_per_byte interval so
     * PCConnect sees inter-byte gaps comparable to real 115200 8N1.
     * Without throttling, PCConnect's frame parser receives the full
     * device response in a single TCP segment, which is consistent with
     * stuck retries in the opcode-0x46 phase. */
    if (g.tx_ns_per_byte > 0) {
        uint64_t now = mono_ns();
        if (g.tx_next_due_ns == 0)
            g.tx_next_due_ns = now;
        while (!pcc_ring_is_empty(&g.tx_ring) && now >= g.tx_next_due_ns) {
            uint8_t byte = (uint8_t)pcc_ring_pop(&g.tx_ring);
            ssize_t n = write(g.fd, &byte, 1);
            if (n == 1) {
                g.tx_next_due_ns += g.tx_ns_per_byte;
                if (g.tx_next_due_ns + 10ull * g.tx_ns_per_byte < now)
                    g.tx_next_due_ns = now;     /* re-anchor after long idle */
                now = mono_ns();
                continue;
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* Push the byte back; try again next tick. */
                    g.tx_ring.tail = (g.tx_ring.tail + PCC_RING_CAP - 1u)
                        % PCC_RING_CAP;
                    g.tx_ring.count++;
                    return;
                }
                if (errno == EINTR)
                    continue;
                on_peer_gone(strerror(errno));
                pcc_ring_clear(&g.tx_ring);
                return;
            }
            return;
        }
        return;
    }

    /* Unthrottled bulk path. */
    while (!pcc_ring_is_empty(&g.tx_ring)) {
        size_t count = pcc_ring_count(&g.tx_ring);
        size_t contig = PCC_RING_CAP - g.tx_ring.tail;
        if (contig > count)
            contig = count;
        ssize_t n = write(g.fd, &g.tx_ring.buf[g.tx_ring.tail], contig);
        if (n > 0) {
            g.tx_ring.tail = (g.tx_ring.tail + (size_t)n) % PCC_RING_CAP;
            g.tx_ring.count -= (size_t)n;
            continue;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR)
                continue;
            on_peer_gone(strerror(errno));
            pcc_ring_clear(&g.tx_ring);
            return;
        }
        /* n == 0: defensive — try again next tick */
        return;
    }
}

/* ---------- public API ---------- */

bool pcconnect_bridge_configure(const pcc_bridge_config_t *cfg)
{
    if (!cfg)
        return false;
    if (g.armed) {
        fprintf(stderr, "pcconnect-bridge: already configured\n");
        return false;
    }

    /* Clean slate. */
    memset(&g, 0, sizeof(g));
    g.fd = -1;
    g.listen_fd = -1;

    /* SIGPIPE -> SIG_IGN so dropped peers don't terminate the emulator. */
    signal(SIGPIPE, SIG_IGN);

    g.kind = cfg->kind;
    g.target = cfg->target ? strdup(cfg->target) : NULL;
    g.uart_name = cfg->uart_name ? cfg->uart_name : PCC_BR_DEFAULT_UART;
    g.trace = cfg->trace;
    g.baud = cfg->baud;
    if (cfg->baud > 0) {
        /* 8N1 framing = 10 bit-times per byte. */
        g.tx_ns_per_byte = 10000000000ull / (uint64_t)cfg->baud;
    } else {
        g.tx_ns_per_byte = 0;
    }
    /* H->G stays unthrottled: empirically AtPcCnct.exe handles a single
     * IRQ-with-N-bytes-in-ring better than N per-byte IRQs at 87us
     * spacing. Protocol exchange (28+ frames including RAPI code-upload)
     * worked with burst delivery; per-byte pacing reduced it to 24
     * sync echoes only. The G->H direction still needs the baud throttle
     * because PCConnect's parser is sensitive to inter-byte gaps. */
    g.rx_ns_per_byte = 0;
    g.tx_next_due_ns = 0;
    g.rx_next_due_ns = 0;

    if (cfg->tee_path) {
        g.tee = fopen(cfg->tee_path, "w");
        if (!g.tee) {
            fprintf(stderr, "pcconnect-bridge: open tee '%s': %s\n",
                    cfg->tee_path, strerror(errno));
            free(g.target);
            g.target = NULL;
            return false;
        }
        g.tee_path = strdup(cfg->tee_path);
        setvbuf(g.tee, NULL, _IOLBF, 0);
        fprintf(g.tee,
            "# pcconnect-bridge tee log; mono_ns dir count: hex bytes\n");
        fflush(g.tee);
    }

    bool ok = false;
    switch (g.kind) {
    case PCC_BR_PTY:           ok = open_pty();                  break;
    case PCC_BR_TCP_CONNECT:   ok = open_tcp_connect(g.target);  break;
    case PCC_BR_TCP_LISTEN:    ok = open_tcp_listen(g.target);   break;
    case PCC_BR_UNIX_CONNECT:  ok = open_unix_connect(g.target); break;
    case PCC_BR_UNIX_LISTEN:   ok = open_unix_listen(g.target);  break;
    case PCC_BR_NONE:
    default:
        fprintf(stderr, "pcconnect-bridge: empty kind\n");
        break;
    }
    if (!ok) {
        if (g.tee) { fclose(g.tee); g.tee = NULL; }
        free(g.target); g.target = NULL;
        free(g.tee_path); g.tee_path = NULL;
        return false;
    }

    g.armed = true;
    trace("armed kind=%d target=%s uart=%s tee=%s",
          (int)g.kind, g.target ? g.target : "(null)",
          g.uart_name ? g.uart_name : PCC_BR_DEFAULT_UART,
          g.tee_path ? g.tee_path : "(none)");
    return true;
}

bool pcconnect_bridge_enabled(void)
{
    return g.armed;
}

void pcconnect_bridge_tick(void)
{
    if (!g.armed)
        return;
    /* Listen modes: accept a peer if none yet. Connect modes: re-dial
     * after a disconnect. PTY mode: nothing to accept. */
    if (g.listen_fd >= 0)
        try_accept();
    else if (g.kind == PCC_BR_TCP_CONNECT || g.kind == PCC_BR_UNIX_CONNECT)
        try_reconnect();
    bridge_drain_tx();
    bridge_drain_rx();
}

void pcconnect_bridge_shutdown(void)
{
    if (g.tee) {
        fflush(g.tee);
        fclose(g.tee);
        g.tee = NULL;
    }
    close_fd(&g.fd);
    close_fd(&g.listen_fd);
    free(g.target);
    g.target = NULL;
    free(g.connect_host);
    g.connect_host = NULL;
    free(g.unix_path);
    g.unix_path = NULL;
    free(g.tee_path);
    g.tee_path = NULL;
    g.armed = false;
}

/* ---------- backend entry points ---------- */

bool pcconnect_bridge_ns16550_claims(const char *name)
{
    if (!g.armed || !name)
        return false;

    return strcmp(name, g.uart_name ? g.uart_name : PCC_BR_DEFAULT_UART) == 0;
}

bool pcconnect_bridge_uart_rx_available(void)
{
    if (!g.armed)
        return false;
    /* Drain the host fd into the rx ring on every poll; tick() also drains
     * it so PCConnect polling is retained even before the guest opens COM1. */
    bridge_drain_rx();
    if (!g.cable_connected || !g.guest_uart_ready)
        return false;
    return !pcc_ring_is_empty(&g.rx_ring);
}

size_t pcconnect_bridge_uart_rx_count(void)
{
    if (!g.armed)
        return 0;
    bridge_drain_rx();
    if (!g.cable_connected || !g.guest_uart_ready)
        return 0;
    return pcc_ring_count(&g.rx_ring);
}

int pcconnect_bridge_uart_rx_pop(void)
{
    int byte;
    if (!pcconnect_bridge_uart_rx_available())
        return -1;
    byte = pcc_ring_pop(&g.rx_ring);
    return byte;
}

void pcconnect_bridge_uart_rx_clear(void)
{
    if (!g.armed)
        return;

    pcc_ring_clear(&g.rx_ring);
    pcc_ring_clear(&g.rx_pre_ring);
    g.rx_next_due_ns = 0;
}

void pcconnect_bridge_uart_tx_byte(uint8_t byte)
{
    if (!g.armed || !g.cable_connected) {
        if (g.tee && g.armed)
            tee_write_dir("G>H:drop", &byte, 1);
        return;
    }
    pcc_ring_push(&g.tx_ring, byte);
    tee_write_dir("G>H", &byte, 1);
    /* drain attempted in tick() */
}

uint32_t pcconnect_bridge_uart_baud(void)
{
    if (!g.armed)
        return 0;

    return g.baud != 0 ? g.baud : 115200u;
}

void pcconnect_bridge_set_cable_connected(bool connected)
{
    if (!g.armed)
        return;

    if (g.cable_connected == connected)
        return;

    g.cable_connected = connected;
    if (!connected) {
        g.guest_uart_ready = false;
        pcc_ring_clear(&g.rx_ring);
        pcc_ring_clear(&g.rx_pre_ring);
        pcc_ring_clear(&g.tx_ring);
    } else {
        bridge_release_rx_paced();
    }
    trace("cable %s", connected ? "connected" : "disconnected");
}

void pcconnect_bridge_set_rx_ready_callback(void (*cb)(void *opaque),
    void *opaque)
{
    g.rx_ready_cb = cb;
    g.rx_ready_opaque = opaque;
}

void pcconnect_bridge_note_uart_config(const char *name, uint8_t lcr,
    uint8_t mcr, uint8_t ier, int divisor, int dlab)
{
    bool ready;

    if (!g.armed)
        return;
    if (!pcconnect_bridge_ns16550_claims(name))
        return;
    /* The receiver is usable once the guest has selected 8N1 and left the
     * divisor latch; MCR DTR/RTS are modem outputs, not an RX enable. */
    ready = !dlab && (lcr & 0x7f) == 0x03;
    if (!ready) {
        if (g.guest_uart_ready) {
            trace("guest UART %s receiver not ready lcr=0x%02x mcr=0x%02x divisor=%d",
                  name ? name : "", lcr, mcr, divisor);
        }
        g.guest_uart_ready = false;
        return;
    }
    g.guest_uart_lcr = lcr;
    g.guest_uart_mcr = mcr;
    g.guest_uart_ier = ier;
    g.guest_uart_divisor = divisor;
    if (!g.guest_uart_ready) {
        trace("guest UART %s configured 8N1 divisor=%d",
              name ? name : "", divisor);
    }
    g.guest_uart_ready = true;
    bridge_release_rx_paced();
}

bool pcconnect_bridge_cable_connected(void)
{
    return g.armed && g.cable_connected;
}

bool pcconnect_bridge_guest_uart_ready(void)
{
    return g.armed && g.guest_uart_ready;
}

bool pcconnect_bridge_trace_enabled(void)
{
    return g.armed && g.trace;
}
