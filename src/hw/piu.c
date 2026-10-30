/* VRC4173 touch-panel interface unit (PIU), User's Manual chapter 9. */

#include "hw/piu.h"

#include <string.h>

#define PIU_PANEL_WIDTH   240u
#define PIU_PANEL_HEIGHT  360u

#define PIUCNT_PADRST       0x0001u
#define PIUCNT_PIUSEQEN     0x0004u
#define PIUCNT_PIUMODE_MASK 0x0018u
#define PIUCNT_PADSCANTYPE  0x0020u
#define PIUCNT_PADSCANSTART 0x0040u
#define PIUCNT_PADATSTART   0x0100u
#define PIUCNT_PADATSTOP    0x0200u

static bool time_reached(uint64_t now_ns, uint64_t due_ns)
{
    return (int64_t)(now_ns - due_ns) >= 0;
}

static uint64_t conversion_delay_us(const piu_state_t *p)
{
    uint64_t stable_units = p->regs[3] & 0x003Fu;
    uint64_t conversions = (p->regs[0] & PIUCNT_PADSCANTYPE) ? 5u : 4u;

    return stable_units * 30ULL + conversions * 10ULL;
}

static uint64_t interval_delay_us(const piu_state_t *p)
{
    return (uint64_t)(p->regs[2] & 0x07FFu) * 30ULL +
        conversion_delay_us(p);
}

static void arm_sample(piu_state_t *p, uint64_t now_ns, uint64_t delay_us)
{
    p->data_armed = true;
    p->data_ready_ns = now_ns + delay_us * 1000ULL;
}

static void reset_registers(piu_state_t *p)
{
    bool touch_down = p->touch_down;
    uint16_t touch_x = p->touch_x;
    uint16_t touch_y = p->touch_y;

    memset(p, 0, sizeof(*p));
    p->regs[2] = PIU_RESET_SCAN_INTERVAL;
    p->regs[3] = PIU_RESET_STABLE;
    p->touch_down = touch_down;
    p->touch_x = touch_x;
    p->touch_y = touch_y;
}

void piu_init(piu_state_t *p)
{
    if (!p)
        return;
    memset(p, 0, sizeof(*p));
    reset_registers(p);
}

static void latch_sample(piu_state_t *p)
{
    uint32_t x = p->touch_x;
    uint32_t y = p->touch_y;

    if (x >= PIU_PANEL_WIDTH)
        x = PIU_PANEL_WIDTH - 1u;
    if (y >= PIU_PANEL_HEIGHT)
        y = PIU_PANEL_HEIGHT - 1u;

    p->adc[0] = (uint16_t)(0x81E1u +
        (y * (0x8E30u - 0x81E1u)) / (PIU_PANEL_HEIGHT - 1u));
    p->adc[1] = (uint16_t)(0x8E30u -
        (y * (0x8E30u - 0x81E1u)) / (PIU_PANEL_HEIGHT - 1u));
    p->adc[2] = (uint16_t)(0x8D1Bu -
        (x * (0x8D1Bu - 0x8300u)) / (PIU_PANEL_WIDTH - 1u));
    p->adc[3] = (uint16_t)(0x8300u +
        (x * (0x8D1Bu - 0x8300u)) / (PIU_PANEL_WIDTH - 1u));
}

static void raise_penchg(piu_state_t *p, bool down)
{
    if (p->regs[1] & PIU_PENDING_PENCHG) {
        p->queued_penchg = true;
        p->queued_penstc = down;
        return;
    }

    p->penstc = down;
    p->regs[1] |= PIU_PENDING_PENCHG;
}

static void clear_coordinate_state(piu_state_t *p)
{
    p->regs[1] &= (uint16_t)~PIU_PENDING_COORDINATE;
    memset(p->page_valid, 0, sizeof(p->page_valid));
    p->data_armed = false;
    p->data_ready_ns = 0;
    p->next_page = 0;
}

static bool wait_pen_enabled(const piu_state_t *p)
{
    uint16_t ctl = p->regs[0];

    return (ctl & PIUCNT_PIUSEQEN) && (ctl & PIUCNT_PADATSTART) &&
        (ctl & PIUCNT_PIUMODE_MASK) == 0 && p->adc_on;
}

static void signal_pen_down(piu_state_t *p, uint64_t now_ns)
{
    clear_coordinate_state(p);
    if (p->padstate == PIU_PADSTATE_WAIT_PEN &&
        (p->regs[0] & PIUCNT_PADATSTART)) {
        p->padstate = PIU_PADSTATE_PEN_DATA;
        arm_sample(p, now_ns, conversion_delay_us(p));
    }
    latch_sample(p);
    raise_penchg(p, true);
}

/*
 * Release. UM section 9.2 (7): released in IntervalNextScan, the sequencer
 * returns to WaitPenTouch only if PADATSTOP is set (9.3.1 bit 9, "auto stop
 * ... during release state"); otherwise it keeps scanning, so a later touch
 * resumes sampling without the contact-change interrupt only WaitPenTouch
 * raises (9.2 (5)). A real BE-300 driven without PADATSTOP sat in
 * IntervalNextScan after its first touch; Windows CE sets it (CNT 0x1304,
 * hw_dump_vrc4173.txt:986).
 */
static void signal_pen_up(piu_state_t *p)
{
    bool wait_enabled = wait_pen_enabled(p);
    bool should_signal = wait_enabled || p->padstate >= PIU_PADSTATE_WAIT_PEN;

    if (wait_enabled && p->padstate >= PIU_PADSTATE_PEN_DATA &&
        !(p->regs[0] & PIUCNT_PADATSTOP)) {
        raise_penchg(p, false);
        return;
    }
    p->data_armed = false;
    p->data_ready_ns = 0;
    if (wait_enabled)
        p->padstate = PIU_PADSTATE_WAIT_PEN;
    else if (p->padstate > PIU_PADSTATE_STANDBY)
        p->padstate = PIU_PADSTATE_STANDBY;
    if (should_signal)
        raise_penchg(p, false);
}

static bool page_available(const piu_state_t *p, unsigned page)
{
    uint16_t page_bit = page ? PIU_PENDING_PAGE1 : PIU_PENDING_PAGE0;

    return !(p->regs[1] & page_bit) && !p->page_valid[page];
}

static int select_page(const piu_state_t *p)
{
    unsigned page = p->next_page & 1u;
    unsigned other = page ^ 1u;

    if (page_available(p, page))
        return (int)page;
    if (page_available(p, other))
        return (int)other;
    return -1;
}

static void queue_coordinate_sample(piu_state_t *p, uint64_t now_ns)
{
    int page;
    uint16_t page_bit;

    if (!p->touch_down || p->padstate < PIU_PADSTATE_PEN_DATA ||
        !p->data_armed || !time_reached(now_ns, p->data_ready_ns))
        return;

    page = select_page(p);
    if (page < 0) {
        /* VRC4173 UM section 9.7 condition 3: both pages still valid. */
        p->regs[1] |= PIU_PENDING_DATALOST;
        p->padstate = PIU_PADSTATE_STANDBY;
        p->data_armed = false;
        p->data_ready_ns = 0;
        return;
    }

    latch_sample(p);
    memcpy(p->page_adc[page], p->adc, sizeof(p->adc));
    p->page_valid[page] = true;
    p->next_page = (uint8_t)(((unsigned)page ^ 1u) & 1u);
    page_bit = page ? PIU_PENDING_PAGE1 : PIU_PENDING_PAGE0;
    p->regs[1] |= page_bit;
    p->padstate = PIU_PADSTATE_INTERVAL;
    arm_sample(p, now_ns, interval_delay_us(p));
}

static void refresh_control_state(piu_state_t *p, uint64_t now_ns)
{
    uint16_t ctl;

    if (!p)
        return;
    ctl = p->regs[0];

    if (p->padstate == PIU_PADSTATE_DISABLE && ctl != 0)
        p->padstate = PIU_PADSTATE_STANDBY;

    if (wait_pen_enabled(p)) {
        /* UM section 9.7 requires software to clear both valid pages and
         * PADDLOSTINTR before restarting after a page-full data loss. */
        if (p->padstate == PIU_PADSTATE_STANDBY &&
            (p->regs[1] & PIU_PENDING_COORDINATE) != 0)
            return;
        if (p->padstate < PIU_PADSTATE_WAIT_PEN)
            p->padstate = PIU_PADSTATE_WAIT_PEN;
        if (p->touch_down && p->padstate == PIU_PADSTATE_WAIT_PEN)
            signal_pen_down(p, now_ns);
    } else if ((ctl & (PIUCNT_PIUSEQEN | PIUCNT_PADSCANSTART)) ==
        (PIUCNT_PIUSEQEN | PIUCNT_PADSCANSTART) &&
        (ctl & PIUCNT_PIUMODE_MASK) == 0 && p->adc_on &&
        p->padstate == PIU_PADSTATE_STANDBY) {
        p->padstate = PIU_PADSTATE_PEN_DATA;
        if (p->touch_down)
            arm_sample(p, now_ns, conversion_delay_us(p));
    } else if (!(ctl & PIUCNT_PIUSEQEN) &&
        p->padstate > PIU_PADSTATE_STANDBY) {
        clear_coordinate_state(p);
        p->padstate = PIU_PADSTATE_STANDBY;
    }
}

void piu_set_touch(piu_state_t *p, bool down, uint16_t x, uint16_t y,
    uint64_t now_ns)
{
    bool prev;

    if (!p)
        return;
    prev = p->touch_down;
    p->touch_down = down;
    p->touch_x = x;
    p->touch_y = y;
    if (p->padstate == PIU_PADSTATE_WAIT_PEN && down && !prev)
        signal_pen_down(p, now_ns);
    else if (!down && prev)
        signal_pen_up(p);
    else if (down)
        queue_coordinate_sample(p, now_ns);
}

void piu_tick(piu_state_t *p, uint64_t now_ns)
{
    if (p)
        queue_coordinate_sample(p, now_ns);
}

void piu_write(piu_state_t *p, uint32_t offset, uint16_t value,
    uint64_t now_ns)
{
    if (!p || offset > 0x18 || (offset & 3u) != 0)
        return;

    if (offset == 0x00) {
        if (value & PIUCNT_PADRST) {
            /* PADRST takes effect before it self-clears after four clocks. */
            reset_registers(p);
            return;
        }
        p->regs[0] = value & 0x03FEu;
        refresh_control_state(p, now_ns);
        return;
    }

    if (offset == 0x04) {
        uint16_t old_pending = p->regs[1] & 0x00FFu;
        uint16_t pending_ack = value & 0x00FFu;
        uint16_t new_mask = (value >> 8) & 0x00FFu;
        bool data_acked = (old_pending & pending_ack &
            PIU_PENDING_TOUCH_DATA) != 0;

        p->regs[1] &= (uint16_t)~(pending_ack & PIU_PENDING_ANY);
        p->regs[1] = (p->regs[1] & 0x00FFu) | (uint16_t)(new_mask << 8);
        if (pending_ack & PIU_PENDING_PAGE0)
            p->page_valid[0] = false;
        if (pending_ack & PIU_PENDING_PAGE1)
            p->page_valid[1] = false;
        if (pending_ack & PIU_PENDING_PENCHG) {
            if (p->queued_penchg) {
                p->penstc = p->queued_penstc;
                p->queued_penchg = false;
                p->regs[1] |= PIU_PENDING_PENCHG;
            } else {
                p->penstc = p->touch_down;
            }
        }
        if (data_acked && p->touch_down &&
            p->padstate >= PIU_PADSTATE_PEN_DATA && !p->data_armed)
            arm_sample(p, now_ns, interval_delay_us(p));
        return;
    }

    p->regs[offset / 4] = value;
}

uint16_t piu_read(piu_state_t *p, uint32_t offset, uint64_t now_ns)
{
    unsigned page;

    if (!p)
        return 0;
    piu_tick(p, now_ns);

    if (offset == 0x00) {
        return (uint16_t)((p->regs[0] & 0x03FFu) |
            ((p->padstate & 7u) << 10) |
            (((p->regs[1] & PIU_PENDING_PENCHG) ?
                p->penstc : p->touch_down) ? 0x2000u : 0));
    }
    if (offset == 0x04)
        return p->regs[1];
    if (offset <= 0x18 && (offset & 3u) == 0)
        return p->regs[offset / 4];
    if (!((offset >= 0x20 && offset <= 0x2C) ||
        (offset >= 0x50 && offset <= 0x5C)))
        return 0;

    page = offset >= 0x50 ? 1u : 0u;
    if (p->page_valid[page]) {
        switch (offset & 0x0Fu) {
        case 0x00: return p->page_adc[page][0];
        case 0x04: return p->page_adc[page][1];
        case 0x08: return p->page_adc[page][2];
        case 0x0C: return p->page_adc[page][3];
        }
    } else {
        switch (offset & 0x0Fu) {
        case 0x00: return 0x81E1u;
        case 0x04: return 0x8E30u;
        case 0x08: return 0x8300u;
        case 0x0C: return 0x8D1Bu;
        }
    }
    return 0;
}

bool piu_irq_pending(const piu_state_t *p)
{
    return p && (p->regs[1] & (p->regs[1] >> 8)) != 0;
}
