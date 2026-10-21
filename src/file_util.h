#pragma once

#include <stddef.h>

/*
 * Replace path with len bytes of data: write path.tmp, flush it to disk,
 * then rename it over path, so a crash or a failed write leaves the old
 * file intact. Returns 0, or -1 (errno set) with path unchanged.
 */
int be300_write_file_atomic(const char *path, const void *data, size_t len);
