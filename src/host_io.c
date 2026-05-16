#include <errno.h>
#include <stddef.h>
#ifdef _WIN32
#include <conio.h>
#else
#include <unistd.h>
#include <sys/select.h>
#endif

#include "host_io.h"

static host_io_serial_sink_t g_serial_sink = NULL;
static void *g_serial_sink_data = NULL;
static bool g_stdout_enabled = true;
static bool g_console_stdin_enabled = true;
static bool g_stdin_eof = false;

void host_io_set_serial_sink(host_io_serial_sink_t sink, void *user_data)
{
    g_serial_sink = sink;
    g_serial_sink_data = user_data;
}

void host_io_set_stdout_enabled(bool enabled)
{
    g_stdout_enabled = enabled;
}

bool host_io_stdout_enabled(void)
{
    return g_stdout_enabled;
}

void host_io_set_console_stdin_enabled(bool enabled)
{
    g_console_stdin_enabled = enabled;
}

bool host_io_console_stdin_enabled(void)
{
    return g_console_stdin_enabled;
}

void host_io_emit_serial(int ch)
{
    if (g_serial_sink) {
        g_serial_sink(ch, g_serial_sink_data);
    }
}

void host_io_reset_stdin_state(void)
{
    g_stdin_eof = false;
}

size_t host_io_read_stdin_nonblocking(unsigned char *buf, size_t cap)
{
#ifdef __EMSCRIPTEN__
    (void)buf;
    (void)cap;
    return 0;
#elif defined(_WIN32)
    size_t len = 0;

    if (!buf || cap == 0 || g_stdin_eof)
        return 0;

    while (len < cap && _kbhit()) {
        int ch = _getch();
        if (ch < 0) {
            g_stdin_eof = true;
            break;
        }
        buf[len++] = (unsigned char)ch;
    }
    return len;
#else
    fd_set rfds;
    struct timeval tv;
    int ready;
    ssize_t len;

    if (!buf || cap == 0 || g_stdin_eof)
        return 0;

    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    do {
        ready = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
    } while (ready < 0 && errno == EINTR);

    if (ready <= 0)
        return 0;

    do {
        len = read(STDIN_FILENO, buf, cap);
    } while (len < 0 && errno == EINTR);

    if (len <= 0) {
        g_stdin_eof = true;
        return 0;
    }

    return (size_t)len;
#endif
}
