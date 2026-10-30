/* ROM/SPL NAND-path tests using a synthetic data-only image. */

#include "hw/nand.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures;
static uint8_t image[4 * NAND_PAGE_DATA];

/* nand.c's legacy UART diagnostic path is not exercised by these tests. */
void host_io_emit_serial(int ch) { (void)ch; }
bool host_io_stdout_enabled(void) { return false; }

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static void fill_image(void)
{
    for (unsigned page = 0; page < 4; page++)
        for (unsigned i = 0; i < NAND_PAGE_DATA; i++)
            image[page * NAND_PAGE_DATA + i] = (uint8_t)(page * 0x40u + i);
}

static void test_rom_stream_and_oob(void)
{
    nand_state_t n;

    nand_init(&n, image, sizeof(image), sizeof(image));
    CHECK(nand_read(&n, NAND_REG_STREAM_DATA, 4, 0) == UINT32_C(0xFFFFFFFF));

    nand_write(&n, NAND_REG_BOOT_CMD, 1, 0x06, 0);
    nand_write(&n, NAND_REG_BOOT_ADDR, 1, 0x00, 0);
    nand_write(&n, NAND_REG_BOOT_ADDR, 1, 0x01, 0);
    nand_write(&n, NAND_REG_BOOT_ADDR, 1, 0x00, 0);
    nand_write(&n, NAND_REG_BOOT_MODE, 1, 0x05, 0);
    CHECK(nand_read(&n, NAND_REG_BOOT_STATUS, 4, 0) == 1);
    CHECK(nand_read(&n, NAND_REG_STREAM_DATA, 4, 0) == UINT32_C(0x43424140));

    n.stream_cursor = NAND_PAGE_DATA;
    CHECK(nand_read(&n, NAND_REG_STREAM_DATA, 2, 0) == UINT16_C(0x55AA));
    CHECK(nand_read(&n, NAND_REG_STREAM_DATA, 1, 0) == 0x0F);
}

static void test_empty_ata_taskfile(void)
{
    nand_state_t n;

    nand_init(&n, image, sizeof(image), sizeof(image));
    /* A READ SECTORS command with no card latches nothing. */
    nand_write(&n, NAND_ATA_TF_BASE + 2, 1, 1, 0);
    nand_write(&n, NAND_ATA_TF_BASE + 6, 1, 0xE0, 0);
    nand_write(&n, NAND_ATA_TF_BASE + 7, 1, 0x20, 0);
    CHECK(nand_read(&n, NAND_ATA_TF_BASE + 7, 1, 0) == 0xFF);
    CHECK(nand_read(&n, NAND_ATA_TF_BASE, 4, 0) == UINT32_C(0xFFFFFFFF));
    CHECK(nand_read(&n, NAND_ATA_ALT_STATUS, 1, 0) == 0xFF);
}

static void test_clean_ecc_and_inactive_buffer(void)
{
    nand_state_t n;

    nand_init(&n, image, sizeof(image), sizeof(image));
    CHECK(nand_read(&n, NAND_REG_STREAM_DATA, 1, 0) == 0xFF);
    nand_write(&n, NAND_REG_BOOT_MODE, 1, 0x01, 0);
    for (unsigned i = 0; i < 6; i++)
        nand_write(&n, NAND_REG_BOOT_ECC_IN, 2, 0x155 + i, 0);
    CHECK(n.boot_ecc_count == 6);
    for (uint32_t off = NAND_REG_BOOT_ECC_OUT_BASE;
         off < NAND_REG_BOOT_ECC_OUT_END; off += 4)
        CHECK(nand_read(&n, off, 4, 0) == 0);
    CHECK(nand_read(&n, NAND_REG_BOOT_STATUS2, 4, 0) == 0);
}

int main(void)
{
    /* LCD/backlight accesses must never enter the legacy recovery engine. */
    for (unsigned off = 0x980; off < 0x990; off++)
        CHECK(!nand_restore_handles_offset(off));
    for (unsigned off = 0x9A0; off < 0x9AC; off++)
        CHECK(!nand_restore_handles_offset(off));
    for (unsigned off = 0xC00; off < 0xC50; off++)
        CHECK(!nand_restore_handles_offset(off));
    CHECK(nand_restore_handles_offset(0xB00));
    CHECK(nand_restore_handles_offset(0xC50));
    fill_image();
    test_rom_stream_and_oob();
    test_empty_ata_taskfile();
    test_clean_ecc_and_inactive_buffer();
    if (failures) {
        fprintf(stderr, "test_nand_boot: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_nand_boot: all tests passed");
    return 0;
}
