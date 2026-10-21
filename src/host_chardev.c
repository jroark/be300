/*
 * host_chardev — non-blocking host byte stream (TCP / Unix socket / PTY)
 * shared by the PC Connect serial bridge and the usbredir USB device.
 * Factored out of pcconnect_bridge.c; behaviour is unchanged.
 */

/* glibc declares posix_openpt/grantpt/unlockpt/ptsname only for XSI; without
 * this, ptsname() was implicitly int and truncated the slave name pointer
 * on 64-bit Linux. */
#define _GNU_SOURCE

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
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#endif

#include "host_chardev.h"

#ifdef _WIN32
#define HC_SOCKOPT_PTR(p) ((const char *)(p))
#else
#define HC_SOCKOPT_PTR(p) (p)
#endif

#define HC_RECONNECT_NS 500000000ull

static uint64_t mono_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void hc_trace(host_chardev_t *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void hc_trace(host_chardev_t *c, const char *fmt, ...)
{
    if (!c->trace)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", c->tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void set_nonblocking(host_chardev_handle_t fd)
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

static void close_fd(host_chardev_handle_t *fd)
{
    if (*fd >= 0) {
#ifdef _WIN32
        closesocket((SOCKET)*fd);
#else
        close(*fd);
#endif
        *fd = HOST_CHARDEV_INVALID;
    }
}

ssize_t host_chardev_read(host_chardev_t *c, void *buf, size_t len)
{
#ifdef _WIN32
    int n = recv((SOCKET)c->fd, (char *)buf, (int)len, 0);
    if (n < 0)
        errno = WSAGetLastError();
    return n;
#else
    return read(c->fd, buf, len);
#endif
}

ssize_t host_chardev_write(host_chardev_t *c, const void *buf, size_t len)
{
#ifdef _WIN32
    int n = send((SOCKET)c->fd, (const char *)buf, (int)len, 0);
    if (n < 0)
        errno = WSAGetLastError();
    return n;
#else
    return write(c->fd, buf, len);
#endif
}

bool host_chardev_errno_would_block(void)
{
#ifdef _WIN32
    return errno == WSAEWOULDBLOCK || errno == WSAEINPROGRESS ||
        errno == WSAEALREADY;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

static bool errno_connect_pending(void)
{
#ifdef _WIN32
    return host_chardev_errno_would_block();
#else
    return errno == EINPROGRESS || errno == EALREADY ||
        host_chardev_errno_would_block();
#endif
}

/* ---------- spec parsing ---------- */

bool host_chardev_parse_spec(const char *spec, host_chardev_kind_t *kind,
    const char **target)
{
    if (!spec || !kind || !target)
        return false;
    *kind = HOST_CHARDEV_NONE;
    *target = NULL;

    if (strncmp(spec, "tcp:", 4) == 0) {
        *kind = HOST_CHARDEV_TCP_CONNECT;
        *target = spec + 4;
        if (!strchr(*target, ':')) {
            fprintf(stderr,
                "pcconnect-bridge: tcp: spec needs host:port (got '%s')\n",
                *target);
            return false;
        }
    } else if (strncmp(spec, "tcp-listen:", 11) == 0) {
        *kind = HOST_CHARDEV_TCP_LISTEN;
        *target = spec + 11;
    } else if (strncmp(spec, "unix:", 5) == 0) {
        *kind = HOST_CHARDEV_UNIX_CONNECT;
        *target = spec + 5;
    } else if (strncmp(spec, "unix-listen:", 12) == 0) {
        *kind = HOST_CHARDEV_UNIX_LISTEN;
        *target = spec + 12;
    } else if (strcmp(spec, "pty:auto") == 0) {
        *kind = HOST_CHARDEV_PTY;
        *target = "auto";
    } else {
        fprintf(stderr,
            "pcconnect-bridge: unknown spec '%s' (expected tcp:HOST:PORT, "
            "tcp-listen:PORT[@ADDR], unix:/PATH, unix-listen:/PATH, pty:auto)\n",
            spec);
        return false;
    }
    return true;
}

/* ---------- openers ---------- */

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

static host_chardev_handle_t tcp_socket(void)
{
    host_chardev_handle_t sk = (host_chardev_handle_t)socket(AF_INET,
        SOCK_STREAM, 0);
    if (sk < 0)
        return sk;
    int one = 1;
    setsockopt(sk, IPPROTO_TCP, TCP_NODELAY, HC_SOCKOPT_PTR(&one),
        sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(sk, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    set_nonblocking(sk);
    return sk;
}

static bool open_tcp_connect(host_chardev_t *c, const char *spec)
{
    const char *host_in = NULL;
    int port = 0;
    if (!split_host_port(spec, &host_in, &port) || !host_in) {
        fprintf(stderr, "[%s] tcp: spec needs HOST:PORT (got '%s')\n",
            c->tag, spec);
        return false;
    }
    size_t host_len = (size_t)(strrchr(spec, ':') - host_in);
    char *host = malloc(host_len + 1);
    if (!host) {
        fprintf(stderr, "[%s] oom\n", c->tag);
        return false;
    }
    memcpy(host, host_in, host_len);
    host[host_len] = '\0';

    c->connect_host = host;
    c->connect_port = port;

    host_chardev_handle_t sk = tcp_socket();
    if (sk < 0) {
        fprintf(stderr, "[%s] socket: %s\n", c->tag, strerror(errno));
        return false;
    }

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        fprintf(stderr,
            "[%s] inet_pton('%s') failed; only IPv4 dotted-quad "
            "supported (use 127.0.0.1 etc.)\n", c->tag, host);
        close_fd(&sk);
        return false;
    }
    int rc = connect(sk, (struct sockaddr *)&sa, sizeof(sa));
#ifdef _WIN32
    if (rc < 0)
        errno = WSAGetLastError();
#endif
    if (rc < 0 && !errno_connect_pending()) {
        fprintf(stderr, "[%s] tcp connect %s:%d failed (%s); will retry\n",
            c->tag, host, port, strerror(errno));
        close_fd(&sk);
        c->next_reconnect_ns = mono_ns() + HC_RECONNECT_NS;
        return true;
    }
    c->fd = sk;
    c->connecting = rc < 0;
    if (!c->connecting) {
        fprintf(stderr, "[%s] tcp connected to %s:%d\n", c->tag, host, port);
        c->new_peer = true;
    }
    return true;
}

static bool open_tcp_listen(host_chardev_t *c, const char *spec)
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
        fprintf(stderr, "[%s] tcp-listen: spec needs PORT[@ADDR] (got '%s')\n",
            c->tag, spec);
        free(target);
        return false;
    }
    host_chardev_handle_t sk = (host_chardev_handle_t)socket(AF_INET,
        SOCK_STREAM, 0);
    if (sk < 0) {
        fprintf(stderr, "[%s] socket: %s\n", c->tag, strerror(errno));
        free(target);
        return false;
    }
    int one = 1;
    setsockopt(sk, SOL_SOCKET, SO_REUSEADDR, HC_SOCKOPT_PTR(&one),
        sizeof(one));
    set_nonblocking(sk);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_addr, &sa.sin_addr) != 1) {
        fprintf(stderr, "[%s] tcp-listen bind addr '%s' must be IPv4\n",
            c->tag, bind_addr);
        close_fd(&sk);
        free(target);
        return false;
    }
    if (bind(sk, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "[%s] bind %s:%ld: %s\n",
                c->tag, bind_addr, port, strerror(errno));
        close_fd(&sk);
        free(target);
        return false;
    }
    if (listen(sk, 1) != 0) {
        fprintf(stderr, "[%s] listen: %s\n", c->tag, strerror(errno));
        close_fd(&sk);
        free(target);
        return false;
    }
    c->listen_fd = sk;
    fprintf(stderr, "[%s] tcp listening on %s:%ld; awaiting peer\n",
        c->tag, bind_addr, port);
    free(target);
    return true;
}

#ifndef _WIN32
static int unix_socket(const char *path, struct sockaddr_un *sa)
{
    memset(sa, 0, sizeof(*sa));
    sa->sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strncpy(sa->sun_path, path, sizeof(sa->sun_path) - 1);
    int sk = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sk < 0)
        return -1;
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(sk, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    set_nonblocking(sk);
    return sk;
}
#endif

static bool open_unix(host_chardev_t *c, const char *path, bool listening)
{
#ifdef _WIN32
    fprintf(stderr, "[%s] unix sockets are unsupported on Windows: %s\n",
        c->tag, path ? path : "(null)");
    (void)listening;
    return false;
#else
    struct sockaddr_un sa;

    c->unix_path = strdup(path);
    if (!c->unix_path) return false;
    int sk = unix_socket(path, &sa);
    if (sk < 0) {
        fprintf(stderr, "[%s] unix socket '%s': %s\n", c->tag, path,
            strerror(errno));
        return false;
    }
    if (listening) {
        unlink(path);
        if (bind(sk, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            fprintf(stderr, "[%s] bind unix '%s': %s\n",
                    c->tag, path, strerror(errno));
            close(sk);
            return false;
        }
        if (listen(sk, 1) != 0) {
            fprintf(stderr, "[%s] listen: %s\n", c->tag, strerror(errno));
            close(sk);
            return false;
        }
        c->listen_fd = sk;
        fprintf(stderr, "[%s] unix listening on %s; awaiting peer\n",
            c->tag, path);
        return true;
    }
    int rc = connect(sk, (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0 && !errno_connect_pending()) {
        fprintf(stderr, "[%s] unix connect '%s' failed (%s); will retry\n",
            c->tag, path, strerror(errno));
        close(sk);
        c->next_reconnect_ns = mono_ns() + HC_RECONNECT_NS;
        return true;
    }
    c->fd = sk;
    c->connecting = rc < 0;
    if (!c->connecting) {
        fprintf(stderr, "[%s] unix connected to %s\n", c->tag, path);
        c->new_peer = true;
    }
    return true;
#endif
}

static bool open_pty(host_chardev_t *c)
{
#ifdef _WIN32
    fprintf(stderr, "[%s] pty mode is unsupported on Windows\n", c->tag);
    return false;
#else
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        fprintf(stderr, "[%s] posix_openpt: %s\n", c->tag, strerror(errno));
        return false;
    }
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        fprintf(stderr, "[%s] grantpt/unlockpt: %s\n", c->tag,
            strerror(errno));
        close(master);
        return false;
    }
    const char *slave = ptsname(master);
    if (!slave) {
        fprintf(stderr, "[%s] ptsname returned NULL\n", c->tag);
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
    if (c->pty_slave_fd >= 0)
        close_fd(&c->pty_slave_fd);
    {
        int slave_fd = open(slave, O_RDWR | O_NOCTTY);
        if (slave_fd < 0) {
            fprintf(stderr,
                "[%s] warning: could not open pty slave %s for raw setup: %s\n",
                c->tag, slave, strerror(errno));
        } else {
            struct termios tio;
            if (tcgetattr(slave_fd, &tio) == 0) {
                cfmakeraw(&tio);
                tio.c_cc[VMIN] = 1;
                tio.c_cc[VTIME] = 0;
                if (tcsetattr(slave_fd, TCSANOW, &tio) != 0)
                    fprintf(stderr,
                        "[%s] warning: tcsetattr(slave) raw failed: %s\n",
                        c->tag, strerror(errno));
            }
            c->pty_slave_fd = slave_fd;
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
    c->fd = master;
    fprintf(stderr, "[%s] pty slave=%s\n", c->tag, slave);
    fprintf(stderr, "[%s]   attach with: screen %s 115200,cs8\n",
            c->tag, slave);
    return true;
#endif
}

bool host_chardev_reopen_pty(host_chardev_t *c)
{
    return c->kind == HOST_CHARDEV_PTY && open_pty(c);
}

/* ---------- lifecycle ---------- */

void host_chardev_init(host_chardev_t *c, const char *tag, bool trace)
{
    memset(c, 0, sizeof(*c));
    c->tag = tag;
    c->trace = trace;
    c->fd = HOST_CHARDEV_INVALID;
    c->listen_fd = HOST_CHARDEV_INVALID;
    c->pty_slave_fd = HOST_CHARDEV_INVALID;
}

bool host_chardev_open(host_chardev_t *c, host_chardev_kind_t kind,
    const char *target)
{
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    c->kind = kind;
    switch (kind) {
    case HOST_CHARDEV_PTY:          return open_pty(c);
    case HOST_CHARDEV_TCP_CONNECT:  return open_tcp_connect(c, target);
    case HOST_CHARDEV_TCP_LISTEN:   return open_tcp_listen(c, target);
    case HOST_CHARDEV_UNIX_CONNECT: return open_unix(c, target, false);
    case HOST_CHARDEV_UNIX_LISTEN:  return open_unix(c, target, true);
    case HOST_CHARDEV_NONE:
    default:
        fprintf(stderr, "[%s] empty kind\n", c->tag);
        return false;
    }
}

void host_chardev_close(host_chardev_t *c)
{
    close_fd(&c->fd);
    close_fd(&c->listen_fd);
    close_fd(&c->pty_slave_fd);
    free(c->connect_host);
    c->connect_host = NULL;
    free(c->unix_path);
    c->unix_path = NULL;
    c->connecting = false;
    c->new_peer = false;
}

static void try_accept(host_chardev_t *c)
{
    if (c->listen_fd < 0 || c->fd >= 0)
        return;
#ifdef _WIN32
    host_chardev_handle_t sk = (host_chardev_handle_t)accept(
        (SOCKET)c->listen_fd, NULL, NULL);
#else
    host_chardev_handle_t sk = accept(c->listen_fd, NULL, NULL);
#endif
    if (sk < 0) {
#ifdef _WIN32
        errno = WSAGetLastError();
#endif
        if (host_chardev_errno_would_block() || errno == EINTR)
            return;
        fprintf(stderr, "[%s] accept: %s\n", c->tag, strerror(errno));
        return;
    }
    set_nonblocking(sk);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(sk, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    c->fd = sk;
    c->new_peer = true;
    hc_trace(c, "peer accepted on fd=%lld", (long long)sk);
}

static void try_reconnect(host_chardev_t *c)
{
    if (c->fd >= 0)
        return;
    uint64_t now = mono_ns();
    if (now < c->next_reconnect_ns)
        return;
    c->next_reconnect_ns = now + HC_RECONNECT_NS;
    if (c->kind == HOST_CHARDEV_TCP_CONNECT) {
        host_chardev_handle_t sk = tcp_socket();
        if (sk < 0) return;
        struct sockaddr_in sa = {0};
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)c->connect_port);
        inet_pton(AF_INET, c->connect_host, &sa.sin_addr);
        int rc = connect(sk, (struct sockaddr *)&sa, sizeof(sa));
#ifdef _WIN32
        if (rc < 0)
            errno = WSAGetLastError();
#endif
        if (rc == 0 || (rc < 0 && errno_connect_pending())) {
            c->fd = sk;
            c->connecting = rc < 0;
            hc_trace(c, "tcp reconnect to %s:%d in progress",
                c->connect_host, c->connect_port);
        } else {
            close_fd(&sk);
        }
#ifndef _WIN32
    } else if (c->kind == HOST_CHARDEV_UNIX_CONNECT) {
        struct sockaddr_un sa;
        int sk = unix_socket(c->unix_path, &sa);
        if (sk < 0) return;
        int rc = connect(sk, (struct sockaddr *)&sa, sizeof(sa));
        if (rc == 0 || (rc < 0 && errno_connect_pending())) {
            c->fd = sk;
            c->connecting = rc < 0;
            hc_trace(c, "unix reconnect to %s in progress", c->unix_path);
        } else {
            close(sk);
        }
#endif
    }
}

void host_chardev_tick(host_chardev_t *c)
{
    if (c->listen_fd >= 0)
        try_accept(c);
    else if (c->kind == HOST_CHARDEV_TCP_CONNECT ||
             c->kind == HOST_CHARDEV_UNIX_CONNECT)
        try_reconnect(c);
}

bool host_chardev_ready(host_chardev_t *c)
{
    fd_set wfds;
    struct timeval tv = { 0, 0 };
    int err = 0;
    socklen_t len = sizeof(err);
    int rc;

    if (c->fd < 0)
        return false;
    if (!c->connecting)
        return true;

    FD_ZERO(&wfds);
    FD_SET(c->fd, &wfds);
    rc = select((int)c->fd + 1, NULL, &wfds, NULL, &tv);
    if (rc == 0)
        return false;
    if (rc < 0 ||
        getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0)
        err = errno;
    c->connecting = false;
    if (err != 0) {
        hc_trace(c, "connect failed (%s); will retry", strerror(err));
        close_fd(&c->fd);
        return false;
    }
    if (c->kind == HOST_CHARDEV_TCP_CONNECT)
        fprintf(stderr, "[%s] tcp connected to %s:%d\n", c->tag,
            c->connect_host, c->connect_port);
    else
        fprintf(stderr, "[%s] unix connected to %s\n", c->tag, c->unix_path);
    c->new_peer = true;
    return true;
}

bool host_chardev_take_new_peer(host_chardev_t *c)
{
    bool v = c->new_peer;
    c->new_peer = false;
    return v;
}

void host_chardev_drop_peer(host_chardev_t *c)
{
    close_fd(&c->fd);
    c->connecting = false;
    c->new_peer = false;
}
