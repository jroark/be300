#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cf.h"

#define CF_COMPANION_PRESENT UINT32_C(0x00000004)
#define CF_COMPANION_REMOVED UINT32_C(0x0000000C)
#define CF_SOCKET_NO_MEDIA UINT32_C(0x00000040)
#define CF_INSERT_EVENT UINT8_C(0x08)

#define CF_ST_ERR  0x01u
#define CF_ST_DRQ  0x08u
#define CF_ST_DSC  0x10u
#define CF_ST_DRDY 0x40u
#define CF_ST_BSY  0x80u

#define CF_DH_LBA  0x40u

#define CF_CISTPL_FUNCID UINT8_C(0x21)
#define CF_FUNCID_FIXED_DISK UINT8_C(0x04)

#define CF_CMD_RECAL        0x10u
#define CF_CMD_READ_SECTORS 0x20u
#define CF_CMD_WRITE_SECTORS 0x30u
#define CF_CMD_READ_VERIFY  0x40u
#define CF_CMD_DIAGNOSTIC   0x90u
#define CF_CMD_SET_FEATURES 0xEFu
#define CF_CMD_IDENTIFY     0xECu

#define CF_BOOT_TF_BASE 0xC170u
#define CF_BOOT_TF_END  0xC178u
#define CF_BOOT_ALT_REG 0xC376u

static const uint16_t cf_no_card_owned_offsets[] = {
    0x0000, 0x0008, 0x0044, 0x004C,
};

static bool cf_is_no_card_owned_offset(uint32_t offset)
{
    for (size_t i = 0;
         i < sizeof(cf_no_card_owned_offsets) / sizeof(cf_no_card_owned_offsets[0]);
         i++) {
        if (offset == cf_no_card_owned_offsets[i])
            return true;
    }
    return false;
}

static void cf_put_companion_u32(cf_state_t *s, uint16_t off, uint32_t val)
{
    memcpy(&s->companion_page[off], &val, sizeof(val));
}

static void cf_refresh_socket_status(cf_state_t *s)
{
    uint32_t val;

    if (!s)
        return;

    /*
     * docs/hardware/hardware.txt identifies 0xaa001000 as CF status:
     * 0x04 = card inserted, 0x0c = card removed.  The hardware dumps were
     * captured with a CF memory card inserted, so do not mirror the broad
     * inserted-card table for no-card boots.
     */
    val = s->attached ? CF_COMPANION_PRESENT : CF_COMPANION_REMOVED;
    cf_put_companion_u32(s, 0x0000, val);
    cf_put_companion_u32(s, 0x0008, val);

    /* Card-state summary used by pcmcia/card_ex probing. */
    cf_put_companion_u32(s, 0x0040, s->attached ? 0x00000001u : 0x00000000u);
    if (s->attached) {
        /*
         * Do not refresh 0x0044/0x004C for attached media.  pcmcia.dll
         * treats those words as live state latches and writes values such
         * as 0x23 and 0x08 while running the socket state machine.  The
         * inserted-card hardware dump does not list non-zero values for
         * 0x0A001044/0x0A00104C, so preserving the guest-written latch
         * state is closer to hardware than synthesising fixed status bits.
         */
    } else {
        /*
         * pcmcia.dll uses 0xaa001044 both as a socket/media probe and as
         * part of CardGetStatus.  Real hardware with the PCMCIA adapter
         * present and no card inserted reports "Card type: None" and
         * "Card unit: Set"; bit 6 clear here preserves the unit-present
         * result while the 0x0B000100 status block reports no media.
         *
         * The socket-refresh path at 0x0198b46c also tests bit 6 in the
         * OR of 0xaa001044/0xaa00104c/0xaa001b10 before it scans CIS
         * attribute memory.  Keep the companion bit at 0xaa00104c set so
         * no-card boots do not discover unknown cards in the pulled-up
         * window.
         *
         * TODO(2026-04-25): replace this with named VRC4173/BE-300 PCMCIA
         * bridge semantics once the companion PCMCIA register map is known.
         */
        cf_put_companion_u32(s, 0x0044, 0x00000000u);
        cf_put_companion_u32(s, 0x004C, CF_SOCKET_NO_MEDIA);
    }
}

static void cf_seed_cis(cf_state_t *s)
{
    /*
     * Minimal CIS for a CompactFlash ATA storage card. The order and
     * tuple set are what WinCE 3.0 pcmcia.dll matches against to
     * dispatch to atadisk.dll instead of opening the "Unidentified
     * PCCard Adapter" prompt:
     *
     *   CISTPL_DEVICE       — generic device descriptor (FUNCSPEC, 250 ns).
     *   CISTPL_VERS_1       — vendor / product strings.
     *   CISTPL_MANFID       — manufacturer + product code (used by the
     *                          HKLM\Drivers\PCMCIA\Detect\NN registry
     *                          match path, falls through to FUNCID class
     *                          match if no entry matches).
     *   CISTPL_FUNCID       — function code 0x04 (FIXED_DISK), SYSINIT 0x01.
     *                          PC Card Standard Vol 4 (Metaformat) §7.1.
     *   CISTPL_FUNCE        — fixed-disk interface = 0x01 (IDE).
     *                          PC Card Standard Vol 4 §7.2.2.
     *   CISTPL_CONFIG       — configuration register base + last index.
     *   CISTPL_CFTABLE_ENTRY — single 8-bit I/O entry.
     *   CISTPL_NO_LINK      — no MFC link table.
     *   CISTPL_END          — terminate tuple chain (PC Card §3.2.10).
     */
    static const uint8_t cis[] = {
        0x01, 0x03, 0xDC, 0x00, 0xFF,
        0x15, 0x1A,
        0x04, 0x01, ' ', 0x00,
        'N', 'i', 'n', 'j', 'a', 'A', 'T', 'A', '-', 0x00,
        'V', '1', '.', '0', 0x00,
        'A', 'P', '0', '0', ' ', 0x00, 0xFF,
        0x20, 0x04, 0x23, 0x01, 0x02, 0x00,
        0x21, 0x02, 0x04, 0x01,
        0x22, 0x02, 0x01, 0x01,
        0x1A, 0x05, 0x01, 0x23, 0x00, 0x02, 0x03,
        0x1B, 0x15,
        0xE1, 0x01, 0x3D, 0x11, 0x55, 0x1E, 0xFC, 0x23,
        0xF0, 0x61, 0x70, 0x01, 0x07, 0x76, 0x03, 0x01,
        0x30, 0x68, 0xD0, 0x10, 0x00,
        0x14, 0x00,
        0xFF, 0x00,
    };

    memset(s->cis, 0xFF, sizeof(s->cis));
    memcpy(s->cis, cis, sizeof(cis));
}

static void cf_seed_companion_page(cf_state_t *s)
{
    struct seed_entry { uint16_t off; uint32_t val; };
    static const struct seed_entry regs[] = {
        { 0x0004, 0x00000000u },
        { 0x000C, 0x00000000u },
        { 0x0010, 0x0000000Du },
        { 0x0050, 0x00000000u },
        { 0x0054, 0x00000043u },
        { 0x0058, 0x00000000u },
        { 0x005C, 0x00000040u },
        { 0x0060, 0x00000040u },
        { 0x00A0, 0x00000000u },
        { 0x00A4, 0x00000000u },
        { 0x00E0, 0x00000000u },
        { 0x00E4, 0x00000000u },
    };
    static const struct seed_entry inserted_regs[] = {
        /*
         * Inserted-CF steady-state companion words from the real-hardware
         * VRC4173 dump (docs/hardware/hw_dump_vrc4173.txt:170-176).  These
         * are socket/status fields, not ATA taskfile data; keeping them
         * hardware-seeded lets the guest classify --cf as a storage card
         * without inventing a PCMCIA jacket accessory.
         */
        { 0x0B10, 0x00000048u },
        { 0x0B14, 0x00000001u },
        { 0x0B20, 0x00000001u },
        { 0x0B2C, 0x00000001u },
        { 0x0B34, 0x000000FFu },
        { 0x0B58, 0x0000000Fu },
        { 0x0B64, 0x00000001u },
        { 0x0B74, 0x00000004u },
        { 0x0B90, 0x00000008u },
    };
    size_t i;

    memset(s->companion_page, 0, sizeof(s->companion_page));
    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
        memcpy(&s->companion_page[regs[i].off], &regs[i].val, sizeof(uint32_t));
    if (s->attached) {
        for (i = 0; i < sizeof(inserted_regs) / sizeof(inserted_regs[0]); i++)
            memcpy(&s->companion_page[inserted_regs[i].off],
                &inserted_regs[i].val, sizeof(uint32_t));
    }
    cf_refresh_socket_status(s);
}

static void cf_reset_taskfile(cf_state_t *s)
{
    s->error_reg = 0;
    s->feature_reg = 0;
    s->sector_count = 1;
    s->sector_number = 1;
    s->cylinder_low = 0;
    s->cylinder_high = 0;
    s->drive_head = 0xE0u;
    s->command_reg = 0;
    s->device_control = 0;
    s->status_reg = s->attached ? (CF_ST_DRDY | CF_ST_DSC) : 0;
    s->data_length = 0;
    s->data_pos = 0;
    s->pending_write = false;
    s->transfer_lba = 0;
    s->transfer_remaining = 0;
}

static void cf_arm_boot_status(cf_state_t *s)
{
    if (!s)
        return;

    s->boot_status_floating = false;
}

void cf_init(cf_state_t *s)
{
    if (!s)
        return;

    memset(s, 0, sizeof(*s));
    cf_seed_cis(s);
    cf_seed_companion_page(s);
    cf_reset_taskfile(s);
    cf_arm_boot_status(s);
}

void cf_destroy(cf_state_t *s)
{
    if (!s)
        return;

    free(s->data_buffer);
    s->data_buffer = NULL;
    free(s->image);
    s->image = NULL;
    free(s->image_path);
    s->image_path = NULL;
    s->image_size = 0;
    s->attached = false;
}

static int cf_reserve_buffer(cf_state_t *s, size_t size)
{
    uint8_t *buf;

    if (size <= s->data_capacity)
        return 0;

    buf = realloc(s->data_buffer, size);
    if (!buf)
        return -1;

    s->data_buffer = buf;
    s->data_capacity = size;
    return 0;
}

int cf_load_image(cf_state_t *s, const char *path)
{
    FILE *f;
    long fsize;
    uint8_t *data;

    if (!s || !path)
        return -1;

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[CF] Cannot open image: %s\n", path);
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    fsize = ftell(f);
    rewind(f);

    if (fsize <= 0) {
        fprintf(stderr, "[CF] Image is empty\n");
        fclose(f);
        return -1;
    }
    if ((fsize % CF_SECTOR_SIZE) != 0) {
        fprintf(stderr, "[CF] Image size must be a multiple of 512 bytes\n");
        fclose(f);
        return -1;
    }

    data = malloc((size_t)fsize);
    if (!data) {
        fclose(f);
        return -1;
    }
    if ((long)fread(data, 1, (size_t)fsize, f) != fsize) {
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);

    free(s->image);
    s->image = data;
    s->image_size = (size_t)fsize;
    s->attached = true;
    s->dirty = false;
    s->irq_pending = true;
    s->state_change_pending = true;
    s->boot_visible = false;
    s->cis_funcid_seen = false;
    s->insert_event_pending = true;
    s->card_windows_enabled = false;

    free(s->image_path);
    s->image_path = strdup(path);
    if (!s->image_path)
        return -1;

    cf_seed_companion_page(s);
    cf_reset_taskfile(s);
    cf_arm_boot_status(s);
    return 0;
}

int cf_save_image(cf_state_t *s)
{
    FILE *f;

    if (!s || !s->attached || !s->dirty || !s->image_path || !s->image)
        return 0;

    f = fopen(s->image_path, "wb");
    if (!f) {
        fprintf(stderr, "[CF] Failed to save image: %s\n", s->image_path);
        return -1;
    }

    if (fwrite(s->image, 1, s->image_size, f) != s->image_size) {
        fclose(f);
        fprintf(stderr, "[CF] Short write while saving image\n");
        return -1;
    }
    fclose(f);

    s->dirty = false;
    return 0;
}

bool cf_present(const cf_state_t *s)
{
    return s && s->attached;
}

void cf_set_boot_visibility(cf_state_t *s, bool visible)
{
    if (!s)
        return;

    s->boot_visible = visible && s->attached;
    cf_reset_taskfile(s);
    cf_arm_boot_status(s);
}

bool cf_boot_handles_rom_offset(const cf_state_t *s, uint32_t offset)
{
    return s && s->attached &&
        ((offset >= CF_BOOT_TF_BASE && offset < CF_BOOT_TF_END) ||
         offset == CF_BOOT_ALT_REG);
}

void cf_clear_irq(cf_state_t *s)
{
    if (s)
        s->irq_pending = false;
}

void cf_consume_state_change(cf_state_t *s)
{
    if (s)
        s->state_change_pending = false;
}

uint32_t cf_giu_source_bits(const cf_state_t *s)
{
    /*
     * GIRQ0-0 source bit (PA 0xAA000004 bit 0).  hardware.txt:32
     * documents this bit as the cascaded PCMCIA interrupt.  Keep the
     * source asserted while either the legacy boot-restore path has set
     * irq_pending or a card-state change is pending delivery.
     *
     * state_change_pending survives the early-NK clears that hit
     * cf_clear_irq via writes to PA 0x0A000010/0x0A000014 (cc590ba5
     * "Implement ROM restore CF path"), so the SYSINTR_PCMCIA_STATE
     * dispatch path (NK FUN_8007aff8 / FUN_8007b134 / FUN_8007b1f8 in
     * the verified WinCE 3.0 NK dump) still sees bit 0 once the kernel
     * unmasks GIU GIRQ0-0 after init.  Clearing happens via the
     * existing GIRQ0-0 ack path at PA 0xAA00A03C (be300_devices.c
     * dev_be300_nand_access offset 0xA03C-0xA040) and via the
     * cf_companion_write IRQ-clear at 0x40/0x50.
     */
    return (s && s->attached &&
            (s->irq_pending || s->state_change_pending)) ? 0x00000001u : 0u;
}

uint16_t cf_card_state_bits(const cf_state_t *s)
{
    if (!s || !s->attached)
        return 0x0000u;
    /*
     * 0xAA00A03C cause bits read by NK GIRQ0-0 dispatcher
     * (hardware.txt:91-101): bits 0x22 -> SYSINTR_PCMCIA_STATE,
     * bit 0x01 -> SYSINTR_PCMCIA_LEVEL.  Report state-change cause while
     * a state_change is pending delivery; otherwise level-only.
     */
    return (s->irq_pending || s->state_change_pending) ? 0x0023u : 0x0001u;
}

static uint32_t cf_current_lba(const cf_state_t *s)
{
    return (uint32_t)s->sector_number
        | ((uint32_t)s->cylinder_low << 8)
        | ((uint32_t)s->cylinder_high << 16)
        | ((uint32_t)(s->drive_head & 0x0Fu) << 24);
}

static void cf_set_current_lba(cf_state_t *s, uint32_t lba)
{
    s->sector_number = (uint8_t)(lba & 0xFFu);
    s->cylinder_low = (uint8_t)((lba >> 8) & 0xFFu);
    s->cylinder_high = (uint8_t)((lba >> 16) & 0xFFu);
    s->drive_head = (uint8_t)((s->drive_head & 0xF0u) |
        ((lba >> 24) & 0x0Fu));
}

static void cf_set_ready(cf_state_t *s)
{
    s->status_reg = s->attached ? (CF_ST_DRDY | CF_ST_DSC) : 0;
}

static void cf_set_error(cf_state_t *s, uint8_t err)
{
    s->error_reg = err;
    s->status_reg = CF_ST_ERR | (s->attached ? (CF_ST_DRDY | CF_ST_DSC) : 0);
    s->data_length = 0;
    s->data_pos = 0;
    s->pending_write = false;
    s->transfer_remaining = 0;
}

static void cf_put_word_le(uint8_t *buf, size_t word_index, uint16_t value)
{
    buf[word_index * 2 + 0] = (uint8_t)(value & 0xFFu);
    buf[word_index * 2 + 1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void cf_put_ata_string(uint8_t *buf, size_t word_index, size_t words,
                              const char *text)
{
    size_t i;
    size_t len = strlen(text);
    size_t max = words * 2;

    for (i = 0; i < max; i += 2) {
        uint8_t a = (i + 0 < len) ? (uint8_t)text[i + 0] : (uint8_t)' ';
        uint8_t b = (i + 1 < len) ? (uint8_t)text[i + 1] : (uint8_t)' ';
        buf[word_index * 2 + i + 0] = b;
        buf[word_index * 2 + i + 1] = a;
    }
}

static int cf_prepare_identify(cf_state_t *s)
{
    uint32_t total_sectors;

    if (cf_reserve_buffer(s, CF_SECTOR_SIZE) != 0)
        return -1;

    memset(s->data_buffer, 0, CF_SECTOR_SIZE);
    total_sectors = (uint32_t)(s->image_size / CF_SECTOR_SIZE);

    cf_put_word_le(s->data_buffer, 0, 0x848Au);
    cf_put_word_le(s->data_buffer, 1, 0x3FFFu);
    cf_put_word_le(s->data_buffer, 3, 0x0010u);
    cf_put_word_le(s->data_buffer, 6, 0x003Fu);
    cf_put_ata_string(s->data_buffer, 10, 10, "BE300-CF0001");
    cf_put_ata_string(s->data_buffer, 23, 4, "1.0");
    cf_put_ata_string(s->data_buffer, 27, 20, "BE-300 RECOVERY CF");
    cf_put_word_le(s->data_buffer, 47, 0x8001u);
    cf_put_word_le(s->data_buffer, 49, 0x0200u);
    cf_put_word_le(s->data_buffer, 53, 0x0007u);
    cf_put_word_le(s->data_buffer, 54, 0x3FFFu);
    cf_put_word_le(s->data_buffer, 55, 0x0010u);
    cf_put_word_le(s->data_buffer, 56, 0x003Fu);
    cf_put_word_le(s->data_buffer, 57, (uint16_t)(total_sectors & 0xFFFFu));
    cf_put_word_le(s->data_buffer, 58, (uint16_t)(total_sectors >> 16));
    cf_put_word_le(s->data_buffer, 60, (uint16_t)(total_sectors & 0xFFFFu));
    cf_put_word_le(s->data_buffer, 61, (uint16_t)(total_sectors >> 16));

    s->data_length = CF_SECTOR_SIZE;
    s->data_pos = 0;
    s->pending_write = false;
    s->transfer_remaining = 0;
    s->status_reg = CF_ST_DRDY | CF_ST_DSC | CF_ST_DRQ;
    return 0;
}

static void cf_finish_transfer(cf_state_t *s)
{
    s->data_length = 0;
    s->data_pos = 0;
    s->pending_write = false;
    s->transfer_remaining = 0;
    cf_set_ready(s);
}

static void cf_update_taskfile_after_sector(cf_state_t *s, uint32_t next_lba,
                                            uint16_t remaining)
{
    cf_set_current_lba(s, next_lba);
    s->sector_count = remaining == 256u ? 0u : (uint8_t)(remaining & 0xFFu);
}

static int cf_load_read_sector(cf_state_t *s)
{
    size_t off = (size_t)s->transfer_lba * CF_SECTOR_SIZE;

    if (cf_reserve_buffer(s, CF_SECTOR_SIZE) != 0)
        return -1;
    if (off + CF_SECTOR_SIZE > s->image_size) {
        cf_set_error(s, 0x10u);
        return -1;
    }

    memcpy(s->data_buffer, s->image + off, CF_SECTOR_SIZE);
    s->data_length = CF_SECTOR_SIZE;
    s->data_pos = 0;
    s->pending_write = false;
    s->status_reg = CF_ST_DRDY | CF_ST_DSC | CF_ST_DRQ;
    return 0;
}

static int cf_prepare_write_sector(cf_state_t *s)
{
    if (cf_reserve_buffer(s, CF_SECTOR_SIZE) != 0)
        return -1;

    memset(s->data_buffer, 0, CF_SECTOR_SIZE);
    s->data_length = CF_SECTOR_SIZE;
    s->data_pos = 0;
    s->pending_write = true;
    s->status_reg = CF_ST_DRDY | CF_ST_DSC | CF_ST_DRQ;
    return 0;
}

static int cf_prepare_read(cf_state_t *s)
{
    uint32_t lba;
    uint32_t sectors;

    if (!(s->drive_head & CF_DH_LBA)) {
        cf_set_error(s, 0x04u);
        return -1;
    }

    lba = cf_current_lba(s);
    sectors = s->sector_count ? s->sector_count : 256u;

    if (((uint64_t)lba + sectors) * CF_SECTOR_SIZE > s->image_size) {
        cf_set_error(s, 0x10u);
        return -1;
    }

    s->transfer_lba = lba;
    s->transfer_remaining = (uint16_t)sectors;
    return cf_load_read_sector(s);
}

static int cf_begin_write(cf_state_t *s)
{
    uint32_t lba;
    uint32_t sectors;

    if (!(s->drive_head & CF_DH_LBA)) {
        cf_set_error(s, 0x04u);
        return -1;
    }

    lba = cf_current_lba(s);
    sectors = s->sector_count ? s->sector_count : 256u;

    if (((uint64_t)lba + sectors) * CF_SECTOR_SIZE > s->image_size) {
        cf_set_error(s, 0x10u);
        return -1;
    }

    s->transfer_lba = lba;
    s->transfer_remaining = (uint16_t)sectors;
    return cf_prepare_write_sector(s);
}

static void cf_execute_command(cf_state_t *s, uint8_t cmd)
{
    s->command_reg = cmd;
    s->error_reg = 0;

    if (!s->attached) {
        cf_set_error(s, 0x04u);
        return;
    }

    switch (cmd) {
    case CF_CMD_IDENTIFY:
        if (cf_prepare_identify(s) != 0)
            cf_set_error(s, 0x04u);
        break;
    case CF_CMD_READ_SECTORS:
        if (cf_prepare_read(s) != 0)
            return;
        break;
    case CF_CMD_WRITE_SECTORS:
        if (cf_begin_write(s) != 0)
            return;
        break;
    case CF_CMD_RECAL:
    case CF_CMD_READ_VERIFY:
    case CF_CMD_DIAGNOSTIC:
        cf_set_ready(s);
        break;
    case CF_CMD_SET_FEATURES:
        if (s->feature_reg == 0x03u || s->feature_reg == 0x01u) {
            cf_set_ready(s);
        } else {
            cf_set_error(s, 0x04u);
        }
        break;
    default:
        cf_set_error(s, 0x04u);
        break;
    }
}

static uint64_t cf_data_read(cf_state_t *s, unsigned size)
{
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        uint8_t byte = 0xFFu;

        if (s->data_pos < s->data_length)
            byte = s->data_buffer[s->data_pos++];
        val |= (uint64_t)byte << (8u * i);
    }

    if (s->data_pos >= s->data_length) {
        uint32_t next_lba = s->transfer_lba + 1u;
        uint16_t remaining = s->transfer_remaining;

        if (remaining > 0)
            remaining--;
        cf_update_taskfile_after_sector(s, next_lba, remaining);
        s->transfer_lba = next_lba;
        s->transfer_remaining = remaining;

        if (remaining > 0) {
            if (cf_load_read_sector(s) != 0)
                cf_set_error(s, 0x10u);
        } else {
            cf_finish_transfer(s);
        }
    }

    return val;
}

static void cf_commit_write(cf_state_t *s)
{
    uint32_t next_lba = s->transfer_lba + 1u;
    uint16_t remaining = s->transfer_remaining;

    memcpy(s->image + (size_t)s->transfer_lba * CF_SECTOR_SIZE, s->data_buffer,
        CF_SECTOR_SIZE);
    s->dirty = true;

    if (remaining > 0)
        remaining--;
    cf_update_taskfile_after_sector(s, next_lba, remaining);
    s->transfer_lba = next_lba;
    s->transfer_remaining = remaining;

    if (remaining > 0) {
        if (cf_prepare_write_sector(s) != 0)
            cf_set_error(s, 0x10u);
    } else {
        cf_finish_transfer(s);
    }
}

static void cf_data_write(cf_state_t *s, unsigned size, uint64_t value)
{
    unsigned i;

    if (!s->pending_write || s->data_length == 0)
        return;

    for (i = 0; i < size && s->data_pos < s->data_length; i++)
        s->data_buffer[s->data_pos++] = (uint8_t)((value >> (8u * i)) & 0xFFu);

    if (s->data_pos >= s->data_length)
        cf_commit_write(s);
}

static int cf_decode_taskfile_offset(uint32_t offset, bool *is_alt)
{
    uint32_t page_off = offset & 0x3FFu;

    *is_alt = false;
    if (page_off >= 0x1F0u && page_off <= 0x1F7u)
        return (int)(page_off - 0x1F0u);
    if (page_off >= 0x170u && page_off <= 0x177u)
        return (int)(page_off - 0x170u);
    if (page_off >= 0x180u && page_off <= 0x187u)
        return (int)(page_off - 0x180u);
    if (page_off == 0x206u || page_off == 0x376u || page_off == 0x3F6u) {
        *is_alt = true;
        return 0;
    }
    return -1;
}

static int cf_decode_boot_offset(uint32_t offset, bool *is_alt)
{
    *is_alt = false;

    if (offset >= CF_BOOT_TF_BASE && offset < CF_BOOT_TF_END)
        return (int)(offset - CF_BOOT_TF_BASE);
    if (offset == CF_BOOT_ALT_REG) {
        *is_alt = true;
        return 0;
    }
    return -1;
}

static uint8_t cf_read_reg8(cf_state_t *s, int reg, bool is_alt, bool boot_path)
{
    if (boot_path) {
        if (!s->boot_visible)
            return is_alt || reg == 7 ? 0x00u : 0xFFu;
        if (s->boot_status_floating && (is_alt || reg == 7))
            return 0xFFu;
    }

    if (is_alt)
        return s->status_reg;

    switch (reg) {
    case 1: return s->error_reg;
    case 2: return s->sector_count;
    case 3: return s->sector_number;
    case 4: return s->cylinder_low;
    case 5: return s->cylinder_high;
    case 6: return s->drive_head;
    case 7: return s->status_reg;
    default: return 0xFFu;
    }
}

static void cf_write_reg8(cf_state_t *s, int reg, bool is_alt, bool boot_path,
                          uint8_t value)
{
    if (is_alt) {
        s->device_control = value;
        if (boot_path && s->boot_visible && value == 0) {
            cf_reset_taskfile(s);
            s->boot_status_floating = false;
            return;
        }
        if (value & 0x04u)
            cf_reset_taskfile(s);
        return;
    }

    if (boot_path && !s->boot_visible)
        return;

    switch (reg) {
    case 1:
        s->feature_reg = value;
        break;
    case 2:
        s->sector_count = value;
        break;
    case 3:
        s->sector_number = value;
        break;
    case 4:
        s->cylinder_low = value;
        break;
    case 5:
        s->cylinder_high = value;
        break;
    case 6:
        s->drive_head = value;
        break;
    case 7:
        cf_execute_command(s, value);
        break;
    default:
        break;
    }
}

static void cf_note_cis_read(cf_state_t *s, uint32_t cis_idx)
{
    if (!s || s->cis_funcid_seen || cis_idx + 2 >= sizeof(s->cis))
        return;

    if (s->cis[cis_idx] == CF_CISTPL_FUNCID &&
        s->cis[cis_idx + 1] >= 2 &&
        s->cis[cis_idx + 2] == CF_FUNCID_FIXED_DISK)
        s->cis_funcid_seen = true;
}

uint8_t cf_cis_read_byte(cf_state_t *s, uint32_t cis_idx)
{
    if (!s || cis_idx >= sizeof(s->cis))
        return 0xFFu;

    cf_note_cis_read(s, cis_idx);
    return s->cis[cis_idx];
}

uint64_t cf_cis_read(cf_state_t *s, uint32_t offset, unsigned size)
{
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        uint32_t off = offset + i;
        uint8_t byte = 0xFFu;

        if ((off & 1u) == 0u)
            byte = cf_cis_read_byte(s, off >> 1);
        val |= (uint64_t)byte << (8u * i);
    }

    return val;
}

bool cf_pcmcia_windows_enabled(const cf_state_t *s)
{
    return s && s->attached && s->card_windows_enabled;
}

uint64_t cf_companion_read(cf_state_t *s, uint32_t offset, unsigned size)
{
    uint64_t val = 0;
    unsigned i;
    bool consumed_insert_event = false;

    if (!s || size == 0)
        return 0;

    cf_refresh_socket_status(s);
    for (i = 0; i < size && (offset + i) < sizeof(s->companion_page); i++) {
        uint8_t byte = s->companion_page[offset + i];

        /*
         * The real inserted-CF dump has VRC4173 offset 0x1B10 = 0x48
         * (docs/hardware/hw_dump_vrc4173.txt, 0x0A001B10 row).  During
         * early pcmcia.dll socket init, bit 6 also gates whether the CIS is
         * scanned; expose only the card-state edge bit until the CF FUNCID
         * tuple has been read, then report the settled inserted-card value.
         */
        if (s->attached && offset + i == 0x0B10u)
            byte = (uint8_t)((byte & ~(uint8_t)0x48u) |
                (s->cis_funcid_seen ? 0x48u : 0x08u));

        /*
         * VRC4173 offset 0x1B50 is observed as zero in the real inserted-CF
         * steady-state dump (docs/hardware/hw_dump_vrc4173.txt, 0x0A001B50
         * row), but pcmcia.dll's SYSINTR_PCMCIA_STATE thread uses bit 3 as
         * the insertion edge that triggers the initial CIS scan.  Expose the
         * edge once after host attachment, then return to the dumped steady
         * value so later card-unit/battery queries do not see a PCMCIA jacket
         * accessory event.
         */
        if (s->attached && s->insert_event_pending &&
            offset + i == 0x0B50u) {
            byte |= CF_INSERT_EVENT;
            consumed_insert_event = true;
        }

        val |= (uint64_t)byte << (8u * i);
    }

    if (consumed_insert_event)
        s->insert_event_pending = false;

    return val;
}

void cf_companion_write(cf_state_t *s, uint32_t offset, unsigned size,
                        uint64_t value)
{
    unsigned i;

    if (!s || size == 0)
        return;


    for (i = 0; i < size && (offset + i) < sizeof(s->companion_page); i++) {
        uint32_t byte_off = offset + i;
        uint32_t word_off = byte_off & ~3u;

        if (!s->attached && cf_is_no_card_owned_offset(word_off))
            continue;
        if (s->attached && word_off >= 0x0B00u && word_off < 0x0BA0u)
            s->card_windows_enabled = true;
        if (word_off == 0x0B50u &&
            ((value >> (8u * i)) & CF_INSERT_EVENT) != 0)
            s->insert_event_pending = false;

        s->companion_page[byte_off] =
            (uint8_t)((value >> (8u * i)) & 0xFFu);
    }

    if (offset == 0x0040u || offset == 0x0050u)
        cf_clear_irq(s);
    cf_refresh_socket_status(s);
}

uint64_t cf_window_read(cf_state_t *s, uint32_t offset, unsigned size)
{
    bool is_alt = false;
    int reg;
    uint64_t val = 0;
    unsigned i;

    if (!s || size == 0)
        return 0;
    if (!s->attached)
        return size >= 4 ? UINT32_C(0xFFFFFFFF) : UINT64_C(0xFF);

    reg = cf_decode_taskfile_offset(offset, &is_alt);
    if (reg == 0 && !is_alt) {
        val = cf_data_read(s, size);
    } else if (reg >= 0) {
        for (i = 0; i < size; i++) {
            uint8_t byte = cf_read_reg8(s, reg, is_alt, false);
            val |= (uint64_t)byte << (8u * i);
        }
    } else {
        val = cf_cis_read(s, offset, size);
    }

    return val;
}

void cf_window_write(cf_state_t *s, uint32_t offset, unsigned size,
                     uint64_t value)
{
    bool is_alt = false;
    int reg;
    unsigned i;

    if (!s || size == 0 || !s->attached)
        return;

    reg = cf_decode_taskfile_offset(offset, &is_alt);
    if (reg == 0 && !is_alt) {
        cf_data_write(s, size, value);
    } else if (reg >= 0) {
        for (i = 0; i < size; i++)
            cf_write_reg8(s, reg, is_alt, false,
                (uint8_t)((value >> (8u * i)) & 0xFFu));
    }

}

uint64_t cf_boot_read(cf_state_t *s, uint32_t offset, unsigned size)
{
    bool is_alt = false;
    int reg;
    uint64_t val = 0;
    unsigned i;

    if (!s || size == 0)
        return 0;
    if (!s->attached)
        return size >= 4 ? UINT32_C(0xFFFFFFFF) : UINT64_C(0xFF);

    reg = cf_decode_boot_offset(offset, &is_alt);
    if (reg == 0 && !is_alt) {
        val = s->boot_visible ? cf_data_read(s, size)
                              : (size >= 4 ? UINT32_C(0xFFFFFFFF) : UINT64_C(0xFF));
    } else if (reg >= 0) {
        for (i = 0; i < size; i++) {
            uint8_t byte = cf_read_reg8(s, reg, is_alt, true);
            val |= (uint64_t)byte << (8u * i);
        }
    } else {
        val = size >= 4 ? UINT32_C(0xFFFFFFFF) : UINT64_C(0xFF);
    }

    return val;
}

void cf_boot_write(cf_state_t *s, uint32_t offset, unsigned size,
                   uint64_t value)
{
    bool is_alt = false;
    int reg;
    unsigned i;

    if (!s || size == 0 || !s->attached)
        return;

    reg = cf_decode_boot_offset(offset, &is_alt);
    if (reg == 0 && !is_alt) {
        if (s->boot_visible)
            cf_data_write(s, size, value);
    } else if (reg >= 0) {
        for (i = 0; i < size; i++) {
            cf_write_reg8(s, reg, is_alt, true,
                (uint8_t)((value >> (8u * i)) & 0xFFu));
        }
    }

}
