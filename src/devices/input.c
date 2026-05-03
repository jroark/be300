/*
 *  src/devices/input.c — touchpanel and button input devices.
 *
 *  Touchpanel: VRC4173 PA 0x0A000300, size 0x60.
 *  Buttons:    VRC4173 PA 0x0A00A040, size 0x10.
 *
 *  Both share the be300_input_device struct and route through GIRQ0:
 *  GIU bit 8 -> GIRQ0 bit 0 (touch via PIUINTREG bit 9, buttons via bit 1).
 *
 *  Split out of src/be300_devices.c. Public API (declared in be300.h):
 *    be300_buttons_host_update, be300_touch_tick, be300_register_input.
 *  Internal callbacks consumed by the VRC4173 latch handler:
 *    piu_girq0_source_bits, piu_update_state,
 *    button_girq0_source_bits, button_ack_keyboard_source.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"

#include "be300.h"
#include "be300_probe.h"
#include "devices.h"

#include "devices_internal.h"

struct be300_input_device {
    machine_t *m;
    bool       log_mmio;
    /* Button device state. */
    bool       button_irq_pending;
    struct interrupt button_irq;  /* BE-300 GIRQ0 cascaded interrupt */
    bool       button_irq_connected;
    bool       button_irq_asserted;
    /* PIU state (touch device only) */
    uint16_t   piu_regs[16];       /* register file: index = offset / 4 */
    uint16_t   piu_padstate;       /* PADSTATE(2:0) scan sequencer state */
    bool       piu_prev_touch;     /* previous touch_down for edge detect */
    bool       piu_penstc;         /* PENSTC latched while PENCHGINTR is set */
    bool       piu_queued_penchg;  /* one contact-state change after latched PENCHGINTR */
    bool       piu_queued_penstc;
    bool       piu_data_armed;     /* PenDataScan has a coordinate sample due */
    uint64_t   piu_clock_ns;       /* VRC4173 PIU peripheral timebase */
    uint64_t   piu_last_host_ns;
    uint64_t   piu_data_ready_ns;
    uint8_t    piu_next_page;      /* next coordinate page buffer interrupt */
    uint16_t   piu_adc[4];         /* y+, y-, x-, x+ */
    uint16_t   piu_page_adc[2][4]; /* page 0/page 1 coordinate buffers */
    bool       piu_page_valid[2];
    struct interrupt piu_irq;      /* BE-300 GIRQ0 cascaded interrupt */
    bool       piu_irq_connected;
    bool       piu_irq_asserted;
};

#define PIU_PENDING_PENCHG      0x01u
/* VRC4173 UM §9.7: PADDLOSTINTR is bit 2 in PIUINTREG. */
#define PIU_PENDING_DATALOST    0x04u
#define PIU_PENDING_PAGE0       0x08u
#define PIU_PENDING_PAGE1       0x10u
#define PIU_PENDING_TOUCH_DATA  (PIU_PENDING_PAGE0 | PIU_PENDING_PAGE1)
#define PIU_PENDING_COORDINATE  (PIU_PENDING_TOUCH_DATA | PIU_PENDING_DATALOST)
#define PIU_PENDING_TOUCH_GROUP 0x5Cu
#define PIU_PENDING_ANY         (PIU_PENDING_PENCHG | PIU_PENDING_TOUCH_GROUP)
#define TOUCH_PANEL_W           240u
#define TOUCH_PANEL_H           (319u + 40u)
#define BUTTON_POWER_BIT        0x80u
/* Real-hardware quiescent PIU timing: docs/hardware/hw_dump_vrc4173.txt:983. */
#define PIU_DEFAULT_SCAN_INTERVAL   0x05DCu
#define PIU_DEFAULT_STABLE          0x00C8u

/*
 *  PIU interrupt helper — idempotent assert/deassert through BE-300's
 *  cascaded GIU interrupt route.  The Linux4BE hardware notes describe
 *  touch as SYSINT1 bit 8 (GIU) -> GIUINTLREG bit 0 (GIRQ0) ->
 *  VRC4173 offset 0x0004 bit 9 (GIRQ0-9), then PIUINTREG at 0x0A000304.
 *  See docs/hardware/hardware.txt:15-19, :65-89, and :132-145.
 */
static void piu_irq_update(struct be300_input_device *d, bool want)
{
    if (!d->piu_irq_connected)
        return;
    if (want && !d->piu_irq_asserted) {
        INTERRUPT_ASSERT(d->piu_irq);
        d->piu_irq_asserted = true;
    } else if (!want && d->piu_irq_asserted) {
        INTERRUPT_DEASSERT(d->piu_irq);
        d->piu_irq_asserted = false;
    }
}

static void button_irq_update(struct be300_input_device *d, bool want)
{
    if (!d || !d->button_irq_connected)
        return;
    if (want && !d->button_irq_asserted) {
        INTERRUPT_ASSERT(d->button_irq);
        d->button_irq_asserted = true;
    } else if (!want && d->button_irq_asserted) {
        INTERRUPT_DEASSERT(d->button_irq);
        d->button_irq_asserted = false;
    }
}

uint32_t button_girq0_source_bits(const struct be300_input_device *d)
{
    /*
     * docs/hardware/hardware.txt:107-112 identifies GIRQ0-1 as the
     * keyboard SYSINTR source, and the real hardware idle dump keeps the
     * button block at 0x0A00A040 distinct from the generic NAND/latch
     * range (docs/hardware/hw_dump_vrc4173.txt:520).  Host button state
     * changes latch this cascaded source until the OAL writes the GIRQ0-1
     * acknowledge path at AA000014.
     */
    return (d && d->button_irq_pending) ? BUTTON_GIRQ0_SOURCE : 0;
}

void button_ack_keyboard_source(struct be300_input_device *d)
{
    if (!d || !d->button_irq_pending)
        return;

    d->button_irq_pending = false;
    button_irq_update(d, false);
}

static bool button_change_is_power_release_only(uint8_t old_set1,
    uint8_t old_set2, uint8_t new_set1, uint8_t new_set2)
{
    return old_set1 == new_set1 &&
        ((old_set2 ^ new_set2) == BUTTON_POWER_BIT) &&
        (old_set2 & BUTTON_POWER_BIT) != 0 &&
        (new_set2 & BUTTON_POWER_BIT) == 0;
}

void be300_buttons_host_update(machine_t *m, uint8_t old_set1,
    uint8_t old_set2)
{
    struct be300_input_device *d;

    if (!m || !m->button_device)
        return;
    if (m->btn_set1 == old_set1 && m->btn_set2 == old_set2)
        return;

    d = (struct be300_input_device *)m->button_device;
    /*
     * The stock OAL has a separate SYSINT1 power-switch source
     * (docs/hardware/hardware.txt:15-18) and the keyboard source is used to
     * sample the active button bitmap.  Treating the power release as another
     * keyboard interrupt wakes SUSPEND immediately after the press powered the
     * guest down.  The live bit still clears; only the release edge is not a
     * keyboard wake source.
     */
    if (button_change_is_power_release_only(old_set1, old_set2,
        m->btn_set1, m->btn_set2))
        return;

    d->button_irq_pending = true;
    button_irq_update(d, true);
}

uint32_t piu_girq0_source_bits(const struct be300_input_device *d)
{
    if (!d)
        return 0;

    return (d->piu_regs[1] & (d->piu_regs[1] >> 8)) ? 0x00000200u : 0;
}

static void piu_refresh_irq(struct be300_input_device *d)
{
    piu_irq_update(d, (d->piu_regs[1] & (d->piu_regs[1] >> 8)) != 0);
}

static void piu_raise_penchg(struct be300_input_device *d, bool down)
{
    if (!d)
        return;

    /*
     * VRC4173 UM §9.3.1: PENSTC records the touch-panel state at the time
     * PENCHGINTR is set, and it remains latched until PENCHGINTR is
     * cleared. Returning the live host pen state here makes short taps
     * disappear if WinCE services the interrupt after the host button has
     * already lifted.
     */
    if (d->piu_regs[1] & PIU_PENDING_PENCHG) {
        /*
         * PENSTC deliberately does not change while PENCHGINTR is pending.
         * Preserve one later edge and expose it after the guest W1C, so a
         * short down/up host tap cannot collapse into a permanent down.
         */
        d->piu_queued_penchg = true;
        d->piu_queued_penstc = down;
        return;
    }

    d->piu_penstc = down;
    d->piu_regs[1] |= PIU_PENDING_PENCHG;
}

static void piu_latch_sample(struct be300_input_device *d)
{
    uint32_t x, y;

    if (!d || !d->m)
        return;

    x = d->m->touch_x;
    y = d->m->touch_y;
    if (x >= TOUCH_PANEL_W)
        x = TOUCH_PANEL_W - 1u;
    if (y >= TOUCH_PANEL_H)
        y = TOUCH_PANEL_H - 1u;

    d->piu_adc[0] = (uint16_t)(0x81E1u +
        (y * (0x8E30u - 0x81E1u)) / TOUCH_PANEL_H);
    d->piu_adc[1] = (uint16_t)(0x8E30u -
        (y * (0x8E30u - 0x81E1u)) / TOUCH_PANEL_H);
    d->piu_adc[2] = (uint16_t)(0x8D1Bu -
        (x * (0x8D1Bu - 0x8300u)) / (TOUCH_PANEL_W - 1u));
    d->piu_adc[3] = (uint16_t)(0x8300u +
        (x * (0x8D1Bu - 0x8300u)) / (TOUCH_PANEL_W - 1u));
}

static void piu_store_page_sample(struct be300_input_device *d, unsigned page)
{
    if (!d || page >= 2)
        return;

    memcpy(d->piu_page_adc[page], d->piu_adc, sizeof(d->piu_adc));
    d->piu_page_valid[page] = true;
}

static bool piu_coordinate_page_available(const struct be300_input_device *d,
    unsigned page)
{
    uint16_t page_bit;

    if (!d || page >= 2)
        return false;

    page_bit = page ? PIU_PENDING_PAGE1 : PIU_PENDING_PAGE0;
    return !(d->piu_regs[1] & page_bit) && !d->piu_page_valid[page];
}

static void piu_clear_coordinate_state(struct be300_input_device *d)
{
    if (!d)
        return;

    /*
     * VRC4173 UM §9.2 models pen contact as a state-machine transition
     * from WaitPenTouch into PenDataScan/IntervalNextScan and back again.
     * Coordinate page buffers belong to that scan session; carrying old
     * page-valid/data-lost state into the next pen session can suppress the
     * next page or pen-change interrupt and was observed as missed taps.
     */
    d->piu_regs[1] &= (uint16_t)~PIU_PENDING_COORDINATE;
    memset(d->piu_page_valid, 0, sizeof(d->piu_page_valid));
    d->piu_data_armed = false;
    d->piu_data_ready_ns = 0;
    d->piu_next_page = 0;
}

static void piu_stop_coordinate_scan(struct be300_input_device *d)
{
    if (!d)
        return;

    /*
     * VRC4173 UM §9.2 moves IntervalNextScan back to WaitPenTouch on
     * release, but §9.3.6 keeps page-buffer VALID set until the matching
     * PADPAGE interrupt source is cleared. Do not discard unread coordinate
     * pages just because the pen lifted.
     */
    d->piu_data_armed = false;
    d->piu_data_ready_ns = 0;
}

static bool piu_wait_pen_touch_enabled(const struct be300_input_device *d)
{
    uint16_t ctl;

    if (!d)
        return false;

    ctl = d->piu_regs[0];
    return (ctl & 0x0004) && (ctl & 0x0100) && ((ctl >> 3) & 3) == 0;
}

static uint64_t piu_host_monotonic_ns(void)
{
    return be300_host_monotonic_ns();
}

static uint64_t piu_now_ns(struct be300_input_device *d)
{
    uint64_t host_ns;

    if (!d)
        return 0;

    /*
     * VRC4173 UM sections 9.3.3 and 9.3.4 define PIU scan/stabilization
     * intervals in peripheral-clock-derived microseconds. They are not
     * driven by the VR4131 CP0 Count register, so tying PIU conversion
     * completion to CP0 Count makes touch sampling stall while WinCE idles
     * in WAIT/SUSPEND paths.
     */
    host_ns = piu_host_monotonic_ns();
    if (d->piu_last_host_ns == 0) {
        d->piu_last_host_ns = host_ns;
        return d->piu_clock_ns;
    }

    if (host_ns > d->piu_last_host_ns)
        d->piu_clock_ns += host_ns - d->piu_last_host_ns;
    d->piu_last_host_ns = host_ns;
    return d->piu_clock_ns;
}

static uint64_t piu_delay_ns(uint64_t delay_us)
{
    return delay_us * 1000ULL;
}

static bool piu_time_reached(uint64_t now_ns, uint64_t due_ns)
{
    return (int64_t)(now_ns - due_ns) >= 0;
}

static uint64_t piu_conversion_delay_us(const struct be300_input_device *d)
{
    uint64_t stable_units = d ? (uint64_t)(d->piu_regs[3] & 0x003Fu) :
        (PIU_DEFAULT_STABLE & 0x003Fu);
    uint64_t conversions = d && (d->piu_regs[0] & 0x0020u) ? 5u : 4u;

    /*
     * VRC4173 UM sections 9.2, 9.3.4, and Figure 9-5: PenDataScan waits
     * the PIUSTBLREG STABLE(5:0) stabilization interval in 30 us units,
     * then performs four coordinate A/D conversions at about 10 us each.
     * PADSCANTYPE adds a pressure conversion.
     */
    return stable_units * 30ULL + conversions * 10ULL;
}

static uint64_t piu_interval_delay_us(const struct be300_input_device *d)
{
    uint64_t scan_units = d ? (uint64_t)(d->piu_regs[2] & 0x07FFu) :
        PIU_DEFAULT_SCAN_INTERVAL;

    /*
     * VRC4173 UM section 9.3.3: PIUSIVLREG's SCANINTVAL(10:0) is the
     * interval between coordinate pairs in 30 us units. The next interrupt
     * occurs after that interval and the following PenDataScan conversion.
     */
    return scan_units * 30ULL + piu_conversion_delay_us(d);
}

static void piu_arm_coordinate_sample(struct be300_input_device *d,
    uint64_t delay_us)
{
    d->piu_data_armed = true;
    d->piu_data_ready_ns = piu_now_ns(d) + piu_delay_ns(delay_us);
}

static void piu_arm_coordinate_sample_from(struct be300_input_device *d,
    uint64_t now_ns, uint64_t delay_us)
{
    d->piu_data_armed = true;
    d->piu_data_ready_ns = now_ns + piu_delay_ns(delay_us);
}

static void piu_signal_pen_down(struct be300_input_device *d)
{
    if (!d)
        return;

    piu_clear_coordinate_state(d);
    if (d->piu_padstate == 4 && (d->piu_regs[0] & 0x100)) {
        d->piu_padstate = 5;
        piu_arm_coordinate_sample(d, piu_conversion_delay_us(d));
    }

    piu_latch_sample(d);
    piu_raise_penchg(d, true);
    piu_refresh_irq(d);
}

static void piu_signal_pen_up(struct be300_input_device *d)
{
    bool wait_enabled;
    bool should_signal;

    if (!d)
        return;

    wait_enabled = piu_wait_pen_touch_enabled(d);
    should_signal = wait_enabled || d->piu_padstate >= 4;
    piu_stop_coordinate_scan(d);
    if (wait_enabled)
        d->piu_padstate = 4;
    else if (d->piu_padstate > 1)
        d->piu_padstate = 1;
    if (!should_signal) {
        piu_refresh_irq(d);
        return;
    }
    piu_raise_penchg(d, false);
    piu_refresh_irq(d);
}

static int piu_select_coordinate_page(struct be300_input_device *d)
{
    unsigned page = d->piu_next_page & 1u;
    unsigned other = page ^ 1u;

    if (piu_coordinate_page_available(d, page))
        return (int)page;
    if (piu_coordinate_page_available(d, other))
        return (int)other;

    return -1;
}

static void piu_queue_coordinate_sample(struct be300_input_device *d)
{
    uint16_t page_bit;
    uint64_t now_ns;
    int page;

    /*
     * VRC4173 UM section 9.2: with PADATSTART active, WaitPenTouch
     * moves to PenDataScan on contact, samples four coordinate values,
     * and then raises page-buffer interrupts. touch.dll consumes three
     * repeated page samples before accepting a stable calibration
     * coordinate.
     */
    if (!d || !d->m || !d->m->touch_down)
        return;
    if (d->piu_padstate < 5 || !d->piu_data_armed)
        return;
    now_ns = piu_now_ns(d);
    if (!piu_time_reached(now_ns, d->piu_data_ready_ns))
        return;
    /*
     * VRC4173 UM sections 9.1 and 9.3.2: coordinate data is stored in two
     * page buffers, with independent page 0/page 1 valid interrupts. Do not
     * block a fresh conversion merely because the other page is still pending.
     */
    page = piu_select_coordinate_page(d);
    if (page < 0) {
        d->piu_regs[1] |= PIU_PENDING_DATALOST;
        d->piu_padstate = 1;
        d->piu_data_armed = false;
        d->piu_data_ready_ns = 0;
        piu_refresh_irq(d);
        return;
    }

    piu_latch_sample(d);
    piu_store_page_sample(d, (unsigned)page);
    d->piu_padstate = 5;
    d->piu_next_page = (uint8_t)(((unsigned)page ^ 1u) & 1u);
    page_bit = page ? PIU_PENDING_PAGE1 : PIU_PENDING_PAGE0;
    d->piu_regs[1] |= page_bit;
    if (d->m->touch_down) {
        d->piu_padstate = 6;  /* IntervalNextScan after one coordinate pair. */
        piu_arm_coordinate_sample_from(d, now_ns, piu_interval_delay_us(d));
    } else {
        d->piu_data_armed = false;
        d->piu_data_ready_ns = 0;
    }
    piu_refresh_irq(d);
}

static void piu_maybe_queue_coordinate_sample(struct be300_input_device *d)
{
    if (d && d->m && d->m->touch_down)
        piu_queue_coordinate_sample(d);
}

static void piu_reset_regs(struct be300_input_device *d)
{
    memset(d->piu_regs, 0, sizeof(d->piu_regs));
    d->piu_regs[2] = PIU_DEFAULT_SCAN_INTERVAL;
    d->piu_regs[3] = PIU_DEFAULT_STABLE;
    memset(d->piu_page_adc, 0, sizeof(d->piu_page_adc));
    memset(d->piu_page_valid, 0, sizeof(d->piu_page_valid));
    d->piu_queued_penchg = false;
    d->piu_queued_penstc = false;
}

/*
 *  PIU scan sequencer state update after PIUCNTREG write.
 *  Follows the state transition diagram in VRC4173 manual Figure 9-4.
 *
 *  The BE-300 touch panel control block is at PA 0x0A000300. Linux4BE
 *  notes `TouchPanel (0xaa000300)` and the real VRC4173 dump shows the
 *  live control/status word at 0x0A000300 (`docs/hardware/hardware.txt:225`,
 *  `docs/hardware/hw_dump_vrc4173.txt:983`).
 */
void piu_update_state(struct be300_input_device *d)
{
    uint16_t ctl = d->piu_regs[0];

    if (ctl & 0x0001) {
        /* PADRST: reset everything */
        piu_reset_regs(d);
        d->piu_padstate = 0;  /* Disable */
        d->piu_prev_touch = false;
        d->piu_penstc = false;
        d->piu_data_armed = false;
        d->piu_data_ready_ns = 0;
        d->piu_next_page = 0;
        piu_irq_update(d, false);
        return;
    }

    /*
     * The touch.dll init sequence stores 0x0304 to 0x0A000300 and real
     * hardware reads back 0x1304 there, i.e. PADSTATE=4 with bit 1 clear.
     * Do not gate this BE-300 touch block on the VRC4173 core PIUPWR bit.
     */
    if (d->piu_padstate == 0 && ctl != 0)
        d->piu_padstate = 1;  /* Disable → Standby */

    if (piu_wait_pen_touch_enabled(d)) {
        /* PIUSEQEN=1, PADATSTART=1, PIUMODE=00 → WaitPenTouch */
        if (d->piu_padstate < 4)
            d->piu_padstate = 4;  /* → WaitPenTouch */

        /* If pen is already down, transition immediately */
        if (d->m->touch_down && d->piu_padstate == 4)
            piu_signal_pen_down(d);
    } else if (!(ctl & 0x0004)) {
        /* PIUSEQEN=0 → back to Standby */
        if (d->piu_padstate > 1) {
            piu_clear_coordinate_state(d);
            d->piu_padstate = 1;
            piu_irq_update(d, false);
        }
    }
}

/*
 *  Touchpanel device — PA 0x0A000300, size 0x60
 *
 *  Register layout from docs/hardware/hardware.txt ISR analysis (GIRQ0-9):
 *
 *  +0x00 PIUCNTREG: bit 13 (0x2000) = pendown status
 *  +0x04 PIUINTREG: byte 0 (bits 7:0) = pending status (set by HW, W1C)
 *                   byte 1 (bits 15:8) = interrupt mask (R/W by driver)
 *                   ISR does: intr = W[304]; intr &= (intr >> 8);
 *                   Active bits: 0x5C -> SYSINTR_TOUCH, 0x01 -> SYSINTR_TOUCH_CHANGED
 *                   OAL masks delivered bits in the high byte; touch.dll
 *                   clears the low-byte pending bits with W1C writes.
 *  +0x20-0x2C, +0x50-0x5C: ADC coordinate buffers (y+, y-, x-, x+)
 */
DEVICE_ACCESS(be300_touch)
{
    struct be300_input_device *d = (struct be300_input_device *)extra;
    machine_t *m = d->m;
    uint32_t off = (uint32_t)relative_addr;
    uint64_t val;

    if (d->log_mmio)
        be300_log_mmio("be300_touch", writeflag, off, (unsigned)len,
            data, (uint32_t)cpu->pc);

    be300_probe_note_mmio("vrc4173-piu-touch", off,
        writeflag == MEM_WRITE ? 'W' : 'R',
        (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
        BE300_MMIO_CLASS_KNOWN);

    if (writeflag == MEM_WRITE) {
        val = memory_readmax64(cpu, data, len);

        if (off <= 0x18 && (off & 3) == 0) {
            uint32_t idx = off / 4;
            if (off == 0x00) {
                /* PIUCNTREG: store writable bits 9:0, except bit 0
                 * which is a self-clearing command/start bit.
                 * Real-hw dump `hw_dump_vrc4173.txt` shows
                 * `0x0A000300: 00001304` — bit 0 always reads 0.
                 * touch.dll at UM 0x01A412E0..0x01A412EC spins on
                 * `andi $9, $8, 1; bnel $9, $0, -2` (wait-for-clear)
                 * after writing 0x0401 to this register; the write
                 * is a command kick and hardware auto-clears bit 0
                 * synchronously. Pass 25 (2026-04-19): without this
                 * mask the driver polls at ~370k reads/sec forever. */
                d->piu_regs[0] = (uint16_t)(val & 0x03FE);
                piu_update_state(d);
            } else if (off == 0x04) {
                uint16_t old_pending = d->piu_regs[1] & 0xFFu;
                uint16_t new_mask = (uint16_t)((val >> 8) & 0xFFu);
                uint16_t pending_ack = (uint16_t)(val & 0xFFu);
                bool data_acked = (old_pending & pending_ack &
                    (PIU_PENDING_TOUCH_DATA | PIU_PENDING_DATALOST)) != 0;
                bool lost_acked = (old_pending & pending_ack &
                    PIU_PENDING_DATALOST) != 0;

                /* PIUINTREG: mask/status byte pair.
                 * Byte 0 (bits 7:0) = pending status (set by HW, W1C)
                 * Byte 1 (bits 15:8) = interrupt mask (writable)
                 * docs/hardware/hardware.txt:132-145 shows the ISR
                 * masks delivered GIRQ0-9 sources by clearing high-byte
                 * bits before returning SYSINTR. touch.dll later ACKs
                 * pending status with read/OR/write W1C stores: bit 0 at
                 * 0x01A41C6C..0x01A41C78 and page bits 3/4 at
                 * 0x01A42000..0x01A42008. */
                d->piu_regs[1] &= (uint16_t)~
                    (pending_ack & PIU_PENDING_ANY);
                d->piu_regs[1] = (d->piu_regs[1] & 0x00FF)
                    | (uint16_t)(new_mask << 8);
                if (pending_ack & PIU_PENDING_PAGE0)
                    d->piu_page_valid[0] = false;
                if (pending_ack & PIU_PENDING_PAGE1)
                    d->piu_page_valid[1] = false;
                if (pending_ack & PIU_PENDING_PENCHG) {
                    if (d->piu_queued_penchg) {
                        d->piu_penstc = d->piu_queued_penstc;
                        d->piu_queued_penchg = false;
                        d->piu_regs[1] |= PIU_PENDING_PENCHG;
                    } else {
                        d->piu_penstc = m->touch_down;
                    }
                }
                if (lost_acked && m->touch_down &&
                    piu_wait_pen_touch_enabled(d) &&
                    (d->piu_regs[1] & PIU_PENDING_COORDINATE) == 0) {
                    d->piu_padstate = 5;
                    piu_arm_coordinate_sample(d, piu_conversion_delay_us(d));
                }
                if (data_acked && m->touch_down && d->piu_padstate >= 5 &&
                    !d->piu_data_armed)
                    piu_arm_coordinate_sample(d, piu_interval_delay_us(d));
                piu_refresh_irq(d);
            } else {
                d->piu_regs[idx] = (uint16_t)val;
            }
        }
        return 1;
    }

    /* Read path */
    val = 0;

    if (off == 0x00) {
        piu_maybe_queue_coordinate_sample(d);
        /* PIUCNTREG: compose from stored bits + dynamic state */
        val = (d->piu_regs[0] & 0x03FFu)
            | ((uint16_t)(d->piu_padstate & 7) << 10)
            | ((d->piu_regs[1] & PIU_PENDING_PENCHG ?
                d->piu_penstc : m->touch_down) ? 0x2000u : 0);
    } else if (off == 0x04) {
        piu_maybe_queue_coordinate_sample(d);
        /* Return (mask << 8) | pending; ISR does pending & mask */
        val = d->piu_regs[1];
    } else if (off <= 0x18 && (off & 3) == 0) {
        piu_maybe_queue_coordinate_sample(d);
        val = d->piu_regs[off / 4];
    } else if ((off >= 0x20 && off <= 0x2C) || (off >= 0x50 && off <= 0x5C)) {
        unsigned page = off >= 0x50 ? 1u : 0u;
        piu_maybe_queue_coordinate_sample(d);
        /* ADC buffer registers — return the requested page's conversion.
         * docs/hardware/hardware.txt:225-235 identifies these registers
         * as touch-panel y+/y-/x-/x+ sample buffers; VRC4173 UM section
         * 9.3.2 exposes page 0 and page 1 as distinct valid buffers. */
        if (d->piu_page_valid[page]) {
            switch (off & 0x0F) {
            case 0x00: val = d->piu_page_adc[page][0]; break;
            case 0x04: val = d->piu_page_adc[page][1]; break;
            case 0x08: val = d->piu_page_adc[page][2]; break;
            case 0x0C: val = d->piu_page_adc[page][3]; break;
            }
        } else {
            switch (off & 0x0F) {
            case 0x00: val = 0x81E1u; break;
            case 0x04: val = 0x8E30u; break;
            case 0x08: val = 0x8300u; break;
            case 0x0C: val = 0x8D1Bu; break;
            }
        }
    }

    memory_writemax64(cpu, data, len, val);
    return 1;
}

/*
 *  Called once per BE-300 run-loop batch after host input is sampled to
 *  detect pen state changes and generate PIU interrupts when the scan
 *  sequencer is active.
 */
void be300_touch_tick(machine_t *m)
{
    struct be300_input_device *d;
    bool now, prev;

    if (!m || !m->touch_device)
        return;
    d = (struct be300_input_device *)m->touch_device;

    now = m->touch_down;
    prev = d->piu_prev_touch;
    d->piu_prev_touch = now;

    if (d->piu_padstate == 4 && now && !prev) {
        /* WaitPenTouch + pen down → PenDataScan */
        piu_signal_pen_down(d);
    } else if (!now && prev) {
        /* Pen release */
        piu_signal_pen_up(d);
    } else if (now) {
        piu_queue_coordinate_sample(d);
    }
}

/* Button register device — PA 0x0A00A040, size 0x10 */
DEVICE_ACCESS(be300_buttons)
{
    struct be300_input_device *d = (struct be300_input_device *)extra;
    machine_t *m = d->m;
    uint8_t reg[0x10] = { 0 };
    uint64_t val = 0;

    if (d->log_mmio)
        be300_log_mmio("be300_buttons", writeflag, (uint32_t)relative_addr,
            (unsigned)len, data, (uint32_t)cpu->pc);

    be300_probe_note_mmio("vrc4173-buttons", (uint32_t)relative_addr,
        writeflag == MEM_WRITE ? 'W' : 'R',
        (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
        BE300_MMIO_CLASS_KNOWN);

    if (writeflag == MEM_WRITE)
        return 1;

    /*
     * Real-hardware idle dump:
     *   docs/hardware/hw_dump_vrc4173.txt:520
     *   0x0A00A040: 00009EFF ...
     * Keep the low halfword at its quiescent value.  The stock WinCE
     * keybddr.dll initializes that halfword to 0x9EFF, then its interrupt
     * path performs a dummy lhu at 0x0A00A042 and samples the active-high
     * button bitmap at 0x0A00A044 masked by 0x9EFF
     * (keybddr.dll PCs 0x01A32460 and 0x01A3411C-0x01A34128).
     */
    reg[0x00] = 0xffu;
    reg[0x01] = 0x9eu;
    reg[0x04] = m->btn_set1;
    reg[0x05] = m->btn_set2;

    if ((uint32_t)relative_addr >= sizeof(reg))
        val = 0;
    else {
        uint32_t off = (uint32_t)relative_addr;
        unsigned n = (unsigned)len;
        if (n > 8)
            n = 8;
        for (unsigned i = 0; i < n && off + i < sizeof(reg); i++)
            val |= (uint64_t)reg[off + i] << (8u * i);
    }

    memory_writemax64(cpu, data, len, val);
    return 1;
}

/*
 *  be300_register_input():
 *
 *  Register touch and button input devices.  Called for all boot modes
 *  after any latch/NAND device registration (address ranges are pre-carved).
 */
void be300_register_input(struct machine *gxm, machine_t *m, bool log_mmio)
{
    struct be300_input_device *touch_d, *btn_d;

    CHECK_ALLOCATION(touch_d = calloc(1, sizeof(struct be300_input_device)));
    touch_d->m = m;
    touch_d->log_mmio = log_mmio;
    piu_reset_regs(touch_d);

    /* Connect BE-300 touch through the cascaded GIU route documented in
     * docs/hardware/hardware.txt:15-19, :65-89, and :132-145:
     * SYSINT1 bit 8 (GIU) -> GIUINTLREG bit 0 (GIRQ0) ->
     * VRC4173 offset 0x0004 bit 9 (GIRQ0-9) -> PIUINTREG. */
    {
        char tmps[300];
        snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i.giu.%i",
            gxm->path, gxm->bootstrap_cpu, 8, 0);
        INTERRUPT_CONNECT(tmps, touch_d->piu_irq);
        touch_d->piu_irq_connected = true;
        touch_d->piu_irq_asserted = false;
    }

    m->touch_device = touch_d;
    g_be300_machine = m;

    memory_device_register(gxm->memory, "be300_touch",
        0x0A000300ULL, 0x60,
        dev_be300_touch_access, (void *)touch_d, DM_DEFAULT, NULL);

    CHECK_ALLOCATION(btn_d = calloc(1, sizeof(struct be300_input_device)));
    btn_d->m = m;
    btn_d->log_mmio = log_mmio;
    /* docs/hardware/hardware.txt:65-112 routes keyboard buttons as
     * SYSINT1 bit 8 (GIU) -> GIUINTLREG bit 0 (GIRQ0) ->
     * VRC4173 offset 0x0004 bit 1 (GIRQ0-1). */
    {
        char tmps[300];
        snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i.giu.%i",
            gxm->path, gxm->bootstrap_cpu, 8, 0);
        INTERRUPT_CONNECT(tmps, btn_d->button_irq);
        btn_d->button_irq_connected = true;
        btn_d->button_irq_asserted = false;
    }
    m->button_device = btn_d;
    memory_device_register(gxm->memory, "be300_buttons",
        0x0A00A040ULL, 0x10,
        dev_be300_buttons_access, (void *)btn_d, DM_DEFAULT, NULL);
}
