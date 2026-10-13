#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Default capacity for host-to-guest queues. */
#define PCC_RING_CAP 4096u

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t head;
    size_t tail;
    size_t count;
} pcc_ring_t;

static inline bool pcc_ring_init(pcc_ring_t *r, size_t cap)
{
    r->buf = (uint8_t *)malloc(cap);
    r->cap = r->buf ? cap : 0;
    r->head = 0;
    r->tail = 0;
    r->count = 0;
    return r->buf != NULL;
}

static inline void pcc_ring_free(pcc_ring_t *r)
{
    free(r->buf);
    r->buf = NULL;
    r->cap = 0;
    r->head = 0;
    r->tail = 0;
    r->count = 0;
}

static inline void pcc_ring_clear(pcc_ring_t *r)
{
    r->head = 0;
    r->tail = 0;
    r->count = 0;
}

static inline size_t pcc_ring_count(const pcc_ring_t *r)
{
    return r->count;
}

static inline size_t pcc_ring_space(const pcc_ring_t *r)
{
    return r->cap - r->count;
}

static inline bool pcc_ring_is_empty(const pcc_ring_t *r)
{
    return r->count == 0;
}

static inline bool pcc_ring_is_full(const pcc_ring_t *r)
{
    return r->count == r->cap;
}

static inline void pcc_ring_push(pcc_ring_t *r, uint8_t byte)
{
    if (r->cap == 0)
        return;
    if (r->count == r->cap) {
        r->tail = (r->tail + 1u) % r->cap;
        r->count--;
    }
    r->buf[r->head] = byte;
    r->head = (r->head + 1u) % r->cap;
    r->count++;
}

static inline int pcc_ring_pop(pcc_ring_t *r)
{
    uint8_t byte;
    if (r->count == 0)
        return -1;
    byte = r->buf[r->tail];
    r->tail = (r->tail + 1u) % r->cap;
    r->count--;
    return (int)byte;
}
