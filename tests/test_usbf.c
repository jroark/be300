/*
 * USB function controller model test (src/hw/usbf.c).
 *
 * Replays usb.dll's init writes (docs/BE300_USB_FUNCTION.md, pc
 * 0x01923398 / 0x01923574) and then plays both sides: the USB host through
 * the usbf_host_* API and usb.dll through register accesses.
 */

#include "hw/usbf.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
            #cond); \
        failures++; \
    } \
} while (0)

/* Descriptor bytes from hw_dump_vrc4173.txt:105-116. */
static const uint8_t k_desc[] = {
    0x12, 0x01, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0x10, 0xCF, 0x07, 0x02, 0x20,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x09, 0x02, 0x27, 0x00, 0x01, 0x01, 0x00, 0x40, 0x01,
    0x09, 0x04, 0x00, 0x00, 0x03, 0xFF, 0xFF, 0xFF, 0x00,
    0x07, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x83, 0x03, 0x08, 0x00, 0x01,
};

static uint8_t g_ram[0x10000];
static int g_irq_events;
static int g_in_ready;
static int g_ctrl_status = 99;

static uint8_t *ram_fn(void *ctx, uint32_t pa, uint32_t len)
{
    (void)ctx;
    if (pa + len > sizeof(g_ram))
        return NULL;
    return &g_ram[pa];
}

static void irq_fn(void *ctx) { (void)ctx; g_irq_events++; }
static void in_ready_fn(void *ctx) { (void)ctx; g_in_ready++; }
static void ctrl_fn(void *ctx, int status) { (void)ctx; g_ctrl_status = status; }

static void guest_init(usbf_state_t *u)
{
    usbf_write(u, USBF_TOP_EN, 0);
    usbf_write(u, USBF_DMA_EN, 0);
    usbf_write(u, USBF_UNIT_EN, 1);
    usbf_write(u, USBF_RESET, 1);
    usbf_write(u, USBF_RESET, 0);
    usbf_write(u, USBF_TOP_CLR, 3);
    usbf_write(u, 0x1494, 1);
    usbf_write(u, 0x1494, ~1u);
    usbf_write(u, USBF_CORE_EN, 1);
    CHECK(usbf_read(u, USBF_CORE_EN) != 0);   /* init polls this */
    for (unsigned i = 0; i < sizeof(k_desc); i++)
        usbf_write(u, USBF_DESC_BASE + 4u * i, k_desc[i]);
    usbf_write(u, 0x1500, 1);
    usbf_write(u, USBF_INTCLR0, 0);
    usbf_write(u, USBF_INTCLR1, 0);
    usbf_write(u, USBF_INTCLR2, 0);
    usbf_write(u, USBF_EP_CTRL, 0x3F);
    usbf_write(u, USBF_INTMASK0, 0x7D);
    usbf_write(u, USBF_INTMASK1, 0x38);
    usbf_write(u, USBF_INTMASK2, 0x09);
    usbf_write(u, USBF_TOP_EN, 0x1A);
    usbf_write(u, 0x14C0, 0x22);
    usbf_write(u, USBF_CORE_EN, 0);
}

static void setup_model(usbf_state_t *u)
{
    memset(u, 0, sizeof(*u));
    u->ram = ram_fn;
    u->irq_changed = irq_fn;
    u->in_ready = in_ready_fn;
    u->ctrl_done = ctrl_fn;
    usbf_init(u);
    guest_init(u);
    usbf_host_attach(u, true);
}

static int ctrl(usbf_state_t *u, uint8_t rt, uint8_t req, uint16_t value,
                uint16_t index, uint16_t length, uint8_t *in, uint16_t *in_len)
{
    uint8_t s[8] = { rt, req, (uint8_t)value, (uint8_t)(value >> 8),
                     (uint8_t)index, (uint8_t)(index >> 8),
                     (uint8_t)length, (uint8_t)(length >> 8) };
    return usbf_host_control(u, s, in, in_len);
}

static void test_enumeration(void)
{
    usbf_state_t u;
    uint8_t buf[64];
    uint16_t n;

    setup_model(&u);
    CHECK(usbf_descriptors_ready(&u));
    CHECK(!usbf_irq_pending(&u));

    CHECK(ctrl(&u, 0x80, 6, 0x0100, 0, 64, buf, &n) == USBF_CTRL_OK);
    CHECK(n == 18);
    CHECK(buf[8] == 0xCF && buf[9] == 0x07 && buf[10] == 0x02 &&
          buf[11] == 0x20);
    CHECK(ctrl(&u, 0x80, 6, 0x0200, 0, 9, buf, &n) == USBF_CTRL_OK);
    CHECK(n == 9);
    CHECK(ctrl(&u, 0x80, 6, 0x0200, 0, 255, buf, &n) == USBF_CTRL_OK);
    CHECK(n == 0x27 && buf[18] == 0x07 && buf[20] == 0x81);
    CHECK(ctrl(&u, 0x80, 6, 0x0301, 0, 255, buf, &n) == USBF_CTRL_STALL);

    CHECK(ctrl(&u, 0x00, 5, 8, 0, 0, buf, &n) == USBF_CTRL_OK);
    CHECK(usbf_read(&u, USBF_ADDRESS) == 8);
    CHECK(ctrl(&u, 0x00, 9, 1, 0, 0, buf, &n) == USBF_CTRL_OK);

    /* INTSTAT2 bit 2 is unmasked (0x09) and reaches the top level. */
    CHECK(usbf_read(&u, USBF_INTSTAT2) & USBF_I2_CONFIG);
    CHECK(usbf_read(&u, USBF_TOP_STAT) & USBF_TOP_CORE);
    CHECK(usbf_irq_pending(&u));
    CHECK(usbf_read(&u, USBF_DEV_STATE) & USBF_DEV_CONFIGURED);
    CHECK(usbf_read(&u, USBF_CONFIG) == 1);
    usbf_write(&u, USBF_INTCLR2, (uint8_t)~USBF_I2_CONFIG);
    CHECK(!(usbf_read(&u, USBF_INTSTAT2) & USBF_I2_CONFIG));
    CHECK(!usbf_irq_pending(&u));
}

static void test_class_request(void)
{
    usbf_state_t u;
    uint8_t buf[8], setup[8];
    uint16_t n;

    setup_model(&u);
    ctrl(&u, 0x00, 9, 1, 0, 0, buf, &n);
    usbf_write(&u, USBF_INTCLR2, 0);

    /* SET_CONTROL_LINE_STATE, DTR|RTS */
    CHECK(ctrl(&u, 0x21, 0x22, 3, 0, 0, buf, &n) == USBF_CTRL_PENDING);
    CHECK(usbf_irq_pending(&u));
    CHECK(usbf_read(&u, USBF_INTSTAT1) & USBF_I1_SETUP);
    for (int i = 0; i < 8; i++) {
        usbf_write(&u, USBF_INTCLR1, (uint8_t)~USBF_I1_SETUP_OVR);
        setup[i] = (uint8_t)usbf_read(&u, USBF_SETUP_FIFO);
    }
    CHECK(setup[0] == 0x21 && setup[1] == 0x22 && setup[2] == 3);
    CHECK(!(usbf_read(&u, USBF_INTSTAT1) &
            (USBF_I1_SETUP | USBF_I1_SETUP_OVR)));
    CHECK(usbf_read(&u, USBF_INTSTAT1) & USBF_I1_STATUS_RDY);
    CHECK(g_ctrl_status == 99);

    usbf_write(&u, USBF_EP_CTRL, 2);
    usbf_write(&u, USBF_EP_COMMIT, usbf_read(&u, USBF_EP_COMMIT) | 1u);
    CHECK(g_ctrl_status == USBF_CTRL_OK);
    CHECK(usbf_read(&u, USBF_INTSTAT2) & USBF_I2_STATUS_DONE);

    /* A vendor request usb.dll rejects is stalled through 0x1460 bit 3. */
    g_ctrl_status = 99;
    CHECK(ctrl(&u, 0xC0, 0x01, 0, 0, 4, buf, &n) == USBF_CTRL_PENDING);
    usbf_write(&u, USBF_EP0_CTRL, usbf_read(&u, USBF_EP0_CTRL) | 8u);
    CHECK(g_ctrl_status == USBF_CTRL_STALL);
}

static void test_bulk_out(void)
{
    usbf_state_t u;
    uint8_t buf[64], pkt[64];
    uint16_t n;
    unsigned cnt;

    setup_model(&u);
    CHECK(!usbf_host_out(&u, pkt, 4));         /* not configured yet */
    ctrl(&u, 0x00, 9, 1, 0, 0, buf, &n);

    for (int i = 0; i < 64; i++)
        pkt[i] = (uint8_t)(0x40 + i);

    /* PIO: 0x1454 bit 3, count at 0x14D8, data from 0x14D4. */
    CHECK(usbf_host_out(&u, pkt, 10));
    CHECK(usbf_read(&u, USBF_OUT_STATE) & USBF_OUT_PENDING);
    CHECK(usbf_read(&u, USBF_INTSTAT1) & USBF_I1_OUT_READY);
    cnt = usbf_read(&u, USBF_OUT_COUNT);
    CHECK(cnt == 10);
    for (unsigned i = 0; i < cnt; i++)
        buf[i] = (uint8_t)usbf_read(&u, USBF_OUT_FIFO);
    CHECK(memcmp(buf, pkt, 10) == 0);
    CHECK(!(usbf_read(&u, USBF_OUT_STATE) & USBF_OUT_PENDING));

    /* DMA into the RX ring at PA 0xA000 (pc 0x01922f0c). */
    CHECK(usbf_host_out(&u, pkt, 64));
    CHECK(usbf_read(&u, USBF_TOP_STAT) & USBF_TOP_OUT_READY);
    cnt = usbf_read(&u, USBF_OUT_COUNT);
    usbf_write(&u, USBF_DMA_DIR, 0);
    usbf_write(&u, USBF_DMA_LEN, cnt);
    usbf_write(&u, USBF_DMA_ADDR, 0xA000A000u + 64u * 3u);
    usbf_write(&u, USBF_DMA_START, 1);
    CHECK(memcmp(&g_ram[0xA000 + 64 * 3], pkt, 64) == 0);
    CHECK(usbf_read(&u, USBF_TOP_STAT) & USBF_TOP_RX_DONE);
    usbf_write(&u, USBF_TOP_CLR, USBF_TOP_RX_DONE);
    CHECK(!(usbf_read(&u, USBF_TOP_STAT) & USBF_TOP_RX_DONE));
    CHECK(usbf_read(&u, USBF_DMA_ADDR) == 0xA000u + 64u * 3u);

    /* The FIFO holds USBF_OUT_SLOTS packets; the next one is NAKed. */
    for (unsigned i = 0; i < USBF_OUT_SLOTS; i++)
        CHECK(usbf_host_out(&u, pkt, 64));
    CHECK(!usbf_host_out(&u, pkt, 64));
}

static void test_bulk_in(void)
{
    usbf_state_t u;
    uint8_t buf[64];
    uint16_t n;

    setup_model(&u);
    ctrl(&u, 0x00, 9, 1, 0, 0, buf, &n);
    CHECK(usbf_host_in(&u, buf, 64) == -1);
    CHECK(usbf_read(&u, USBF_INTSTAT0) & USBF_I0_EP1_EMPTY);

    /* 0x1488 = 0x85 requests EP1 DMA through 0x1324 bit 3; 0 stops it. */
    usbf_write(&u, USBF_INTCLR2, 0);           /* ack the config event */
    CHECK(!usbf_irq_pending(&u));
    CHECK(!(usbf_read(&u, USBF_TOP_STAT) & USBF_TOP_TX_REQ));
    usbf_write(&u, USBF_EP1_CTRL, 0x85);
    CHECK(usbf_read(&u, USBF_TOP_STAT) & USBF_TOP_TX_REQ);
    CHECK(usbf_irq_pending(&u));
    usbf_write(&u, USBF_EP1_CTRL, 0);
    CHECK(!usbf_irq_pending(&u));

    for (int i = 0; i < 20; i++)
        g_ram[0x8000 + 64 * 5 + i] = (uint8_t)(0x11 + i);
    /* TX DMA from the ring at PA 0x8000 (pc 0x01922b88). */
    usbf_write(&u, USBF_DMA_DIR, 1);
    usbf_write(&u, USBF_DMA_HI, 0);
    usbf_write(&u, USBF_DMA_LEN, 20);
    usbf_write(&u, USBF_DMA_ADDR, 0xA0008000u + 64u * 5u);
    usbf_write(&u, USBF_DMA_START, 1);
    CHECK(usbf_read(&u, USBF_TOP_STAT) & USBF_TOP_TX_DONE);
    CHECK(usbf_host_in(&u, buf, 64) == -1);    /* not committed yet */

    g_in_ready = 0;
    usbf_write(&u, USBF_EP_COMMIT, 2);
    CHECK(g_in_ready == 1);
    CHECK(!(usbf_read(&u, USBF_INTSTAT0) & USBF_I0_EP1_EMPTY));
    CHECK(usbf_host_in(&u, buf, 64) == 20);
    CHECK(buf[0] == 0x11 && buf[19] == 0x11 + 19);
    CHECK(usbf_read(&u, USBF_INTSTAT0) & USBF_I0_EP1_EMPTY);

    /* ZLP after a full packet: clear EP1 FIFO, commit (pc 0x01922ad8). */
    usbf_write(&u, USBF_EP_CTRL, usbf_read(&u, USBF_EP_CTRL) | 4u);
    usbf_write(&u, USBF_EP_COMMIT, 2);
    CHECK(usbf_host_in(&u, buf, 64) == 0);
    CHECK(usbf_host_in(&u, buf, 64) == -1);
}

static void test_ep3_and_reset(void)
{
    usbf_state_t u;
    uint8_t buf[8];
    uint16_t n;

    setup_model(&u);
    ctrl(&u, 0x00, 9, 1, 0, 0, buf, &n);

    /* Serial-state notification (pc 0x01923334..0x01923354). */
    CHECK(usbf_host_intr_in(&u, buf, 8) == -1);
    usbf_write(&u, USBF_EP3_FIFO, 0x81);
    usbf_write(&u, USBF_EP3_FIFO, 0);
    usbf_write(&u, USBF_EP_COMMIT, 4);
    CHECK(usbf_read(&u, USBF_OUT_STATE) & USBF_EP3_BUSY);
    CHECK(usbf_host_intr_in(&u, buf, 8) == 2);
    CHECK(buf[0] == 0x81 && buf[1] == 0);
    CHECK(!(usbf_read(&u, USBF_OUT_STATE) & USBF_EP3_BUSY));

    /* Bus reset drops the configuration and raises INTSTAT2 bit 1. */
    usbf_host_bus_reset(&u);
    CHECK(usbf_read(&u, USBF_CONFIG) == 0);
    CHECK(usbf_read(&u, USBF_INTSTAT2) & USBF_I2_BUS_RESET);

    /* Guest reset (KjCMU) drops descriptors-ready until usb.dll re-inits. */
    usbf_reset(&u);
    CHECK(!usbf_descriptors_ready(&u));
    guest_init(&u);
    CHECK(usbf_descriptors_ready(&u));
}

int main(void)
{
    test_enumeration();
    test_class_request();
    test_bulk_out();
    test_bulk_in();
    test_ep3_and_reset();
    if (failures) {
        fprintf(stderr, "test_usbf: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_usbf: ok\n");
    return 0;
}
