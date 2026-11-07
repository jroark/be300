/* CF data modem model; see modem.h. */
#include "modem.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define LSR_DR      0x01u
#define LSR_THRE    0x20u
#define LSR_TEMT    0x40u
#define MSR_CTS     0x10u
#define MSR_DSR     0x20u
#define MSR_DCD     0x80u
#define MSR_DDCD    0x08u
#define MCR_DTR     0x01u

static void push(cf_modem_t *m, uint8_t b)
{
    if (m->rx_count == CF_MODEM_RX)
        return;
    m->rx[(m->rx_head + m->rx_count++) % CF_MODEM_RX] = b;
}

static void say(cf_modem_t *m, const char *s)
{
    while (*s)
        push(m, (uint8_t)*s++);
}

/* Verbose results are framed in CR LF; numeric ones end in CR (ITU V.250). */
static void result(cf_modem_t *m, const char *text, int code)
{
    char buf[48];
    if (m->verbose)
        snprintf(buf, sizeof(buf), "\r\n%s\r\n", text);
    else
        snprintf(buf, sizeof(buf), "%d\r", code);
    say(m, buf);
}

void cf_modem_reset(cf_modem_t *m)
{
    void *opaque = m->opaque;
    bool (*dial)(void *, const char *) = m->dial;
    void (*hangup)(void *) = m->hangup;
    void (*tx)(void *, const uint8_t *, size_t) = m->tx;
    bool gps = m->gps;
    memset(m, 0, sizeof(*m));
    m->opaque = opaque; m->dial = dial; m->hangup = hangup; m->tx = tx;
    m->gps = gps;
    m->echo = m->verbose = !gps;
    m->online = gps;
    m->thre_irq = true;
    m->connect_rate = 52000;
}

static void hang_up(cf_modem_t *m, bool report)
{
    if (!m->online)
        return;
    m->online = false;
    m->msr_delta |= MSR_DDCD;
    if (m->hangup)
        m->hangup(m->opaque);
    if (report)
        result(m, "NO CARRIER", 3);
}

void cf_modem_carrier_lost(cf_modem_t *m)
{
    hang_up(m, true);
}

/* Skips a parameter's digits. */
static const char *digits(const char *p, int *value)
{
    int v = 0;
    while (isdigit((unsigned char)*p))
        v = v * 10 + (*p++ - '0');
    if (value)
        *value = v;
    return p;
}

static void execute(cf_modem_t *m, const char *line)
{
    while (*line == ' ')
        line++;
    if (toupper((unsigned char)line[0]) != 'A' || toupper((unsigned char)line[1]) != 'T') {
        if (line[0])
            result(m, "ERROR", 4);
        return;
    }
    const char *p = line + 2;
    while (*p) {
        int c = toupper((unsigned char)*p++), v;
        switch (c) {
        case ' ':
            break;
        case 'Z':
            p = digits(p, NULL);
            m->echo = m->verbose = true;
            break;
        case 'E':
            p = digits(p, &v);
            m->echo = v != 0;
            break;
        case 'V':
            p = digits(p, &v);
            m->verbose = v != 0;
            break;
        case 'I':
            p = digits(p, &v);
            if (v == 3) {
                say(m, "\r\nL27-01\r\n");
            } else if (v == 0) {
                say(m, "\r\n56000\r\n");
            }
            break;
        case 'H':
            p = digits(p, NULL);
            hang_up(m, false);
            break;
        case 'D': {
            /* The rest of the line is the dial string; ';' would return to
             * command mode, which a data call doesn't use. */
            char number[64];
            unsigned n = 0;
            for (; *p && *p != ';' && n + 1 < sizeof(number); p++)
                if (isdigit((unsigned char)*p) || strchr("*#", *p))
                    number[n++] = *p;
            number[n] = 0;
            if (!n) {
                result(m, "OK", 0);     /* as the RC56LD answers "ATDT," */
                return;
            }
            if (!m->dial || !m->dial(m->opaque, number)) {
                result(m, "NO CARRIER", 3);
                return;
            }
            char text[32];
            snprintf(text, sizeof(text), "CONNECT %u", m->connect_rate);
            result(m, text, 1);
            m->online = true;
            m->msr_delta |= MSR_DDCD;
            return;
        }
        case '&':
            if (*p)
                p++;
            p = digits(p, NULL);
            break;
        case 'S':
            p = digits(p, NULL);
            if (*p == '=')
                p = digits(p + 1, NULL);
            else if (*p == '?') {
                p++;
                say(m, "\r\n000\r\n");
            }
            break;
        default:
            if (!isalpha(c)) {
                result(m, "ERROR", 4);
                return;
            }
            p = digits(p, NULL);
            break;
        }
    }
    result(m, "OK", 0);
}

static void command_byte(cf_modem_t *m, uint8_t b)
{
    if (m->echo)
        push(m, b);
    if (b == '\r') {
        m->cmd[m->cmd_len] = 0;
        m->cmd_len = 0;
        execute(m, m->cmd);
    } else if (b == 8 || b == 127) {
        if (m->cmd_len)
            m->cmd_len--;
    } else if (b >= 32 && b < 127 && m->cmd_len + 1 < sizeof(m->cmd)) {
        m->cmd[m->cmd_len++] = (char)b;
    }
}

uint8_t cf_modem_read(cf_modem_t *m, unsigned reg)
{
    bool dlab = m->lcr & 0x80;
    switch (reg & 7) {
    case 0:
        if (dlab)
            return m->dll;
        if (!m->rx_count)
            return 0;
        {
            uint8_t b = m->rx[m->rx_head];
            m->rx_head = (m->rx_head + 1) % CF_MODEM_RX;
            m->rx_count--;
            return b;
        }
    case 1:
        return dlab ? m->dlm : m->ier;
    case 2: {
        uint8_t fifo = m->fcr & 1 ? 0xC0 : 0;
        if ((m->ier & 1) && m->rx_count)
            return fifo | 0x04;
        if ((m->ier & 2) && m->thre_irq) {
            m->thre_irq = false;        /* reading IIR retires THRE */
            return fifo | 0x02;
        }
        if ((m->ier & 8) && m->msr_delta)
            return fifo | 0x00;
        return fifo | 0x01;
    }
    case 3:
        return m->lcr;
    case 4:
        return m->mcr;
    case 5:
        return (m->rx_count ? LSR_DR : 0) | LSR_THRE | LSR_TEMT;
    case 6: {
        uint8_t v = MSR_CTS | MSR_DSR | (m->online && !m->gps ? MSR_DCD : 0) | m->msr_delta;
        m->msr_delta = 0;
        return v;
    }
    default:
        return m->scr;
    }
}

void cf_modem_write(cf_modem_t *m, unsigned reg, uint8_t value)
{
    bool dlab = m->lcr & 0x80;
    switch (reg & 7) {
    case 0:
        if (dlab) {
            m->dll = value;
            return;
        }
        if (m->online || m->gps) {
            if (m->tx)
                m->tx(m->opaque, &value, 1);
        } else {
            command_byte(m, value);
        }
        m->thre_irq = true;
        return;
    case 1:
        if (dlab)
            m->dlm = value;
        else {
            /* Enabling the THRE interrupt with THR empty raises it. */
            if ((value & 2) && !(m->ier & 2))
                m->thre_irq = true;
            m->ier = value & 0x0f;
        }
        return;
    case 2:
        m->fcr = value;
        if (value & 2) {
            m->rx_head = m->rx_count = 0;
        }
        return;
    case 3:
        m->lcr = value;
        return;
    case 4: {
        bool dtr_was = m->mcr & MCR_DTR;
        m->mcr = value;
        if (m->gps)
            return;
        /* AT&D2: dropping DTR hangs up. */
        if (dtr_was && !(value & MCR_DTR))
            hang_up(m, true);
        return;
    }
    case 7:
        m->scr = value;
        return;
    default:
        return;
    }
}

size_t cf_modem_rx_space(const cf_modem_t *m)
{
    return CF_MODEM_RX - m->rx_count;
}

size_t cf_modem_receive(cf_modem_t *m, const uint8_t *data, size_t len)
{
    size_t n = 0;
    if (!m->online)
        return 0;
    while (n < len && m->rx_count < CF_MODEM_RX)
        push(m, data[n++]);
    return n;
}

bool cf_modem_irq_pending(const cf_modem_t *m)
{
    return ((m->ier & 1) && m->rx_count) || ((m->ier & 2) && m->thre_irq) ||
           ((m->ier & 8) && m->msr_delta);
}
