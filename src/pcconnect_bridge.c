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
 * On the BE-300, the dock cradle wraps a USB-to-UART bridge IC. WinCE
 * drives a plain 115200 8N1 UART, and Windows on the PC sees a USB→Serial
 * COM port. So this bridge is a dumb byte pipe: real PCConnect.exe on a
 * UTM Windows VM (configured with `usb-serial` chardev) drives the
 * full sync handshake at the other end.
 */

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include "win32_compat.h"
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#endif

#include "be300.h"
#include "pcconnect_bridge.h"
#include "pcconnect_ring.h"

#define PCC_PREOPEN_FIFO_CAP 64u
#define PCC_GUEST_RX_FIFO_CAP 16u
#define PCC_RX_PACE_BACKLOG_THRESHOLD 512u

#ifdef _WIN32
typedef intptr_t pcc_handle_t;
#define PCC_INVALID_HANDLE ((pcc_handle_t)-1)
#define PCC_SOCKOPT_PTR(p) ((const char *)(p))
#else
typedef int pcc_handle_t;
#define PCC_INVALID_HANDLE (-1)
#define PCC_SOCKOPT_PTR(p) (p)
#endif

typedef struct pcc_br_state {
    bool armed;
    bool trace;
    bool wince_mode;
    pcc_bridge_uart_id_t uart_id;
    pcc_bridge_kind_t kind;
    char *target;
    const char *uart_name;          /* points into name_storage[] */
    char name_storage[32];

    /* Listener / connected fd. listen_fd is -1 unless in *_LISTEN mode
     * with no client yet. fd is -1 unless a peer is currently attached. */
    pcc_handle_t listen_fd;
    pcc_handle_t fd;

    /* PTY mode only: an internal slave fd the bridge holds open for the
     * pty's lifetime. It is never read/written — its sole jobs are to
     * (1) pin the pts line discipline in raw mode (no ECHO/ICANON, so the
     * guest UART's own TX is not echoed back as phantom RX) and (2) keep
     * the master from seeing EOF/HUP when the user's screen detaches,
     * which otherwise caused a reopen-churn loop. -1 when not a PTY. */
    pcc_handle_t pty_slave_fd;

    /* Saved sockaddr for tcp-connect (re-dial on disconnect). */
    char *connect_host;
    int connect_port;

    /* Saved path for unix-connect / unix-listen. */
    char *unix_path;

    /* Physical cable plus guest-visible serial data path readiness. */
    bool cable_connected;
    bool guest_link_connected;
    bool guest_uart_ready;
    bool drop_initial_rx_idle;
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

static uint64_t mono_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void set_nonblocking(pcc_handle_t fd)
{
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket((SOCKET)fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int fdf = fcntl(fd, F_GETFD, 0);
    if (fdf >= 0)
        fcntl(fd, F_SETFD, fdf | FD_CLOEXEC);
#endif
}

static void close_fd(pcc_handle_t *fd)
{
    if (*fd >= 0) {
#ifdef _WIN32
        closesocket((SOCKET)*fd);
#else
        close(*fd);
#endif
        *fd = PCC_INVALID_HANDLE;
    }
}

static ssize_t bridge_read(pcc_handle_t fd, void *buf, size_t len)
{
#ifdef _WIN32
    int n = recv((SOCKET)fd, (char *)buf, (int)len, 0);
    if (n < 0)
        errno = WSAGetLastError();
    return n;
#else
    return read(fd, buf, len);
#endif
}

static ssize_t bridge_write(pcc_handle_t fd, const void *buf, size_t len)
{
#ifdef _WIN32
    int n = send((SOCKET)fd, (const char *)buf, (int)len, 0);
    if (n < 0)
        errno = WSAGetLastError();
    return n;
#else
    return write(fd, buf, len);
#endif
}

static bool bridge_errno_would_block(void)
{
#ifdef _WIN32
    return errno == WSAEWOULDBLOCK || errno == WSAEINPROGRESS ||
        errno == WSAEALREADY;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

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
    fprintf(s->tee, "%llu %s %zu:", (unsigned long long)mono_ns(), tag, len);
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
    if (!s->guest_uart_ready &&
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

static bool open_tcp_connect(pcc_br_state_t *s, const char *spec)
{
    const char *host_in = NULL;
    int port = 0;
    if (!split_host_port(spec, &host_in, &port) || !host_in) {
        fprintf(stderr,
            "pcconnect-bridge: tcp: spec needs HOST:PORT (got '%s')\n", spec);
        return false;
    }
    size_t host_len = (size_t)(strrchr(spec, ':') - host_in);
    char *host = malloc(host_len + 1);
    if (!host) {
        fprintf(stderr, "pcconnect-bridge: oom\n");
        return false;
    }
    memcpy(host, host_in, host_len);
    host[host_len] = '\0';

    s->connect_host = host;
    s->connect_port = port;

    pcc_handle_t sk = (pcc_handle_t)socket(AF_INET, SOCK_STREAM, 0);
    if (sk < 0) {
        fprintf(stderr, "pcconnect-bridge: socket: %s\n", strerror(errno));
        return false;
    }
    int one = 1;
    setsockopt(sk, IPPROTO_TCP, TCP_NODELAY, PCC_SOCKOPT_PTR(&one),
        sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(sk, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    set_nonblocking(sk);

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        fprintf(stderr,
            "pcconnect-bridge: inet_pton('%s') failed; only IPv4 dotted-quad "
            "supported (use 127.0.0.1 etc.)\n", host);
        close_fd(&sk);
        return false;
    }
    int rc = connect(sk, (struct sockaddr *)&sa, sizeof(sa));
#ifdef _WIN32
    if (rc < 0)
        errno = WSAGetLastError();
#endif
    if (rc < 0 && !bridge_errno_would_block()) {
        fprintf(stderr,
            "pcconnect-bridge: tcp connect %s:%d failed (%s); will retry\n",
            host, port, strerror(errno));
        close_fd(&sk);
        s->next_reconnect_ns = mono_ns() + 500000000ull;
        return true;
    }
    s->fd = sk;
    fprintf(stderr, "[%s] tcp connected to %s:%d\n", bridge_tag(s),
        host, port);
    return true;
}

static bool open_tcp_listen(pcc_br_state_t *s, const char *spec)
{
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
    pcc_handle_t sk = (pcc_handle_t)socket(AF_INET, SOCK_STREAM, 0);
    if (sk < 0) {
        fprintf(stderr, "pcconnect-bridge: socket: %s\n", strerror(errno));
        free(target);
        return false;
    }
    int one = 1;
    setsockopt(sk, SOL_SOCKET, SO_REUSEADDR, PCC_SOCKOPT_PTR(&one),
        sizeof(one));
    set_nonblocking(sk);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_addr, &sa.sin_addr) != 1) {
        fprintf(stderr,
            "pcconnect-bridge: tcp-listen bind addr '%s' must be IPv4\n",
            bind_addr);
        close_fd(&sk);
        free(target);
        return false;
    }
    if (bind(sk, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "pcconnect-bridge: bind %s:%ld: %s\n",
                bind_addr, port, strerror(errno));
        close_fd(&sk);
        free(target);
        return false;
    }
    if (listen(sk, 1) != 0) {
        fprintf(stderr, "pcconnect-bridge: listen: %s\n", strerror(errno));
        close_fd(&sk);
        free(target);
        return false;
    }
    s->listen_fd = sk;
    fprintf(stderr,
        "[%s] tcp listening on %s:%ld; awaiting peer\n",
        bridge_tag(s), bind_addr, port);
    free(target);
    return true;
}

static bool open_unix_connect(pcc_br_state_t *s, const char *path)
{
#ifdef _WIN32
    (void)s;
    fprintf(stderr, "pcconnect-bridge: unix sockets are unsupported on Windows: %s\n",
        path ? path : "(null)");
    return false;
#else
    s->unix_path = strdup(path);
    if (!s->unix_path) return false;
    int sk = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sk < 0) {
        fprintf(stderr, "pcconnect-bridge: socket(AF_UNIX): %s\n",
                strerror(errno));
        return false;
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(sk, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    set_nonblocking(sk);
    struct sockaddr_un sa = {0};
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path)) {
        fprintf(stderr,
            "pcconnect-bridge: unix path too long (max %zu): '%s'\n",
            sizeof(sa.sun_path) - 1, path);
        close(sk);
        return false;
    }
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    int rc = connect(sk, (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0 && errno != EINPROGRESS) {
        fprintf(stderr,
            "pcconnect-bridge: unix connect '%s' failed (%s); will retry\n",
            path, strerror(errno));
        close(sk);
        s->next_reconnect_ns = mono_ns() + 500000000ull;
        return true;
    }
    s->fd = sk;
    fprintf(stderr, "[%s] unix connected to %s\n", bridge_tag(s), path);
    return true;
#endif
}

static bool open_unix_listen(pcc_br_state_t *s, const char *path)
{
#ifdef _WIN32
    (void)s;
    fprintf(stderr, "pcconnect-bridge: unix sockets are unsupported on Windows: %s\n",
        path ? path : "(null)");
    return false;
#else
    s->unix_path = strdup(path);
    if (!s->unix_path) return false;
    int sk = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sk < 0) {
        fprintf(stderr, "pcconnect-bridge: socket(AF_UNIX): %s\n",
                strerror(errno));
        return false;
    }
    set_nonblocking(sk);
    struct sockaddr_un sa = {0};
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path)) {
        fprintf(stderr,
            "pcconnect-bridge: unix path too long (max %zu): '%s'\n",
            sizeof(sa.sun_path) - 1, path);
        close(sk);
        return false;
    }
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    unlink(path);
    if (bind(sk, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "pcconnect-bridge: bind unix '%s': %s\n",
                path, strerror(errno));
        close(sk);
        return false;
    }
    if (listen(sk, 1) != 0) {
        fprintf(stderr, "pcconnect-bridge: listen: %s\n", strerror(errno));
        close(sk);
        return false;
    }
    s->listen_fd = sk;
    fprintf(stderr,
        "[%s] unix listening on %s; awaiting peer\n", bridge_tag(s), path);
    return true;
#endif
}

static void try_accept(pcc_br_state_t *s)
{
    if (s->listen_fd < 0 || s->fd >= 0)
        return;
#ifdef _WIN32
    pcc_handle_t sk = (pcc_handle_t)accept((SOCKET)s->listen_fd, NULL, NULL);
#else
    pcc_handle_t sk = accept(s->listen_fd, NULL, NULL);
#endif
    if (sk < 0) {
#ifdef _WIN32
        errno = WSAGetLastError();
#endif
        if (bridge_errno_would_block() || errno == EINTR)
            return;
        fprintf(stderr, "pcconnect-bridge: accept: %s\n", strerror(errno));
        return;
    }
    set_nonblocking(sk);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(sk, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    clear_serial_queues(s);
    s->fd = sk;
    bridge_trace(s, "peer accepted on fd=%lld", (long long)sk);
}

static void try_reconnect(pcc_br_state_t *s)
{
    if (s->fd >= 0)
        return;
    uint64_t now = mono_ns();
    if (now < s->next_reconnect_ns)
        return;
    s->next_reconnect_ns = now + 500000000ull;
    if (s->kind == PCC_BR_TCP_CONNECT) {
        pcc_handle_t sk = (pcc_handle_t)socket(AF_INET, SOCK_STREAM, 0);
        if (sk < 0) return;
        int one = 1;
        setsockopt(sk, IPPROTO_TCP, TCP_NODELAY, PCC_SOCKOPT_PTR(&one),
            sizeof(one));
#ifdef SO_NOSIGPIPE
        setsockopt(sk, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        set_nonblocking(sk);
        struct sockaddr_in sa = {0};
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)s->connect_port);
        inet_pton(AF_INET, s->connect_host, &sa.sin_addr);
        int rc = connect(sk, (struct sockaddr *)&sa, sizeof(sa));
#ifdef _WIN32
        if (rc < 0)
            errno = WSAGetLastError();
#endif
        if (rc == 0 || (rc < 0 && bridge_errno_would_block())) {
            s->fd = sk;
            bridge_trace(s, "tcp reconnect to %s:%d in progress",
                s->connect_host, s->connect_port);
        } else {
            close_fd(&sk);
        }
#ifndef _WIN32
    } else if (s->kind == PCC_BR_UNIX_CONNECT) {
        int sk = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sk < 0) return;
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(sk, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        set_nonblocking(sk);
        struct sockaddr_un sa = {0};
        sa.sun_family = AF_UNIX;
        strncpy(sa.sun_path, s->unix_path, sizeof(sa.sun_path) - 1);
        int rc = connect(sk, (struct sockaddr *)&sa, sizeof(sa));
        if (rc == 0 || (rc < 0 && bridge_errno_would_block())) {
            s->fd = sk;
            bridge_trace(s, "unix reconnect to %s in progress", s->unix_path);
        } else {
            close(sk);
        }
#endif
    }
}

static bool open_pty(pcc_br_state_t *s)
{
#ifdef _WIN32
    (void)s;
    fprintf(stderr, "pcconnect-bridge: pty mode is unsupported on Windows\n");
    return false;
#else
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
    /*
     * Put the pts line discipline in RAW mode and hold an internal slave
     * fd open for the pty's lifetime. Two bugs this fixes:
     *
     *  1. Echo feedback storm. A pts slave defaults to cooked + ECHO. The
     *     bridge writes guest-UART TX to the master; that lands in the
     *     slave input queue; the slave line discipline ECHOes it back out,
     *     where the bridge reads it from the master and re-injects it as
     *     guest RX. A serial getty then respawns forever reading its own
     *     prompt. cfmakeraw on the *master* fd is a no-op on macOS (only
     *     the slave owns the line discipline), so the termios must be set
     *     on a slave fd.
     *  2. Master EOF/reopen churn. When the user's `screen` detaches the
     *     master sees EOF; the bridge reopened the pty in a tight loop.
     *     Holding our own slave fd open keeps the pts connected so the
     *     master only blocks (EAGAIN) instead of seeing EOF.
     *
     * The held slave fd is never read or written — extra slave opens
     * (the user's `screen`) share the same raw termios and receive all
     * guest TX. This is a transparent byte pipe: the guest UART model
     * owns framing/echo, not the host tty layer.
     */
    if (s->pty_slave_fd >= 0)
        close_fd(&s->pty_slave_fd);
    {
        int slave_fd = open(slave, O_RDWR | O_NOCTTY);
        if (slave_fd < 0) {
            fprintf(stderr,
                "[%s] warning: could not open pty slave %s for raw setup: %s\n",
                bridge_tag(s), slave, strerror(errno));
        } else {
            struct termios tio;
            if (tcgetattr(slave_fd, &tio) == 0) {
                cfmakeraw(&tio);
                tio.c_cc[VMIN] = 1;
                tio.c_cc[VTIME] = 0;
                if (tcsetattr(slave_fd, TCSANOW, &tio) != 0)
                    fprintf(stderr,
                        "[%s] warning: tcsetattr(slave) raw failed: %s\n",
                        bridge_tag(s), strerror(errno));
            }
            s->pty_slave_fd = slave_fd;
        }
    }
    /* Also raw the master where that is honored (Linux). Harmless on macOS. */
    {
        struct termios mt;
        if (tcgetattr(master, &mt) == 0) {
            cfmakeraw(&mt);
            mt.c_cc[VMIN] = 1;
            mt.c_cc[VTIME] = 0;
            (void)tcsetattr(master, TCSANOW, &mt);
        }
    }
    set_nonblocking(master);
    s->fd = master;
    fprintf(stderr, "[%s] pty slave=%s\n", bridge_tag(s), slave);
    fprintf(stderr, "[%s]   attach with: screen %s 115200,cs8\n",
            bridge_tag(s), slave);
    return true;
#endif
}

/* ---------- I/O helpers ---------- */

static void on_peer_gone(pcc_br_state_t *s, const char *why)
{
    if (s->fd < 0)
        return;
    bridge_trace(s, "peer gone (%s); closing fd %lld; cable remains %s",
        why, (long long)s->fd,
        s->cable_connected ? "connected" : "disconnected");
    close_fd(&s->fd);
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
    uint64_t now = mono_ns();
    if (s->rx_next_due_ns == 0 || s->rx_next_due_ns + 100000000ull < now)
        s->rx_next_due_ns = now;
    while (!pcc_ring_is_empty(&s->rx_pre_ring) && now >= s->rx_next_due_ns &&
           bridge_guest_rx_space(s) > 0) {
        int b = pcc_ring_pop(&s->rx_pre_ring);
        if (b < 0) break;
        released |= bridge_guest_rx_push(s, (uint8_t)b);
        s->rx_next_due_ns += s->rx_ns_per_byte;
        now = mono_ns();
    }
    bridge_note_rx_released(s, was_empty, released);
    if (pcc_ring_is_empty(&s->rx_pre_ring))
        s->rx_pace_active = false;
}

static void bridge_drain_rx(pcc_br_state_t *s)
{
    if (s->fd < 0) {
        bridge_release_rx_paced(s);
        return;
    }

    uint8_t scratch[1024];
    bridge_release_rx_paced(s);
    for (;;) {
        size_t cap = PCC_RING_CAP - pcc_ring_count(&s->rx_pre_ring);
        if (cap == 0)
            break;
        if (cap > sizeof(scratch))
            cap = sizeof(scratch);
        ssize_t n = bridge_read(s->fd, scratch, cap);
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
                open_pty(s);
            } else {
                on_peer_gone(s, "EOF");
            }
            break;
        }
        if (bridge_errno_would_block())
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
    if (s->fd < 0) {
        pcc_ring_clear(&s->tx_ring);
        return;
    }

    /* Baud-throttled path: emit one byte per tx_ns_per_byte interval. */
    if (s->tx_ns_per_byte > 0) {
        uint64_t now = mono_ns();
        if (s->tx_next_due_ns == 0)
            s->tx_next_due_ns = now;
        while (!pcc_ring_is_empty(&s->tx_ring) && now >= s->tx_next_due_ns) {
            uint8_t byte = (uint8_t)pcc_ring_pop(&s->tx_ring);
            ssize_t n = bridge_write(s->fd, &byte, 1);
            if (n == 1) {
                s->tx_next_due_ns += s->tx_ns_per_byte;
                if (s->tx_next_due_ns + 10ull * s->tx_ns_per_byte < now)
                    s->tx_next_due_ns = now;
                now = mono_ns();
                continue;
            }
            if (n < 0) {
                if (bridge_errno_would_block()) {
                    s->tx_ring.tail = (s->tx_ring.tail + PCC_RING_CAP - 1u)
                        % PCC_RING_CAP;
                    s->tx_ring.count++;
                    return;
                }
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
        size_t contig = PCC_RING_CAP - s->tx_ring.tail;
        if (contig > count)
            contig = count;
        ssize_t n = bridge_write(s->fd, &s->tx_ring.buf[s->tx_ring.tail], contig);
        if (n > 0) {
            s->tx_ring.tail = (s->tx_ring.tail + (size_t)n) % PCC_RING_CAP;
            s->tx_ring.count -= (size_t)n;
            continue;
        }
        if (n < 0) {
            if (bridge_errno_would_block())
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

static void bridge_tick_one(pcc_br_state_t *s)
{
    if (!s->armed)
        return;
    if (s->listen_fd >= 0)
        try_accept(s);
    else if (s->kind == PCC_BR_TCP_CONNECT || s->kind == PCC_BR_UNIX_CONNECT)
        try_reconnect(s);
    bridge_drain_tx(s);
    bridge_drain_rx(s);
}

static void bridge_shutdown_one(pcc_br_state_t *s)
{
    if (s->tee) {
        fflush(s->tee);
        fclose(s->tee);
        s->tee = NULL;
    }
    close_fd(&s->fd);
    close_fd(&s->listen_fd);
    close_fd(&s->pty_slave_fd);
    free(s->target);
    s->target = NULL;
    free(s->connect_host);
    s->connect_host = NULL;
    free(s->unix_path);
    s->unix_path = NULL;
    free(s->tee_path);
    s->tee_path = NULL;
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
    s->fd = PCC_INVALID_HANDLE;
    s->listen_fd = PCC_INVALID_HANDLE;
    s->pty_slave_fd = PCC_INVALID_HANDLE;
    s->uart_id = cfg->uart_id;
    s->wince_mode = cfg->wince_mode;

#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

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
            return false;
        }
        s->tee_path = strdup(cfg->tee_path);
        setvbuf(s->tee, NULL, _IOLBF, 0);
        fprintf(s->tee,
            "# pcconnect-bridge tee log; mono_ns dir count: hex bytes\n");
        fflush(s->tee);
    }

    bool ok = false;
    switch (s->kind) {
    case PCC_BR_PTY:           ok = open_pty(s);                   break;
    case PCC_BR_TCP_CONNECT:   ok = open_tcp_connect(s, s->target); break;
    case PCC_BR_TCP_LISTEN:    ok = open_tcp_listen(s, s->target);  break;
    case PCC_BR_UNIX_CONNECT:  ok = open_unix_connect(s, s->target); break;
    case PCC_BR_UNIX_LISTEN:   ok = open_unix_listen(s, s->target);  break;
    case PCC_BR_NONE:
    default:
        fprintf(stderr, "pcconnect-bridge: empty kind\n");
        break;
    }
    if (!ok) {
        if (s->tee) { fclose(s->tee); s->tee = NULL; }
        free(s->target); s->target = NULL;
        free(s->tee_path); s->tee_path = NULL;
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

bool pcconnect_bridge_uart_rx_available(void)
{
    pcc_br_state_t *s = wince_bridge();
    if (!s)
        return false;
    bridge_drain_rx(s);
    if (!bridge_data_path_connected(s) || !s->guest_uart_ready)
        return false;
    return !pcc_ring_is_empty(&s->rx_ring);
}

size_t pcconnect_bridge_uart_rx_count(void)
{
    pcc_br_state_t *s = wince_bridge();
    if (!s)
        return 0;
    bridge_drain_rx(s);
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

void pcconnect_bridge_uart_rx_clear(void)
{
    pcc_br_state_t *s = wince_bridge();
    if (!s)
        return;
    pcc_ring_clear(&s->rx_ring);
    pcc_ring_clear(&s->rx_pre_ring);
    s->rx_next_due_ns = 0;
    s->rx_pace_active = false;
}

void pcconnect_bridge_uart_tx_byte(uint8_t byte)
{
    pcc_br_state_t *s = wince_bridge();
    if (!s || !bridge_data_path_connected(s) || s->fd < 0) {
        if (s && s->tee)
            tee_write_dir(s, "G>H:drop", &byte, 1);
        return;
    }
    pcc_ring_push(&s->tx_ring, byte);
    tee_write_dir(s, "G>H", &byte, 1);
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
    bridge_drain_rx(s);
    if (!bridge_data_path_connected(s) || !s->guest_uart_ready)
        return 0;
    return pcc_ring_count(&s->rx_ring);
}

int serial_bridge_uart_rx_pop(const char *name)
{
    pcc_br_state_t *s = bridge_by_name(name);
    if (!s)
        return -1;
    bridge_drain_rx(s);
    if (!bridge_data_path_connected(s) || !s->guest_uart_ready)
        return -1;
    if (pcc_ring_is_empty(&s->rx_ring))
        return -1;
    return pcc_ring_pop(&s->rx_ring);
}

void serial_bridge_uart_rx_clear(const char *name)
{
    pcc_br_state_t *s = bridge_by_name(name);
    if (!s)
        return;
    pcc_ring_clear(&s->rx_ring);
    pcc_ring_clear(&s->rx_pre_ring);
    s->rx_next_due_ns = 0;
    s->rx_pace_active = false;
}

void serial_bridge_uart_tx_byte(const char *name, uint8_t byte)
{
    pcc_br_state_t *s = bridge_by_name(name);
    if (!s || !bridge_data_path_connected(s) || s->fd < 0) {
        if (s && s->tee)
            tee_write_dir(s, "G>H:drop", &byte, 1);
        return;
    }
    pcc_ring_push(&s->tx_ring, byte);
    tee_write_dir(s, "G>H", &byte, 1);
}
