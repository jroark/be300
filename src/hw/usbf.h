#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * USBF — BE-300 USB function (device) controller in the companion window.
 *
 * Register map, interrupt route and data paths are reverse-engineered from
 * usb.dll and the real-hardware companion dump; see
 * docs/BE300_USB_FUNCTION.md.  Offsets below are companion-window offsets
 * (PA 0x0A000000 + off).  Every register is one 32-bit word holding a
 * byte-sized value.
 */

#define USBF_WINDOW_BASE   0x1300u
#define USBF_WINDOW_END    0x1700u    /* descriptor RAM ends at 0x161F */

/* 0x1300 block: DMA and top level */
#define USBF_UNIT_EN       0x1300u
#define USBF_DMA_EN        0x1308u
#define USBF_DMA_START     0x130Cu
#define USBF_DMA_DIR       0x1310u
#define USBF_DMA_HI        0x1318u
#define USBF_DMA_LEN       0x131Cu
#define USBF_DMA_ADDR      0x1320u
#define USBF_TOP_STAT      0x1324u
#define USBF_TOP_CLR       0x1328u
#define USBF_TOP_EN        0x132Cu
#define USBF_RESET         0x1338u

#define USBF_TOP_RX_DONE   0x01u
#define USBF_TOP_TX_DONE   0x02u
#define USBF_TOP_OUT_READY 0x04u
#define USBF_TOP_TX_REQ    0x08u   /* EP1 DMA request (level) */
#define USBF_TOP_CORE      0x10u

#define USBF_DMA_DIR_TX    0x01u      /* memory -> EP1 IN */
#define USBF_DMA_ABORT     0x10u

/* 0x1440 block: USB core */
#define USBF_CORE_EN       0x1444u
#define USBF_DEV_STATE     0x1450u
#define USBF_OUT_STATE     0x1454u
#define USBF_EP0_CTRL      0x1460u
#define USBF_INTSTAT0      0x1464u
#define USBF_INTSTAT1      0x1468u
#define USBF_INTSTAT2      0x146Cu
#define USBF_INTMASK0      0x1470u
#define USBF_INTMASK1      0x1474u
#define USBF_INTMASK2      0x1478u
#define USBF_INTCLR0       0x147Cu
#define USBF_INTCLR1       0x1480u
#define USBF_INTCLR2       0x1484u
#define USBF_EP1_CTRL      0x1488u
#define USBF_EP_CTRL       0x148Cu   /* FIFO-clear commands */
#define USBF_EP_COMMIT     0x1490u

#define USBF_DEV_CONFIGURED 0x80u     /* USBF_DEV_STATE */
#define USBF_OUT_PENDING    0x08u     /* USBF_OUT_STATE */
#define USBF_EP3_BUSY       0x10u     /* USBF_OUT_STATE */
#define USBF_EP0_STALL      0x08u     /* USBF_EP0_CTRL */

#define USBF_I0_EP1_EMPTY   0x04u
#define USBF_I1_SETUP       0x01u
#define USBF_I1_OUT_READY   0x04u
#define USBF_I1_SETUP_OVR   0x10u
#define USBF_I1_STATUS_RDY  0x20u
#define USBF_I2_BUS_RESET   0x02u
#define USBF_I2_CONFIG      0x04u
#define USBF_I2_STATUS_DONE 0x08u

#define USBF_EP_CLR_EP1     0x04u     /* USBF_EP_CTRL: clear EP1 FIFO */
#define USBF_EP1_DMA_REQ    0x80u     /* USBF_EP1_CTRL */
#define USBF_COMMIT_EP0     0x01u     /* USBF_EP_COMMIT */
#define USBF_COMMIT_EP1     0x02u
#define USBF_COMMIT_EP3     0x04u

/* 0x14C0 block: FIFO ports */
#define USBF_SETUP_FIFO    0x14CCu
#define USBF_OUT_FIFO      0x14D4u
#define USBF_OUT_COUNT     0x14D8u
#define USBF_EP3_FIFO      0x14E0u

/* 0x1500 block: hardware request state and descriptor RAM */
#define USBF_ADDRESS       0x1530u
#define USBF_CONFIG        0x1534u
#define USBF_DESC_BASE     0x153Cu
#define USBF_DESC_WORDS    ((0x1620u - USBF_DESC_BASE) / 4u)

#define USBF_EP_MAX_PACKET 64u
#define USBF_OUT_SLOTS     4u
#define USBF_IN_SLOTS      2u

/* Results of a host-side control transfer. */
enum {
    USBF_CTRL_OK = 0,       /* completed; *in_len holds data length */
    USBF_CTRL_STALL = -1,   /* request refused */
    USBF_CTRL_PENDING = 1,  /* handed to the guest; completion callback */
};

typedef struct {
    uint8_t  data[USBF_EP_MAX_PACKET];
    uint8_t  len;
} usbf_packet_t;

/* Returns a host pointer to len bytes of guest RAM at pa, or NULL. */
typedef uint8_t *(*usbf_ram_fn)(void *ctx, uint32_t pa, uint32_t len);
typedef void (*usbf_event_fn)(void *ctx);
typedef void (*usbf_ctrl_done_fn)(void *ctx, int status);

typedef struct {
    /* 0x1300 block */
    uint8_t  unit_en;
    uint8_t  dma_en;
    uint8_t  dma_dir;
    uint8_t  dma_hi;
    uint8_t  dma_len;
    uint32_t dma_addr;
    uint8_t  top_latched;       /* RX_DONE / TX_DONE */
    uint8_t  top_en;

    /* 0x1440 block */
    uint8_t  core_en;
    uint8_t  ep0_ctrl;
    uint8_t  intstat[3];
    uint8_t  intmask[3];
    uint8_t  ep1_ctrl;
    uint8_t  ep_commit;
    uint32_t other[(USBF_WINDOW_END - USBF_WINDOW_BASE) / 4u];

    /* 0x1500 block */
    uint8_t  address;
    uint8_t  config;
    uint8_t  desc[USBF_DESC_WORDS];

    /* EP0 class/vendor request handed to the guest */
    uint8_t  setup[8];
    uint8_t  setup_pos;
    bool     ctrl_pending;

    /* EP2 OUT receive FIFO (host -> guest) */
    usbf_packet_t out[USBF_OUT_SLOTS];
    unsigned out_head, out_count;
    uint8_t  out_pos;           /* bytes already read from out[out_head] */

    /* EP1 IN transmit path (guest -> host) */
    usbf_packet_t in_stage;     /* filled by TX DMA, sent on commit */
    usbf_packet_t in[USBF_IN_SLOTS];
    unsigned in_head, in_count;

    /* EP3 IN interrupt path (serial-state notifications) */
    usbf_packet_t ep3_stage;
    usbf_packet_t ep3;
    bool     ep3_ready;

    bool     attached;

    /* Guest side: RAM access and interrupt level changes. */
    usbf_ram_fn       ram;
    usbf_event_fn     irq_changed;
    void             *ctx;
    /* Host side: an IN packet was committed (EP1 or EP3), and a
     * class/vendor control transfer handed to the guest completed. */
    usbf_event_fn     in_ready;
    usbf_ctrl_done_fn ctrl_done;
    void             *host_ctx;
} usbf_state_t;

void     usbf_init(usbf_state_t *u);   /* keeps callbacks and contexts */
void     usbf_reset(usbf_state_t *u);
bool     usbf_handles(uint32_t off);
uint32_t usbf_read(usbf_state_t *u, uint32_t off);
void     usbf_write(usbf_state_t *u, uint32_t off, uint32_t val);
bool     usbf_irq_pending(const usbf_state_t *u);

/* Host (USB bus) side. */
bool usbf_descriptors_ready(const usbf_state_t *u);
void usbf_host_attach(usbf_state_t *u, bool attached);
void usbf_host_bus_reset(usbf_state_t *u);
int  usbf_host_control(usbf_state_t *u, const uint8_t setup[8],
                       uint8_t *in, uint16_t *in_len);
bool usbf_host_out(usbf_state_t *u, const uint8_t *data, size_t len);
int  usbf_host_in(usbf_state_t *u, uint8_t *buf, size_t max);
int  usbf_host_intr_in(usbf_state_t *u, uint8_t *buf, size_t max);
size_t usbf_descriptor_copy(const usbf_state_t *u, uint8_t type,
                            uint8_t *buf, size_t max);
