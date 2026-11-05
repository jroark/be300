/* PC Card modem model; see modem.h. Result codes follow ITU-T V.250
 * (verbose form). Escape sequences (+++) are not modelled: the host hangs
 * up by dropping DTR, as Glaze's dialer does. */
#include "modem.h"
#include <stdio.h>
#include <string.h>

#if defined(_WIN32) || defined(__EMSCRIPTEN__)
#define MODEM_NO_SOCKETS 1
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

enum { RBR = 0, IER = 1, IIR = 2, LCR = 3, MCR = 4, LSR = 5, MSR = 6, SCR = 7 };
#define MSR_CTS 0x10
#define MSR_DSR 0x20
#define MSR_DCD 0x80

static void rx_put(modem_t *m, const void *data, size_t n)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < n && m->rx_count < MODEM_FIFO; i++)
        m->rx[(m->rx_head + m->rx_count++) % MODEM_FIFO] = p[i];
}

static void reply(modem_t *m, const char *text)
{
    rx_put(m, "\r\n", 2);
    rx_put(m, text, strlen(text));
    rx_put(m, "\r\n", 2);
}

static void hang_up(modem_t *m)
{
#ifndef MODEM_NO_SOCKETS
    if (m->sock >= 0)
        close(m->sock);
#endif
    m->sock = -1;
    m->online = false;
}

void modem_close(modem_t *m) { hang_up(m); }

void modem_reset(modem_t *m)
{
    hang_up(m);
    m->ier = m->mcr = m->scr = m->fcr = 0;
    m->lcr = 0x03;
    m->dll = 1;
    m->dlm = 0;
    m->rx_head = m->rx_count = 0;
    m->thre_pending = false;
    m->echo = true;
    m->verbose = true;
    m->line_len = 0;
}

void modem_init(modem_t *m, const char *peer)
{
    memset(m, 0, sizeof(*m));
    m->sock = -1;
    if (peer)
        snprintf(m->peer, sizeof(m->peer), "%s", peer);
    modem_reset(m);
}

static bool dial(modem_t *m)
{
#ifdef MODEM_NO_SOCKETS
    (void)m;
    return false;
#else
    char host[128];
    snprintf(host, sizeof(host), "%s", m->peer);
    char *port = strrchr(host, ':');
    if (!port)
        return false;
    *port++ = 0;
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res;
    if (getaddrinfo(host, port, &hints, &res))
        return false;
    int s = -1;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s < 0)
            continue;
        if (!connect(s, a->ai_addr, a->ai_addrlen))
            break;
        close(s);
        s = -1;
    }
    freeaddrinfo(res);
    if (s < 0)
        return false;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    m->sock = s;
    m->online = true;
    m->connects++;
    return true;
#endif
}

/* One command line (after "AT"); only what dialers send. */
static void command(modem_t *m, const char *line)
{
    if (strncasecmp(line, "AT", 2)) {
        return;                         /* not a command: ignored */
    }
    const char *p = line + 2;
    while (*p) {
        char c = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
        p++;
        if (c == 'D') {
            if (*p == 'T' || *p == 't' || *p == 'P' || *p == 'p')
                p++;
            fprintf(stderr, "[MODEM] dialling %s via %s\n", p, m->peer[0] ? m->peer : "(no peer)");
            if (m->peer[0] && dial(m))
                reply(m, "CONNECT 57600");
            else
                reply(m, "NO CARRIER");
            return;
        }
        if (c == 'H') { hang_up(m); while (*p >= '0' && *p <= '9') p++; continue; }
        if (c == 'Z') { m->echo = true; m->verbose = true; while (*p >= '0' && *p <= '9') p++; continue; }
        if (c == 'E') { m->echo = *p != '0'; if (*p >= '0' && *p <= '9') p++; continue; }
        if (c == 'V') { m->verbose = *p != '0'; if (*p >= '0' && *p <= '9') p++; continue; }
        if (c == 'I') {
            reply(m, "BE300 emulated modem");
            while (*p >= '0' && *p <= '9') p++;
            continue;
        }
        if (c == '&') { if (*p) p++; while (*p >= '0' && *p <= '9') p++; continue; }
        if (c == 'S') { while (*p && *p != ' ') p++; continue; }
        if ((c >= 'A' && c <= 'Z')) { while (*p >= '0' && *p <= '9') p++; continue; }
        /* spaces and anything else: skipped */
    }
    reply(m, "OK");
}

static void tx_byte(modem_t *m, uint8_t b)
{
    if (m->online) {
#ifndef MODEM_NO_SOCKETS
        if (send(m->sock, &b, 1, 0) != 1 && errno != EAGAIN) {
            hang_up(m);
            reply(m, "NO CARRIER");
        }
#endif
        return;
    }
    if (m->echo)
        rx_put(m, &b, 1);
    if (b == '\r') {
        m->line[m->line_len] = 0;
        m->line_len = 0;
        command(m, m->line);
    } else if (b >= ' ' && m->line_len + 1 < sizeof(m->line)) {
        m->line[m->line_len++] = (char)b;
    }
}

static uint8_t msr_lines(const modem_t *m)
{
    return (uint8_t)(MSR_CTS | MSR_DSR | (m->online ? MSR_DCD : 0));
}

uint8_t modem_read(modem_t *m, unsigned reg)
{
    switch (reg & 7) {
    case RBR:
        if (m->lcr & 0x80) return m->dll;
        if (!m->rx_count) return 0;
        {
            uint8_t b = m->rx[m->rx_head];
            m->rx_head = (m->rx_head + 1) % MODEM_FIFO;
            m->rx_count--;
            return b;
        }
    case IER:
        return (m->lcr & 0x80) ? m->dlm : m->ier;
    case IIR: {
        uint8_t fifo = (m->fcr & 1) ? 0xC0 : 0;
        if ((m->ier & 1) && m->rx_count) return fifo | 0x04;
        if ((m->ier & 2) && m->thre_pending) { m->thre_pending = false; return fifo | 0x02; }
        if ((m->ier & 8) && msr_lines(m) != m->msr_last) return fifo | 0x00;
        return fifo | 0x01;
    }
    case LCR: return m->lcr;
    case MCR: return m->mcr;
    case LSR: return (uint8_t)(0x60 | (m->rx_count ? 1 : 0));
    case MSR: {
        uint8_t now = msr_lines(m);
        uint8_t delta = (uint8_t)(((now ^ m->msr_last) >> 4) & 0x0B);
        m->msr_last = now;
        return now | delta;
    }
    default: return m->scr;
    }
}

void modem_write(modem_t *m, unsigned reg, uint8_t value)
{
    switch (reg & 7) {
    case RBR:
        if (m->lcr & 0x80) { m->dll = value; return; }
        tx_byte(m, value);
        m->thre_pending = true;
        return;
    case IER:
        if (m->lcr & 0x80) { m->dlm = value; return; }
        m->ier = value & 0x0F;
        if (value & 2) m->thre_pending = true;
        return;
    case IIR:
        m->fcr = value;
        if (value & 2) m->rx_head = m->rx_count = 0;
        return;
    case LCR: m->lcr = value; return;
    case MCR:
        /* &D2: DTR dropping hangs up. */
        if ((m->mcr & 1) && !(value & 1) && m->online) {
            hang_up(m);
            fprintf(stderr, "[MODEM] DTR dropped, hung up\n");
        }
        m->mcr = value;
        return;
    case SCR: m->scr = value; return;
    default: return;
    }
}

bool modem_irq_pending(const modem_t *m)
{
    if (!(m->mcr & 0x08)) return false;            /* OUT2 gates the line */
    return ((m->ier & 1) && m->rx_count) || ((m->ier & 2) && m->thre_pending) ||
           ((m->ier & 8) && msr_lines(m) != m->msr_last);
}

void modem_tick(modem_t *m)
{
#ifndef MODEM_NO_SOCKETS
    if (!m->online)
        return;
    uint8_t buf[512];
    size_t room = MODEM_FIFO - m->rx_count;
    if (!(m->mcr & 0x02) || room < sizeof(buf))     /* RTS off or full: wait */
        return;
    ssize_t n = recv(m->sock, buf, sizeof(buf), 0);
    if (n > 0) {
        rx_put(m, buf, (size_t)n);
    } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        hang_up(m);
        reply(m, "NO CARRIER");
        fprintf(stderr, "[MODEM] peer closed\n");
    }
#else
    (void)m;
#endif
}
