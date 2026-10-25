/* Z-Com XI-825 PRISM station controller; see docs/XI825.md for evidence. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XI825_BUFFER_SIZE 4096u
#define XI825_BUFFERS 24u
#define XI825_RECORDS 64u

typedef struct {
    uint16_t id, length;
    bool used;
    uint8_t data[XI825_BUFFER_SIZE];
} xi825_buffer_t;

typedef struct {
    uint16_t id, length;
    uint8_t data[128];
    uint8_t committed[128];
} xi825_record_t;

typedef struct {
    uint16_t regs[32];
    xi825_buffer_t buffers[XI825_BUFFERS];
    xi825_record_t records[XI825_RECORDS];
    uint8_t mac[6];
    bool enabled, associated, link_pending;
    uint16_t next_fid;
    uint16_t info_queue[8];
    unsigned info_head, info_count;
    uint16_t rx_queue[8];
    unsigned rx_head, rx_count;
    void *opaque;
    void (*transmit)(void *, const uint8_t *, size_t);
} xi825_t;

void xi825_init(xi825_t *s, const uint8_t mac[6]);
void xi825_reset(xi825_t *s);
void xi825_set_cis(xi825_t *s, const uint8_t *cis, size_t length);
uint64_t xi825_read(xi825_t *s, unsigned offset, unsigned size);
void xi825_write(xi825_t *s, unsigned offset, unsigned size, uint64_t value);
bool xi825_irq_pending(const xi825_t *s);
bool xi825_receive(xi825_t *s, const uint8_t *ethernet, size_t length);
void xi825_tick(xi825_t *s);
