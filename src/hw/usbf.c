/*
 * USBF — BE-300 USB function (device) controller.
 *
 * The BE-300 is a USB 1.1 full-speed device (DriverDesignGuidlinesforBE-300
 * §3.3).  usb.dll drives this controller through the companion window; the
 * register semantics and the evidence for each are in
 * docs/BE300_USB_FUNCTION.md.  Summary of what is modelled:
 *
 *  - Standard requests (GET_DESCRIPTOR, SET_ADDRESS, SET_CONFIGURATION, ...)
 *    are answered by the controller from the descriptor RAM that usb.dll
 *    fills at 0x153C (hw_dump_vrc4173.txt:105-116).  usb.dll never parses
 *    them; it only reads the resulting address/config (0x1530/0x1534) on an
 *    INTSTAT2 config event (pc 0x0192253c).
 *  - Class/vendor requests are queued in the SETUP FIFO (0x14CC, one byte per
 *    read; the dump shows it popping 0x21, 0x22) and completed when the guest
 *    commits the EP0 status stage (0x1490 bit 0) or stalls (0x1460 bit 3).
 *  - EP1 IN: the guest DMAs up to 64 bytes from RAM, then commits the packet
 *    (0x1490 bit 1); 0x1324 bit 3 requests the next DMA while 0x1488
 *    bit 7 is set.  EP2 OUT: host packets are read by DMA into RAM or by PIO
 *    from 0x14D4 / 0x14D8.  EP3 IN: bytes written to 0x14E0 are committed as
 *    one interrupt packet (0x1490 bit 2).
 *
 * DMA completes synchronously: the transfer is at most 64 bytes, and usb.dll
 * only ever waits for the done status, never for an intermediate busy state.
 */

#include "usbf.h"

#include <string.h>

static unsigned usbf_word(uint32_t off)
{
    return (unsigned)((off - USBF_WINDOW_BASE) / 4u);
}

static void usbf_notify(usbf_state_t *u)
{
    if (u->irq_changed)
        u->irq_changed(u->ctx);
}

static bool usbf_out_pending(const usbf_state_t *u)
{
    return u->out_count != 0;
}

static uint8_t usbf_intstat(const usbf_state_t *u, unsigned i)
{
    uint8_t v = u->intstat[i];

    /* Level sources: EP1 FIFO empty and EP2 OUT data pending. */
    if (i == 0 && u->in_count == 0)
        v |= USBF_I0_EP1_EMPTY;
    if (i == 1 && usbf_out_pending(u))
        v |= USBF_I1_OUT_READY;
    return v;
}

static bool usbf_core_pending(const usbf_state_t *u)
{
    for (unsigned i = 0; i < 3; i++)
        if (usbf_intstat(u, i) & (uint8_t)~u->intmask[i])
            return true;
    return false;
}

static uint8_t usbf_top_stat(const usbf_state_t *u)
{
    uint8_t v = u->top_latched;

    /*
     * EP1 DMA request: usb.dll writes 0x1488 = 0x85 while TX ring entries
     * remain and 0 when the ring is drained (pc 0x01922a60 / 0x01922b54,
     * values from 0x01926680 for board 0x7100), then re-enables this source
     * after every TX-done (pc 0x01922a9c).  The real-hardware dump has this
     * bit clear with its enable set (0x1324 = 0x01, 0x132C = 0x1E).
     */
    if ((u->ep1_ctrl & USBF_EP1_DMA_REQ) && u->in_count < USBF_IN_SLOTS)
        v |= USBF_TOP_TX_REQ;

    if (usbf_out_pending(u))
        v |= USBF_TOP_OUT_READY;
    if (usbf_core_pending(u))
        v |= USBF_TOP_CORE;
    return v;
}

bool usbf_irq_pending(const usbf_state_t *u)
{
    return u && u->unit_en && (usbf_top_stat(u) & u->top_en) != 0;
}

static void usbf_flush_fifos(usbf_state_t *u)
{
    u->out_head = 0;
    u->out_count = 0;
    u->out_pos = 0;
    u->in_head = 0;
    u->in_count = 0;
    u->in_stage.len = 0;
    u->ep3_stage.len = 0;
    u->ep3_ready = false;
    u->setup_pos = 0;
}

static void usbf_finish_control(usbf_state_t *u, int status)
{
    if (!u->ctrl_pending)
        return;
    u->ctrl_pending = false;
    if (u->ctrl_done)
        u->ctrl_done(u->ctx, status);
}

void usbf_init(usbf_state_t *u)
{
    usbf_ram_fn ram = u->ram;
    usbf_event_fn irq_changed = u->irq_changed;
    usbf_event_fn in_ready = u->in_ready;
    usbf_ctrl_done_fn ctrl_done = u->ctrl_done;
    void *ctx = u->ctx;

    memset(u, 0, sizeof(*u));
    u->ram = ram;
    u->irq_changed = irq_changed;
    u->in_ready = in_ready;
    u->ctrl_done = ctrl_done;
    u->ctx = ctx;
    /* Masked until usb.dll programs INTMASK0..2 (init pc 0x019266f8). */
    u->intmask[0] = u->intmask[1] = u->intmask[2] = 0xFF;
}

void usbf_reset(usbf_state_t *u)
{
    bool attached = u->attached;

    usbf_finish_control(u, USBF_CTRL_STALL);
    usbf_init(u);
    u->attached = attached;
    usbf_notify(u);
}

bool usbf_handles(uint32_t off)
{
    return off >= USBF_WINDOW_BASE && off < USBF_WINDOW_END;
}

static uint8_t usbf_out_byte(usbf_state_t *u)
{
    usbf_packet_t *p;
    uint8_t b;

    if (!usbf_out_pending(u))
        return 0;
    p = &u->out[u->out_head];
    b = u->out_pos < p->len ? p->data[u->out_pos] : 0;
    if (++u->out_pos >= p->len) {
        u->out_head = (u->out_head + 1u) % USBF_OUT_SLOTS;
        u->out_count--;
        u->out_pos = 0;
    }
    return b;
}

static uint8_t usbf_out_remaining(const usbf_state_t *u)
{
    if (!usbf_out_pending(u))
        return 0;
    return (uint8_t)(u->out[u->out_head].len - u->out_pos);
}

uint32_t usbf_read(usbf_state_t *u, uint32_t off)
{
    off &= ~3u;
    if (off >= USBF_DESC_BASE && off < USBF_DESC_BASE + 4u * USBF_DESC_WORDS)
        return u->desc[(off - USBF_DESC_BASE) / 4u];

    switch (off) {
    case USBF_UNIT_EN:    return u->unit_en;
    case USBF_DMA_EN:     return u->dma_en;
    case USBF_DMA_START:  return 0;        /* never observed busy */
    case USBF_DMA_DIR:    return u->dma_dir;
    case USBF_DMA_HI:     return u->dma_hi;
    case USBF_DMA_LEN:    return u->dma_len;
    /* hw_dump_vrc4173.txt:84-85 reads back PA 0x8040 / 0x9F80. */
    case USBF_DMA_ADDR:   return u->dma_addr & 0x1FFFFFFFu;
    case USBF_TOP_STAT:   return usbf_top_stat(u);
    case USBF_TOP_EN:     return u->top_en;
    case USBF_CORE_EN:    return u->core_en;
    case USBF_DEV_STATE:  return u->config ? USBF_DEV_CONFIGURED : 0;
    case USBF_OUT_STATE:
        return (usbf_out_pending(u) ? USBF_OUT_PENDING : 0) |
               (u->ep3_ready ? USBF_EP3_BUSY : 0);
    case USBF_EP0_CTRL:   return u->ep0_ctrl;
    case USBF_INTSTAT0:   return usbf_intstat(u, 0);
    case USBF_INTSTAT1:   return usbf_intstat(u, 1);
    case USBF_INTSTAT2:   return usbf_intstat(u, 2);
    case USBF_INTMASK0:   return u->intmask[0];
    case USBF_INTMASK1:   return u->intmask[1];
    case USBF_INTMASK2:   return u->intmask[2];
    case USBF_EP1_CTRL:   return u->ep1_ctrl;
    case USBF_EP_CTRL:    return 0;        /* command bits self-clear */
    case USBF_EP_COMMIT:  return 0;        /* command bits self-clear */
    case USBF_SETUP_FIFO: {
        uint8_t b = u->setup[u->setup_pos & 7u];

        if (u->setup_pos < 8u && ++u->setup_pos == 8u) {
            /* usb.dll never clears INTSTAT1 bit 0 explicitly. */
            u->intstat[1] &= (uint8_t)~USBF_I1_SETUP;
            usbf_notify(u);
        }
        return b;
    }
    case USBF_OUT_FIFO: {
        uint8_t b = usbf_out_byte(u);

        usbf_notify(u);
        return b;
    }
    case USBF_OUT_COUNT:  return usbf_out_remaining(u);
    case USBF_ADDRESS:    return u->address;
    case USBF_CONFIG:     return u->config;
    default:
        return u->other[usbf_word(off)];
    }
}

static void usbf_dma(usbf_state_t *u)
{
    uint32_t pa = u->dma_addr & 0x1FFFFFFFu;
    uint32_t len = u->dma_len;
    uint8_t *ram;

    if (len > USBF_EP_MAX_PACKET)
        len = USBF_EP_MAX_PACKET;
    ram = (u->ram && len) ? u->ram(u->ctx, pa, len) : NULL;

    if (u->dma_dir & USBF_DMA_DIR_TX) {
        u->in_stage.len = (uint8_t)len;
        if (ram)
            memcpy(u->in_stage.data, ram, len);
        u->top_latched |= USBF_TOP_TX_DONE;
    } else {
        for (uint32_t i = 0; i < len && usbf_out_pending(u); i++) {
            uint8_t b = usbf_out_byte(u);

            if (ram)
                ram[i] = b;
        }
        u->top_latched |= USBF_TOP_RX_DONE;
    }
}

static void usbf_commit(usbf_state_t *u, uint32_t val)
{
    if (val & USBF_COMMIT_EP0) {
        /* Status-stage handshake for a class request (pc 0x019227fc). */
        u->intstat[2] |= USBF_I2_STATUS_DONE;
        usbf_finish_control(u, USBF_CTRL_OK);
    }
    if (val & USBF_COMMIT_EP1) {
        /* An empty commit is a ZLP (clear + commit, pc 0x01922ad8). */
        if (u->in_count < USBF_IN_SLOTS) {
            unsigned slot = (u->in_head + u->in_count) % USBF_IN_SLOTS;

            u->in[slot] = u->in_stage;
            u->in_count++;
        }
        u->in_stage.len = 0;
        if (u->in_ready)
            u->in_ready(u->ctx);
    }
    if (val & USBF_COMMIT_EP3) {
        u->ep3 = u->ep3_stage;
        u->ep3_ready = true;
        u->ep3_stage.len = 0;
        if (u->in_ready)
            u->in_ready(u->ctx);
    }
}

void usbf_write(usbf_state_t *u, uint32_t off, uint32_t val)
{
    uint8_t b = (uint8_t)val;

    off &= ~3u;
    if (off >= USBF_DESC_BASE && off < USBF_DESC_BASE + 4u * USBF_DESC_WORDS) {
        u->desc[(off - USBF_DESC_BASE) / 4u] = b;
        return;
    }

    switch (off) {
    case USBF_UNIT_EN:  u->unit_en = b & 1u; break;
    case USBF_DMA_EN:   u->dma_en = b; break;
    case USBF_DMA_START:
        if ((b & 1u) && !(b & USBF_DMA_ABORT))
            usbf_dma(u);
        break;
    case USBF_DMA_DIR:  u->dma_dir = b; break;
    case USBF_DMA_HI:   u->dma_hi = b; break;
    case USBF_DMA_LEN:  u->dma_len = b; break;
    case USBF_DMA_ADDR: u->dma_addr = val; break;
    case USBF_TOP_CLR:  u->top_latched &= (uint8_t)~b; break;
    case USBF_TOP_EN:   u->top_en = b; break;
    case USBF_RESET:
        if (b & 1u) {
            usbf_finish_control(u, USBF_CTRL_STALL);
            usbf_flush_fifos(u);
            u->intstat[0] = u->intstat[1] = u->intstat[2] = 0;
            u->top_latched = 0;
        }
        break;
    case USBF_CORE_EN:  u->core_en = b & 1u; break;
    case USBF_EP0_CTRL:
        if ((b & USBF_EP0_STALL) && u->ctrl_pending)
            usbf_finish_control(u, USBF_CTRL_STALL);
        u->ep0_ctrl = b & (uint8_t)~USBF_EP0_STALL;
        break;
    /* INTCLRn: bits written as 0 clear the status bit (pc 0x0192250c). */
    case USBF_INTCLR0:  u->intstat[0] &= b; break;
    case USBF_INTCLR1:  u->intstat[1] &= b; break;
    case USBF_INTCLR2:  u->intstat[2] &= b; break;
    case USBF_INTMASK0: u->intmask[0] = b; break;
    case USBF_INTMASK1: u->intmask[1] = b; break;
    case USBF_INTMASK2: u->intmask[2] = b; break;
    case USBF_EP1_CTRL: u->ep1_ctrl = b; break;
    case USBF_EP_CTRL:
        /*
         * FIFO-clear commands: init writes 0x3F (pc 0x019237a8); bit 2
         * clears the EP1 fill side before an empty commit sends a ZLP.
         * Packets already committed to the host are not affected.
         */
        if (b & USBF_EP_CLR_EP1)
            u->in_stage.len = 0;
        break;
    case USBF_EP_COMMIT: usbf_commit(u, b); break;
    case USBF_EP3_FIFO:
        if (u->ep3_stage.len < sizeof(u->ep3_stage.data))
            u->ep3_stage.data[u->ep3_stage.len++] = b;
        break;
    case USBF_ADDRESS:
    case USBF_CONFIG:
    case USBF_TOP_STAT:
    case USBF_SETUP_FIFO:
    case USBF_OUT_FIFO:
    case USBF_OUT_COUNT:
        break;
    default:
        u->other[usbf_word(off)] = val;
        break;
    }
    usbf_notify(u);
}

/* ---- host (USB bus) side ---- */

bool usbf_descriptors_ready(const usbf_state_t *u)
{
    return u->unit_en && u->desc[0] == 18u && u->desc[1] == 1u &&
           u->desc[18] == 9u && u->desc[19] == 2u;
}

void usbf_host_attach(usbf_state_t *u, bool attached)
{
    if (u->attached == attached)
        return;
    u->attached = attached;
    if (!attached) {
        usbf_finish_control(u, USBF_CTRL_STALL);
        usbf_flush_fifos(u);
        u->address = 0;
        u->config = 0;
    }
    usbf_notify(u);
}

void usbf_host_bus_reset(usbf_state_t *u)
{
    usbf_finish_control(u, USBF_CTRL_STALL);
    usbf_flush_fifos(u);
    u->address = 0;
    u->config = 0;
    u->intstat[2] |= USBF_I2_BUS_RESET;
    usbf_notify(u);
}

size_t usbf_descriptor_copy(const usbf_state_t *u, uint8_t type,
                            uint8_t *buf, size_t max)
{
    size_t start, len;

    if (type == 1) {
        start = 0;
        len = u->desc[0];
    } else if (type == 2) {
        start = 18;
        len = (size_t)u->desc[20] | ((size_t)u->desc[21] << 8);
    } else {
        return 0;
    }
    if (start + len > USBF_DESC_WORDS)
        len = USBF_DESC_WORDS - start;
    if (len > max)
        len = max;
    memcpy(buf, &u->desc[start], len);
    return len;
}

static int usbf_standard_request(usbf_state_t *u, const uint8_t s[8],
                                 uint8_t *in, uint16_t *in_len)
{
    uint8_t rt = s[0], req = s[1];
    uint16_t value = (uint16_t)(s[2] | (s[3] << 8));
    uint16_t length = (uint16_t)(s[6] | (s[7] << 8));
    size_t n;

    *in_len = 0;
    switch (req) {
    case 0x06: /* GET_DESCRIPTOR */
        if (rt != 0x80)
            return USBF_CTRL_STALL;
        n = usbf_descriptor_copy(u, (uint8_t)(value >> 8), in, length);
        if (n == 0)
            return USBF_CTRL_STALL;   /* no string descriptors in RAM */
        *in_len = (uint16_t)n;
        return USBF_CTRL_OK;
    case 0x05: /* SET_ADDRESS */
        u->address = (uint8_t)(value & 0x7Fu);
        return USBF_CTRL_OK;
    case 0x09: /* SET_CONFIGURATION */
        if (value > u->desc[17])
            return USBF_CTRL_STALL;
        u->config = (uint8_t)value;
        u->intstat[2] |= USBF_I2_CONFIG;
        usbf_notify(u);
        return USBF_CTRL_OK;
    case 0x08: /* GET_CONFIGURATION */
        if (length) {
            in[0] = u->config;
            *in_len = 1;
        }
        return USBF_CTRL_OK;
    case 0x00: /* GET_STATUS */
        if (length >= 2) {
            /* Device: self-powered from bmAttributes (0x40). */
            in[0] = (rt == 0x80 && (u->desc[25] & 0x40u)) ? 1u : 0u;
            in[1] = 0;
            *in_len = 2;
        }
        return USBF_CTRL_OK;
    case 0x0A: /* GET_INTERFACE */
        if (length) {
            in[0] = 0;
            *in_len = 1;
        }
        return USBF_CTRL_OK;
    case 0x0B: /* SET_INTERFACE */
        return value == 0 ? USBF_CTRL_OK : USBF_CTRL_STALL;
    case 0x01: /* CLEAR_FEATURE */
    case 0x03: /* SET_FEATURE */
        return USBF_CTRL_OK;
    default:
        return USBF_CTRL_STALL;
    }
}

int usbf_host_control(usbf_state_t *u, const uint8_t setup[8],
                      uint8_t *in, uint16_t *in_len)
{
    *in_len = 0;
    if (!u->attached || !usbf_descriptors_ready(u))
        return USBF_CTRL_STALL;
    if ((setup[0] & 0x60u) == 0)
        return usbf_standard_request(u, setup, in, in_len);

    /* Class/vendor: hand to usb.dll through the SETUP FIFO. */
    if (u->ctrl_pending)
        usbf_finish_control(u, USBF_CTRL_STALL);
    memcpy(u->setup, setup, 8);
    u->setup_pos = 0;
    u->ctrl_pending = true;
    u->intstat[1] |= USBF_I1_SETUP | USBF_I1_SETUP_OVR | USBF_I1_STATUS_RDY;
    usbf_notify(u);
    return USBF_CTRL_PENDING;
}

bool usbf_host_out(usbf_state_t *u, const uint8_t *data, size_t len)
{
    usbf_packet_t *p;

    if (!u->attached || !u->config || len > USBF_EP_MAX_PACKET ||
        u->out_count >= USBF_OUT_SLOTS)
        return false;
    if (len == 0)
        return true;             /* ZLP: nothing reaches the FIFO */
    p = &u->out[(u->out_head + u->out_count) % USBF_OUT_SLOTS];
    memcpy(p->data, data, len);
    p->len = (uint8_t)len;
    u->out_count++;
    usbf_notify(u);
    return true;
}

int usbf_host_in(usbf_state_t *u, uint8_t *buf, size_t max)
{
    usbf_packet_t *p;
    size_t n;

    if (!u->attached || !u->config || u->in_count == 0)
        return -1;               /* NAK */
    p = &u->in[u->in_head];
    n = p->len < max ? p->len : max;
    memcpy(buf, p->data, n);
    u->in_head = (u->in_head + 1u) % USBF_IN_SLOTS;
    u->in_count--;
    usbf_notify(u);
    return (int)n;
}

int usbf_host_intr_in(usbf_state_t *u, uint8_t *buf, size_t max)
{
    size_t n;

    if (!u->attached || !u->config || !u->ep3_ready)
        return -1;
    n = u->ep3.len < max ? u->ep3.len : max;
    memcpy(buf, u->ep3.data, n);
    u->ep3_ready = false;
    usbf_notify(u);
    return (int)n;
}
