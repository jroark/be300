#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool be300_ppsh_transport_ready(void);
size_t be300_ppsh_queue_host_input(const uint8_t *buf, size_t len);
