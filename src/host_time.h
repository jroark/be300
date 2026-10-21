/*
 *  src/host_time.h — the host monotonic clock in nanoseconds, for the
 *  front-end transports and the few devices paced by host time. Returns 0
 *  if the clock cannot be read.
 */
#ifndef BE300_HOST_TIME_H
#define BE300_HOST_TIME_H

#include <stdint.h>
#include <time.h>

#include "win32_compat.h"

static inline uint64_t be300_host_monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

#endif /* BE300_HOST_TIME_H */
