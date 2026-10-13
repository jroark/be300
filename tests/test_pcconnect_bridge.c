/*
 * Transport tests for the WinCE PC Connect bridge (src/pcconnect_bridge.c).
 *
 * Drives the bridge through the same entry points dev_ns16550.c uses, with a
 * Unix-socket peer standing in for the PC side, and checks that bulk transfers
 * in both directions arrive complete and in order.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <unistd.h>

#include "pcconnect_bridge.h"

#define UART "vrc4173siu"

static int failures;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);    \
        fprintf(stderr, __VA_ARGS__);                           \
        fputc('\n', stderr);                                    \
        failures++;                                             \
    }                                                           \
} while (0)

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_us(long us)
{
    struct timespec ts = { us / 1000000, (us % 1000000) * 1000 };
    nanosleep(&ts, NULL);
}

static uint8_t pattern(size_t i)
{
    /* Never 0x55 at index 0: the bridge drops leading idle 0x55 bytes. */
    return (uint8_t)(i * 7u + 1u);
}

/* Bring up a WinCE-mode bridge on a fresh Unix socket and attach a peer. */
static int setup(const char *dir, int n)
{
    char path[200];
    pcc_bridge_config_t cfg;
    struct sockaddr_un sa;
    int peer;

    snprintf(path, sizeof(path), "%s/b%d.sock", dir, n);
    memset(&cfg, 0, sizeof(cfg));
    cfg.uart_id = PCC_UART_VRC4173_SIU;
    cfg.kind = PCC_BR_UNIX_LISTEN;
    cfg.target = path;
    cfg.wince_mode = true;
    cfg.baud = 115200;
    if (!pcconnect_bridge_configure(&cfg)) {
        fprintf(stderr, "configure failed\n");
        exit(2);
    }

    peer = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    if (connect(peer, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        perror("connect");
        exit(2);
    }
    fcntl(peer, F_SETFL, fcntl(peer, F_GETFL, 0) | O_NONBLOCK);
    pcconnect_bridge_tick();                    /* accept */

    pcconnect_bridge_set_cable_connected(true);
    pcconnect_bridge_note_uart_config(UART, 0x03, 0x03, 0x0f, 10, 0);
    return peer;
}

static void teardown(int peer)
{
    close(peer);
    pcconnect_bridge_shutdown();
}

static size_t peer_drain(int peer, uint8_t *buf, size_t have, size_t cap)
{
    for (;;) {
        ssize_t n;

        if (have >= cap)
            return have;
        n = read(peer, buf + have, cap - have);
        if (n <= 0)
            return have;
        have += (size_t)n;
    }
}

/*
 * The emulated SIU reports THRE as always ready, so the guest can queue a
 * whole frame at CPU speed. Every byte must still reach the host, in order,
 * at no more than the configured line rate.
 */
static void test_tx_burst_delivered(const char *dir)
{
    enum { TOTAL = 20000 };
    static uint8_t got[TOTAL + 64];
    size_t have = 0;
    double t0, dt;
    int peer = setup(dir, 1);

    for (size_t i = 0; i < TOTAL; i++)
        pcconnect_bridge_uart_tx_byte(pattern(i));
    t0 = now_s();
    while (have < TOTAL && now_s() - t0 < 10.0) {
        pcconnect_bridge_tick();
        have = peer_drain(peer, got, have, sizeof(got));
        sleep_us(200);
    }
    dt = now_s() - t0;

    CHECK(have == TOTAL, "tx: received %zu of %d bytes", have, TOTAL);
    for (size_t i = 0; i < have && i < TOTAL; i++) {
        if (got[i] != pattern(i)) {
            CHECK(0, "tx: byte %zu is 0x%02x, want 0x%02x", i, got[i],
                pattern(i));
            break;
        }
    }
    /* 8N1 at 115200 baud is ~86.8 us/byte; allow generous slack. */
    CHECK(dt > TOTAL * 86.8e-6 * 0.8, "tx: finished in %.3fs, faster than "
        "the line rate", dt);
    teardown(peer);
}

/* Overflowing the queue must not corrupt bytes already queued. */
static void test_tx_overflow_keeps_queued_bytes(const char *dir)
{
    enum { TOTAL = 300 * 1024, CHECKED = 8192 };
    static uint8_t got[CHECKED];
    size_t have = 0;
    double t0;
    int peer = setup(dir, 2);

    for (size_t i = 0; i < TOTAL; i++)
        pcconnect_bridge_uart_tx_byte(pattern(i));
    t0 = now_s();
    while (have < CHECKED && now_s() - t0 < 3.0) {
        pcconnect_bridge_tick();
        have = peer_drain(peer, got, have, sizeof(got));
        sleep_us(200);
    }

    CHECK(have == CHECKED, "overflow: only %zu bytes delivered", have);
    for (size_t i = 0; i < have; i++) {
        if (got[i] != pattern(i)) {
            CHECK(0, "overflow: byte %zu is 0x%02x, want 0x%02x", i, got[i],
                pattern(i));
            break;
        }
    }
    teardown(peer);
}

/* Pop everything the guest can see, optionally poking the UART midway. */
static size_t guest_receive(int peer, const uint8_t *src, size_t total,
    uint8_t *out, size_t cap, int poke)
{
    size_t written = 0, have = 0;
    double t0 = now_s();
    int poked = 0;

    while (now_s() - t0 < 5.0) {
        if (written < total) {
            size_t chunk = total - written < 32 ? total - written : 32;
            ssize_t n = write(peer, src + written, chunk);
            if (n > 0)
                written += (size_t)n;
        }
        pcconnect_bridge_tick();
        if (!poked && have >= 200) {
            poked = 1;
            if (poke == 1) {
                pcconnect_bridge_uart_rx_clear();
            } else if (poke == 2) {
                /* serial.dll reprograms the divisor: DLAB on, then off. */
                pcconnect_bridge_note_uart_config(UART, 0x83, 0x03, 0x0f,
                    10, 1);
                for (int i = 0; i < 50; i++) {
                    if (written < total) {
                        size_t chunk = total - written < 32 ?
                            total - written : 32;
                        ssize_t n = write(peer, src + written, chunk);
                        if (n > 0)
                            written += (size_t)n;
                    }
                    pcconnect_bridge_tick();
                    sleep_us(100);
                }
                pcconnect_bridge_note_uart_config(UART, 0x03, 0x03, 0x0f,
                    10, 0);
            }
        }
        while (have < cap && pcconnect_bridge_uart_rx_count() > 0) {
            int b = pcconnect_bridge_uart_rx_pop();
            if (b < 0)
                break;
            out[have++] = (uint8_t)b;
        }
        if (written == total && have > 0 &&
            pcconnect_bridge_uart_rx_count() == 0 && now_s() - t0 > 1.0)
            break;
        sleep_us(50);
    }
    return have;
}

static void test_rx_fifo_reset_keeps_wire_bytes(const char *dir)
{
    enum { TOTAL = 3000 };
    static uint8_t src[TOTAL], got[TOTAL];
    size_t have;
    int peer = setup(dir, 3);

    for (size_t i = 0; i < TOTAL; i++)
        src[i] = pattern(i);
    have = guest_receive(peer, src, TOTAL, got, TOTAL, 1);

    /* Only the 16-byte receive FIFO may be lost by FCR1. */
    CHECK(have >= TOTAL - 16, "rx fifo reset: received %zu of %d", have,
        TOTAL);
    CHECK(have > 0 && got[have - 1] == src[TOTAL - 1],
        "rx fifo reset: stream tail missing");
    teardown(peer);
}

static void test_rx_dlab_toggle_loses_nothing(const char *dir)
{
    enum { TOTAL = 3000 };
    static uint8_t src[TOTAL], got[TOTAL];
    size_t have;
    int peer = setup(dir, 4);

    for (size_t i = 0; i < TOTAL; i++)
        src[i] = pattern(i);
    have = guest_receive(peer, src, TOTAL, got, TOTAL, 2);

    CHECK(have == TOTAL, "rx dlab: received %zu of %d", have, TOTAL);
    CHECK(memcmp(got, src, have) == 0, "rx dlab: stream corrupted");
    teardown(peer);
}

/* tcp:HOST:PORT must finish a non-blocking connect() and carry bytes. */
static void test_tcp_connect_mode(void)
{
    pcc_bridge_config_t cfg;
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    char target[64];
    int lfd, peer = -1, one = 1;
    uint8_t got[4];
    size_t have = 0;
    double t0;

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        listen(lfd, 1) != 0 ||
        getsockname(lfd, (struct sockaddr *)&sa, &len) != 0) {
        perror("listener");
        exit(2);
    }
    fcntl(lfd, F_SETFL, fcntl(lfd, F_GETFL, 0) | O_NONBLOCK);
    snprintf(target, sizeof(target), "127.0.0.1:%d", ntohs(sa.sin_port));

    memset(&cfg, 0, sizeof(cfg));
    cfg.uart_id = PCC_UART_VRC4173_SIU;
    cfg.kind = PCC_BR_TCP_CONNECT;
    cfg.target = target;
    cfg.wince_mode = true;
    cfg.baud = 115200;
    if (!pcconnect_bridge_configure(&cfg)) {
        fprintf(stderr, "configure tcp failed\n");
        exit(2);
    }
    pcconnect_bridge_set_cable_connected(true);
    pcconnect_bridge_note_uart_config(UART, 0x03, 0x03, 0x0f, 10, 0);

    t0 = now_s();
    while (now_s() - t0 < 3.0 && have < sizeof(got)) {
        pcconnect_bridge_tick();
        if (peer < 0) {
            peer = accept(lfd, NULL, NULL);
            if (peer >= 0)
                fcntl(peer, F_SETFL, fcntl(peer, F_GETFL, 0) | O_NONBLOCK);
        }
        if (peer >= 0 && pcconnect_bridge_cable_connected()) {
            static size_t sent;
            if (sent < sizeof(got))
                pcconnect_bridge_uart_tx_byte(pattern(sent++));
        }
        if (peer >= 0)
            have = peer_drain(peer, got, have, sizeof(got));
        sleep_us(500);
    }
    CHECK(peer >= 0, "tcp: bridge never connected to the listener");
    CHECK(have == sizeof(got), "tcp: received %zu of %zu bytes", have,
        sizeof(got));
    if (peer >= 0)
        close(peer);
    close(lfd);
    pcconnect_bridge_shutdown();
}

int main(void)
{
    char dir[] = "/tmp/pccbr.XXXXXX";

    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    test_tx_burst_delivered(dir);
    test_tx_overflow_keeps_queued_bytes(dir);
    test_rx_fifo_reset_keeps_wire_bytes(dir);
    test_rx_dlab_toggle_loses_nothing(dir);
    test_tcp_connect_mode();

    for (int i = 1; i <= 4; i++) {
        char p[256];
        snprintf(p, sizeof(p), "%s/b%d.sock", dir, i);
        unlink(p);
    }
    rmdir(dir);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("pcconnect bridge tests passed\n");
    return 0;
}
