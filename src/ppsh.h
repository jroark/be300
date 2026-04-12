#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool be300_ppsh_transport_ready(void);
size_t be300_ppsh_queue_host_input(const uint8_t *buf, size_t len);

/*
 * Redirect guest-side PPSH shell output. When a sink is installed, PPSH
 * bytes are delivered to `sink(ctx, buf, len)` instead of being printed
 * to stdout. Pass sink=NULL to revert to the stdout fallback.
 */
typedef void (*be300_ppsh_output_sink_t)(void *ctx,
                                         const uint8_t *buf,
                                         size_t len);
void be300_ppsh_set_output_sink(be300_ppsh_output_sink_t sink, void *ctx);
