#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CF_SECTOR_SIZE 512u
#define CF_WINDOW_SIZE 0x1000u

typedef struct {
    uint8_t *image;
    size_t   image_size;
    char    *image_path;
    bool     attached;
    bool     dirty;
    bool     irq_pending;
    bool     boot_visible;
    bool     boot_status_floating;

    uint8_t  companion_page[0x1000];
    uint8_t  cis[256];

    uint8_t *data_buffer;
    size_t   data_capacity;
    size_t   data_length;
    size_t   data_pos;
    bool     pending_write;
    uint32_t transfer_lba;
    uint16_t transfer_remaining;

    uint8_t  error_reg;
    uint8_t  feature_reg;
    uint8_t  sector_count;
    uint8_t  sector_number;
    uint8_t  cylinder_low;
    uint8_t  cylinder_high;
    uint8_t  drive_head;
    uint8_t  command_reg;
    uint8_t  status_reg;
    uint8_t  device_control;
} cf_state_t;

void     cf_init(cf_state_t *s);
void     cf_destroy(cf_state_t *s);
int      cf_load_image(cf_state_t *s, const char *path);
int      cf_save_image(cf_state_t *s);
bool     cf_present(const cf_state_t *s);
void     cf_set_boot_visibility(cf_state_t *s, bool visible);
bool     cf_boot_handles_rom_offset(const cf_state_t *s, uint32_t offset);
void     cf_clear_irq(cf_state_t *s);
uint32_t cf_giu_source_bits(const cf_state_t *s);
uint16_t cf_card_state_bits(const cf_state_t *s);
uint64_t cf_companion_read(cf_state_t *s, uint32_t offset, unsigned size);
void     cf_companion_write(cf_state_t *s, uint32_t offset, unsigned size,
                            uint64_t value);
uint64_t cf_boot_read(cf_state_t *s, uint32_t offset, unsigned size);
void     cf_boot_write(cf_state_t *s, uint32_t offset, unsigned size,
                       uint64_t value);
uint64_t cf_window_read(cf_state_t *s, uint32_t offset, unsigned size);
void     cf_window_write(cf_state_t *s, uint32_t offset, unsigned size,
                         uint64_t value);
