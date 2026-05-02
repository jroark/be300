#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PCC_RING_CAP 4096u

typedef struct {
    uint8_t buf[PCC_RING_CAP];
    size_t head;
    size_t tail;
    size_t count;
} pcc_ring_t;

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

static inline bool pcc_ring_is_empty(const pcc_ring_t *r)
{
    return r->count == 0;
}

static inline bool pcc_ring_is_full(const pcc_ring_t *r)
{
    return r->count == PCC_RING_CAP;
}

static inline void pcc_ring_push(pcc_ring_t *r, uint8_t byte)
{
    if (r->count == PCC_RING_CAP) {
        r->tail = (r->tail + 1u) % PCC_RING_CAP;
        r->count--;
    }
    r->buf[r->head] = byte;
    r->head = (r->head + 1u) % PCC_RING_CAP;
    r->count++;
}

static inline int pcc_ring_pop(pcc_ring_t *r)
{
    uint8_t byte;
    if (r->count == 0)
        return -1;
    byte = r->buf[r->tail];
    r->tail = (r->tail + 1u) % PCC_RING_CAP;
    r->count--;
    return (int)byte;
}
