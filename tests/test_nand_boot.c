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

/* Physical pages must retain their own spare bytes and expose the
 * remainder polynomial that the ROM evaluates into RS syndromes. */
static uint16_t test_xt(uint16_t a) { a<<=1; return (a&0x400)?a^0x409:a; }
static uint16_t test_mul(uint16_t a,uint16_t b) {
    uint16_t r=0; for(unsigned i=0;i<10;i++,b>>=1,a=test_xt(a)) if(b&1) r^=a; return r;
}
static uint8_t test_rev(uint8_t b) {
    uint8_t r=0; for(unsigned i=0;i<8;i++)r=(uint8_t)((r<<1)|((b>>i)&1));return r;
}
static void test_command(nand_state_t *n,uint8_t cmd) {
    nand_write(n,NAND_REG_XFER_CMD,4,3,0);nand_write(n,NAND_REG_XFER_ADDR,1,cmd,0);
    (void)nand_read(n,NAND_REG_XFER_ACK,4,0);nand_write(n,NAND_REG_XFER_CMD,4,2,0);
}
static void test_address(nand_state_t *n,unsigned page,bool program) {
    nand_write(n,NAND_REG_XFER_CMD,4,6,0);
    if(program)nand_write(n,NAND_REG_XFER_ADDR,1,0,0);
    nand_write(n,NAND_REG_XFER_ADDR,1,page&255,0);nand_write(n,NAND_REG_XFER_ADDR,1,page>>8,0);
    (void)nand_read(n,NAND_REG_XFER_ACK,4,0);nand_write(n,NAND_REG_XFER_CMD,4,2,0);
}
static void test_physical(void) {
    static uint8_t data[NAND_IMAGE_SIZE],oob[NAND_OOB_IMAGE_SIZE];
    static const uint8_t parity[]={0xaa,0x55,0x0f,0xff,0,0,0,0,0x5b,0x2d,0x87,0x4e,0x2b,0x7d,0x9f,0x0e};
    memset(data,255,sizeof(data));memset(oob,255,sizeof(oob));memcpy(oob,parity,16);
    nand_state_t n;nand_init(&n,data,sizeof(data),sizeof(data));n.oob=oob;
    for(unsigned i=0;i<16;i++) {
        n.stream_active=true;n.stream_page=0;n.stream_col=0;n.stream_cursor=512+i;
        CHECK(nand_read(&n,NAND_REG_STREAM_DATA,1,0)==parity[i]);
    }
    const uint8_t *q=parity+8;
    uint16_t words[]={ (uint16_t)(((q[7]&15)<<6)|(q[6]>>2)),
        (uint16_t)(((q[6]&3)<<8)|q[5]), (uint16_t)((q[3]>>6)|(q[4]<<2)),
        (uint16_t)(((q[3]&63)<<4)|(q[2]>>4)),
        (uint16_t)(((q[2]&15)<<6)|(q[1]>>2)), (uint16_t)(((q[1]&3)<<8)|q[0]) };
    nand_write(&n,NAND_REG_XFER_MODE,4,1,0);
    for(unsigned i=0;i<6;i++)nand_write(&n,NAND_REG_XFER_MISC,4,words[i],0);
    CHECK(nand_read(&n,NAND_REG_XFER_STATUS2,4,0)==0);
    nand_write(&n,NAND_REG_XFER_MODE,4,1,0);
    for(unsigned i=0;i<6;i++)nand_write(&n,NAND_REG_XFER_MISC,4,words[i]^(i==0?1:0),0);
    CHECK(nand_read(&n,NAND_REG_XFER_STATUS2,4,0)==1);
    data[17]^=1;
    nand_write(&n,NAND_REG_XFER_MODE,4,1,0);
    for(unsigned i=0;i<6;i++)nand_write(&n,NAND_REG_XFER_MISC,4,words[i],0);
    CHECK(nand_read(&n,NAND_REG_XFER_STATUS2,4,0)==1);
    uint8_t b[8];
    for(unsigned i=0;i<4;i++) {
        uint32_t word=(uint32_t)nand_read(&n,NAND_REG_BUFFER_BASE+i*4,4,0);
        b[2*i]=test_rev((uint8_t)word);b[2*i+1]=test_rev((uint8_t)(word>>8));
    }
    uint16_t r[6];r[5]=(b[0]<<2)|(b[1]>>6);r[4]=((b[1]&63)<<4)|(b[2]>>4);
    r[3]=((b[2]&15)<<6)|(b[3]>>2);r[2]=((b[3]&3)<<8)|b[4];
    r[1]=(b[5]<<2)|(b[6]>>6);r[0]=((b[6]&63)<<4)|(b[7]>>4);
    uint16_t root=1;
    for(unsigned k=0;k<6;k++,root=test_xt(root)) {
        uint16_t result=0,expected=0x200;
        for(unsigned j=0;j<6;j++)result=test_mul(result,root)^r[j];
        for(unsigned j=0;j<k*(525-17);j++)expected=test_xt(expected);
        CHECK(result==expected);
    }
    test_command(&n,0x80);test_address(&n,64,true);nand_write(&n,NAND_REG_XFER_MODE,4,7,0);
    static const uint8_t tag[]={0xaa,0x55,0x0f,0xff,2,0,0,0};
    for(unsigned i=0;i<520;i++)nand_write(&n,NAND_REG_STREAM_DATA,1,i<512?0xa5:tag[i-512],0);
    nand_write(&n,NAND_REG_XFER_MODE,4,6,0);
    for(unsigned i=0;i<8;i++)nand_write(&n,NAND_REG_STREAM_DATA,1,0x5a,0);
    test_command(&n,0x10);CHECK(data[64*512]==0xa5);
    for(unsigned i=0;i<16;i++)CHECK(oob[64*16+i]==(i<8?tag[i]:0x5a));
    test_command(&n,0x60);test_address(&n,64,false);test_command(&n,0xd0);
    CHECK(data[64*512]==255);for(unsigned i=0;i<16;i++)CHECK(oob[64*16+i]==255);
    oob[96*16+5]=0;data[96*512]=0x27;
    test_command(&n,0x60);test_address(&n,96,false);test_command(&n,0xd0);
    CHECK((n.status_value&1)==1 && oob[96*16+5]==0 && data[96*512]==0x27);
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
    test_physical();
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
