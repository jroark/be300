#pragma once

#include <stddef.h>
#include <stdbool.h>

typedef void (*host_io_serial_sink_t)(int ch, void *user_data);

void host_io_set_serial_sink(host_io_serial_sink_t sink, void *user_data);
void host_io_set_stdout_enabled(bool enabled);
bool host_io_stdout_enabled(void);
void host_io_set_console_stdin_enabled(bool enabled);
bool host_io_console_stdin_enabled(void);
void host_io_emit_serial(int ch);
void host_io_reset_stdin_state(void);
size_t host_io_read_stdin_nonblocking(unsigned char *buf, size_t cap);
