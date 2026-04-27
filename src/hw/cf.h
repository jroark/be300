#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CF_SECTOR_SIZE 512u
#define CF_WINDOW_SIZE 0x1000u

struct machine;
struct net;

#include "net.h"

typedef enum {
    CF_CARD_NONE = 0,
    CF_CARD_ATA,
    CF_CARD_NE2000,
} cf_card_kind_t;

#define CF_NE2000_MEM_SIZE 0x8000u

typedef struct {
    uint8_t *image;
    size_t   image_size;
    char    *image_path;
    cf_card_kind_t card_kind;
    bool     attached;
    bool     dirty;
    bool     irq_pending;
    bool     state_change_pending;
    bool     boot_visible;
    bool     boot_status_floating;
    bool     cis_funcid_seen;
    bool     insert_event_pending;
    bool     card_windows_enabled;

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

    struct {
        struct nic_data nic;
        struct net *net;
        uint8_t mem[CF_NE2000_MEM_SIZE];
        uint8_t par[6];
        uint8_t mar[8];
        uint8_t cr;
        uint8_t pstart;
        uint8_t pstop;
        uint8_t bnry;
        uint8_t tpsr;
        uint8_t tbcr0;
        uint8_t tbcr1;
        uint8_t isr;
        uint8_t rsar0;
        uint8_t rsar1;
        uint8_t rbcr0;
        uint8_t rbcr1;
        uint8_t rcr;
        uint8_t tcr;
        uint8_t dcr;
        uint8_t imr;
        uint8_t curr;
        uint8_t rsr;
        uint8_t tsr;
        uint8_t reset_latch;
        uint8_t remote_cmd;
        uint16_t remote_addr;
        uint16_t remote_count;
    } ne2000;
} cf_state_t;

void     cf_init(cf_state_t *s);
void     cf_destroy(cf_state_t *s);
int      cf_load_image(cf_state_t *s, const char *path);
int      cf_attach_ne2000(cf_state_t *s, struct machine *machine,
                          struct net *net, const uint8_t *mac);
int      cf_save_image(cf_state_t *s);
bool     cf_present(const cf_state_t *s);
bool     cf_is_ne2000(const cf_state_t *s);
void     cf_set_boot_visibility(cf_state_t *s, bool visible);
bool     cf_boot_handles_rom_offset(const cf_state_t *s, uint32_t offset);
void     cf_note_pcmcia_window_enable(cf_state_t *s);
void     cf_clear_irq(cf_state_t *s);
void     cf_consume_state_change(cf_state_t *s);
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
void     cf_ne2000_tick(cf_state_t *s);
bool     cf_pcmcia_windows_enabled(const cf_state_t *s);
uint8_t  cf_cis_read_byte(cf_state_t *s, uint32_t cis_idx);
uint64_t cf_cis_read(cf_state_t *s, uint32_t offset, unsigned size);
