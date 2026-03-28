#include <stddef.h>

#include "host_io.h"

static host_io_serial_sink_t g_serial_sink = NULL;
static void *g_serial_sink_data = NULL;
static bool g_stdout_enabled = true;

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

void host_io_emit_serial(int ch)
{
    if (g_serial_sink) {
        g_serial_sink(ch, g_serial_sink_data);
    }
}
