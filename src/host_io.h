#pragma once

#include <stdbool.h>

typedef void (*host_io_serial_sink_t)(int ch, void *user_data);

void host_io_set_serial_sink(host_io_serial_sink_t sink, void *user_data);
void host_io_set_stdout_enabled(bool enabled);
bool host_io_stdout_enabled(void);
void host_io_emit_serial(int ch);
