/*
 *  src/devices/serial_dock.c — the serial dock seen through the VRC4173
 *  latch: the CommMode status/socket/modem-event registers, the PC Connect
 *  cable insert sequence and the Stowaway keyboard's GIRQ0-4 wake, plus
 *  their entries for be300_vrc4173_regions[]. Split out of
 *  src/be300_devices.c.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "cpu.h"

#include "devices_internal.h"
#include "vrc4173_latch.h"
#include "host_time.h"
#include "pcconnect.h"
#include "stowaway.h"

#define BE300_NS_PER_MS 1000000ULL

#define BE300_GIRQ0_COMMMODE       0x00000010u
#define BE300_COMMMODE_STATUS_OFF  0x8004u
#define BE300_COMMMODE_SOCKET_OFF  0x8010u
#define BE300_COMMMODE_MODEM_EVENT_OFF 0x1054u
#define BE300_COMMMODE_SERIAL_SOCKET_OFF 0x1010u
#define BE300_COMMSIU_CTRL_OFF     0x8684u
#define BE300_PCCARD_STATUS_OFF    0x1B50u
#define BE300_PCCARD_SOCKET_READY  0x00000008u
#define BE300_COMMMODE_SOCKET_PENDING 0x0001u
#define BE300_COMMMODE_MODEM_PENDING  0x0010u
#define BE300_COMMMODE_PENDING_MASK \
    (BE300_COMMMODE_SOCKET_PENDING | BE300_COMMMODE_MODEM_PENDING)
#define BE300_COMMMODE_SOCKET_IRQ_MASK 0x0100u
#define BE300_COMMMODE_MODEM_IRQ_MASK  0x1000u
#define BE300_COMMMODE_IRQ_MASK \
    (BE300_COMMMODE_SOCKET_IRQ_MASK | BE300_COMMMODE_MODEM_IRQ_MASK)
#define BE300_COMMMODE_SOCKET_VALUE_MASK 0x001Fu
#define BE300_COMMMODE_SOCKET_NONE  0x0007u
#define BE300_COMMMODE_SOCKET_RS232 0x0008u
#define BE300_COMMMODE_SOCKET_USB_SYNC 0x000Cu
#define BE300_COMMMODE_MODEM_EVENT_BITS 0x0030u
#define BE300_COMMSIU_CTRL_RS232   0x0008u
#define BE300_PCC_CONNECT_DELAY_DEFAULT_MS 1000u

static bool be300_pcconnect_cable_enabled(void)
{
    return g_be300_machine && g_be300_machine->cfg.pcconnect_bridge != NULL;
}

static bool be300_stowaway_keyboard_enabled(void)
{
    return g_be300_machine &&
        g_be300_machine->cfg.enable_stowaway_keyboard;
}

static bool be300_serial_dock_socket_enabled(void)
{
    return be300_pcconnect_cable_enabled() ||
        be300_stowaway_keyboard_enabled();
}

static bool be300_serial_dock_socket_connected(
    const struct be300_vrc4173_latch *d)
{
    if (be300_serial_dock_socket_enabled())
        return d && d->pcconnect_dock_connected;

    return false;
}

static bool be300_commmode_interrupt_enabled(void)
{
    return be300_pcconnect_cable_enabled() ||
        be300_stowaway_keyboard_enabled();
}

static uint16_t be300_serial_dock_socket_value(void)
{
    if (be300_pcconnect_cable_enabled() &&
        g_be300_machine->cfg.pcconnect_dock == BE300_PCC_DOCK_USB_SYNC)
        return BE300_COMMMODE_SOCKET_USB_SYNC;

    return BE300_COMMMODE_SOCKET_RS232;
}

uint32_t be300_pcconnect_connect_delay_ms(void)
{
    const char *v = getenv("BE300_PCC_CONNECT_DELAY_MS");
    char *end = NULL;
    unsigned long n;

    if (!v || !*v)
        return BE300_PCC_CONNECT_DELAY_DEFAULT_MS;

    n = strtoul(v, &end, 10);
    if (end == v || *end != '\0' || n > 600000ul)
        return BE300_PCC_CONNECT_DELAY_DEFAULT_MS;

    return (uint32_t)n;
}

static bool be300_pcconnect_time_reached(uint64_t now_ns, uint64_t due_ns)
{
    return (int64_t)(now_ns - due_ns) >= 0;
}

static bool be300_pcconnect_insert_ready(
    const struct be300_vrc4173_latch *d)
{
    if (!d || !be300_serial_dock_socket_enabled() ||
        !d->pcconnect_insert_armed)
        return false;

    return be300_pcconnect_time_reached(be300_host_monotonic_ns(),
        d->pcconnect_insert_deadline_ns);
}

static uint16_t be300_pcconnect_commmode_raw(
    const struct be300_vrc4173_latch *d)
{
    if (!d)
        return 0;

    return (uint16_t)d->bytes[BE300_COMMMODE_STATUS_OFF]
      | ((uint16_t)d->bytes[BE300_COMMMODE_STATUS_OFF + 1u] << 8);
}

static uint16_t be300_pcconnect_commmode_read(
    const struct be300_vrc4173_latch *d);
static uint16_t be300_pcconnect_socket_read(
    const struct be300_vrc4173_latch *d);

static bool be300_pcconnect_trace_enabled(void)
{
    const char *v = getenv("BE300_PCC_TRACE");

    return v && *v && strcmp(v, "0") != 0;
}

void be300_pcconnect_trace(const struct be300_vrc4173_latch *d,
    const char *what, uint32_t off, uint32_t len, uint64_t val, uint32_t pc)
{
    if (!be300_pcconnect_trace_enabled())
        return;

    fprintf(stderr,
        "[PCC_DOCK_TRACE] %s off=0x%04x len=%u val=0x%04" PRIx64
        " pc=0x%08x raw=0x%04x read=0x%04x socket=0x%04x "
        "dock=%d pending=0x%02x armed=%d irq=%d\n",
        what, off, len, val, pc, be300_pcconnect_commmode_raw(d),
        d ? be300_pcconnect_commmode_read(d) : 0,
        d ? be300_pcconnect_socket_read(d) : 0,
        d ? d->pcconnect_dock_connected : 0,
        d ? d->pcconnect_commmode_pending : 0,
        d ? d->pcconnect_insert_armed : 0,
        d ? d->pcconnect_irq_asserted : 0);
}

static uint16_t be300_pcconnect_commmode_read(
    const struct be300_vrc4173_latch *d)
{
    uint16_t v = be300_pcconnect_commmode_raw(d);

    v &= (uint16_t)~BE300_COMMMODE_PENDING_MASK;
    v |= d->pcconnect_commmode_pending & BE300_COMMMODE_PENDING_MASK;
    return v;
}

static uint16_t be300_pcconnect_socket_read(
    const struct be300_vrc4173_latch *d)
{
    uint16_t v;

    if (!d)
        return 0;

    v = (uint16_t)d->bytes[BE300_COMMMODE_SOCKET_OFF]
      | ((uint16_t)d->bytes[BE300_COMMMODE_SOCKET_OFF + 1u] << 8);

    if (be300_serial_dock_socket_enabled()) {
        /*
         * socket.dll maps the Vic/CommMode page, then reads
         * ReadPortDataEx(0, 2, 0x1f) from AA008010.  The WinCE 3.0
         * socket table maps raw 0x0007 to an empty no-driver entry,
         * raw 0x0008 to serial.dll, and raw 0x000c/0x000d to USB/VCom
         * entries.  hardware.txt:88-102 documents AA008004 as the
         * CommMode GIRQ0-4 pending/mask register, and hardware.txt:189-191
         * places this page next to the companion SIU.  For PC Connect, do
         * not expose a socket until the emulated cable edge; after that
         * edge, expose either the RS-232 or USB/VCom socket identity while
         * keeping the same companion SIU byte path.  The Stowaway keyboard
         * dock is already physically inserted when --stowaway-keyboard is
         * selected, and its driver uses this same socket.dll path before
         * accepting the UART DCD level.  Preserve the other latched bits.
         */
        v &= (uint16_t)~BE300_COMMMODE_SOCKET_VALUE_MASK;
        v |= be300_serial_dock_socket_connected(d) ?
            be300_serial_dock_socket_value() : BE300_COMMMODE_SOCKET_NONE;
    }

    return v;
}

static uint16_t be300_serial_dock_commmode_event_read(
    const struct be300_vrc4173_latch *d)
{
    uint16_t v = be300_latch_peek_u16(d, BE300_COMMMODE_MODEM_EVENT_OFF);

    if (d && be300_serial_dock_socket_enabled())
        v |= d->stowaway_commmode_events;

    return v;
}

uint64_t be300_pcconnect_pccard_status_read(
    struct be300_vrc4173_latch *d, bool cf_attached, uint32_t off,
    unsigned len, uint64_t val)
{
    unsigned shift;

    if (!d || !be300_serial_dock_socket_enabled() ||
        cf_attached || !be300_serial_dock_socket_connected(d) ||
        !d->serial_dock_pccard_status_pending || len == 0 ||
        off > BE300_PCCARD_STATUS_OFF ||
        off + len <= BE300_PCCARD_STATUS_OFF)
        return val;

    /*
     * pcmcia.dll maps AA001B00 and waits for AA001B50 bit 3 after enabling
     * the socket path via AA000144 bit 5.  The 0x1000-0x1fff VRC4173 range
     * is currently backed by the CF companion model, so expose the serial
     * dock insert edge at the actual read boundary instead of seeding the
     * generic latch byte array.  Match cf.c's AA001B50 behavior: this is a
     * transition edge, not a held card-present level. Holding it asserted can
     * make later FATFS/card-unit probes see a phantom storage card during
     * PCConnect restore.
     */
    shift = (unsigned)((BE300_PCCARD_STATUS_OFF - off) * 8u);
    d->serial_dock_pccard_status_pending = false;
    return val | ((uint64_t)BE300_PCCARD_SOCKET_READY << shift);
}

static bool be300_pcconnect_commmode_unmasked(
    const struct be300_vrc4173_latch *d)
{
    uint16_t v = be300_pcconnect_commmode_read(d);

    return (v & (v >> 8) & BE300_COMMMODE_PENDING_MASK) != 0;
}

void be300_pcconnect_irq_update(struct be300_vrc4173_latch *d)
{
    bool want;

    if (!d || !d->pcconnect_irq_connected)
        return;

    want = be300_commmode_interrupt_enabled() &&
        be300_pcconnect_commmode_unmasked(d);
    if (want && !d->pcconnect_irq_asserted) {
        INTERRUPT_ASSERT(d->pcconnect_irq);
        d->pcconnect_irq_asserted = true;
    } else if (!want && d->pcconnect_irq_asserted) {
        INTERRUPT_DEASSERT(d->pcconnect_irq);
        d->pcconnect_irq_asserted = false;
    }
}

static void be300_pcconnect_irq_reedge(struct be300_vrc4173_latch *d)
{
    if (!d || !d->pcconnect_irq_connected || !d->pcconnect_irq_asserted)
        return;
    if (!be300_commmode_interrupt_enabled() ||
        !be300_pcconnect_commmode_unmasked(d))
        return;

    INTERRUPT_DEASSERT(d->pcconnect_irq);
    INTERRUPT_ASSERT(d->pcconnect_irq);
}

static void be300_pcconnect_arm_insert_after_reset(
    struct be300_vrc4173_latch *d)
{
    if (!d || !be300_serial_dock_socket_enabled())
        return;

    d->pcconnect_insert_armed = true;
    d->pcconnect_insert_deadline_ns = be300_host_monotonic_ns() +
        (uint64_t)d->pcconnect_insert_delay_ms * BE300_NS_PER_MS;
}

void be300_pcconnect_reset_for_cpu_reset(
    struct be300_vrc4173_latch *d)
{
    if (!d)
        return;

    if (be300_stowaway_keyboard_enabled()) {
        d->pcconnect_dock_connected = true;
        d->pcconnect_commmode_pending = 0;
        d->stowaway_commmode_events = 0;
        d->pcconnect_modem_after_socket_sent = false;
        d->pcconnect_rx_wake_count = 0;
        d->serial_dock_pccard_status_pending = true;
        d->pcconnect_insert_armed = false;
        stowaway_uart_reset();
        if (d->pcconnect_irq_connected && d->pcconnect_irq_asserted) {
            INTERRUPT_DEASSERT(d->pcconnect_irq);
            d->pcconnect_irq_asserted = false;
        }
        return;
    }

    if (!be300_pcconnect_cable_enabled())
        return;

    /*
     * KjCMU resets the CPU while the VRC4173-side latch state remains in
     * host memory.  Keep the guest-visible CommMode socket as "not docked"
     * and schedule a fresh insertion edge after the normal Boot.exe reset,
     * but do not drop the bridge's physical cable state once it has been
     * inserted.  hardware.txt:189-191 places Vic/CommMode and the SIU in
     * the companion dock path, and real-hardware PC Connect observation on
     * 2026-05-06 showed the docked serial connection stays active across
     * guest-side reset/restart until the BE-300 is physically removed from
     * the cradle or powered down; the host PC keeps polling the serial port
     * in the meantime.
     *
     * The delayed guest-visible edge avoids presenting an already-docked
     * CommMode state to the second-boot OAL, which takes the
     * software-shutdown/HIBERNATE path before user PC Connect monitors
     * exist.
     */
    d->pcconnect_dock_connected = false;
    d->pcconnect_commmode_pending = 0;
    d->stowaway_commmode_events = 0;
    d->pcconnect_modem_after_socket_sent = false;
    d->pcconnect_rx_wake_count = 0;
    d->serial_dock_pccard_status_pending = false;
    pcconnect_reset_guest_serial();
    usbf_reset(&d->usbf);
    be300_pcconnect_arm_insert_after_reset(d);

    if (d->pcconnect_irq_connected && d->pcconnect_irq_asserted) {
        INTERRUPT_DEASSERT(d->pcconnect_irq);
        d->pcconnect_irq_asserted = false;
    }
}

static void be300_pcconnect_raise_dock_edge(struct be300_vrc4173_latch *d,
    bool force)
{
    if (be300_stowaway_keyboard_enabled() && !force)
        return;

    if (!d || !be300_serial_dock_socket_enabled() ||
        (!force && !be300_pcconnect_insert_ready(d)) ||
        d->pcconnect_commmode_pending || d->pcconnect_dock_connected)
        return;

    /*
     * PC Connect is launched by the guest's cradle/RS-232 detection path,
     * not by serial bytes appearing spontaneously.  hardware.txt:74-130
     * documents the route as GIU0 -> AA000004 bit 4 -> AA008004
     * pending/mask bits.  NK disassembly at 0x800b6db4 dispatches
     * AA008004 sub-bit 0 through the CommMode socket SYSINTR, while
     * sub-bit 4 returns SYSINTR 0x23 for cedmbltin.dll's modem-warning
     * event thread.  The option models the
     * host plugging in the cable after the first Boot.exe reset and after
     * the guest has enabled the detect source, producing a real insertion
     * transition instead of a reset-time static level.
    */
    d->pcconnect_dock_connected = true;
    d->serial_dock_pccard_status_pending = true;
    d->pcconnect_commmode_pending |= BE300_COMMMODE_SOCKET_PENDING;
    if (be300_pcconnect_cable_enabled())
        pcconnect_set_cable_connected(true);
    if (be300_stowaway_keyboard_enabled()) {
        d->pcconnect_commmode_pending |= BE300_COMMMODE_MODEM_PENDING;
        d->stowaway_commmode_events |= BE300_COMMMODE_MODEM_EVENT_BITS;
    }
    be300_pcconnect_irq_update(d);
    be300_pcconnect_trace(d, force ? "dock-edge-uart" : "dock-edge",
        BE300_COMMMODE_STATUS_OFF, 2, be300_pcconnect_commmode_read(d), 0);
}

/*
 * Wake serial.dll's IST by raising a CommMode modem-pending event whenever the
 * Stowaway-attached ns16550 has data in its RX queue.  Real BE-300 routes the
 * companion SIU IRQ through GIU pin 0; the OAL dispatches by reading
 * AA000004 (level 1) and AA008004 (level 2 sub-bits) and only fires SYSINTR
 * 0x23 (serial.dll) when bit 4 is set in both.  ns16550 alone asserts the
 * GIU line but never touches those latches, so the OAL never dispatches —
 * the byte stays in our RX queue forever.  Synthesizing a modem event here
 * piggy-backs on the same dispatch path that the probe ACK already uses.
 */
void be300_stowaway_signal_uart_irq(int pending)
{
    struct be300_vrc4173_latch *d = g_be300_vrc4173_latch;
    if (!d || !be300_stowaway_keyboard_enabled())
        return;
    if (!pending)
        return;
    d->pcconnect_commmode_pending |= BE300_COMMMODE_MODEM_PENDING;
    d->stowaway_commmode_events |= BE300_COMMMODE_MODEM_EVENT_BITS;
    be300_pcconnect_irq_update(d);
    be300_pcconnect_irq_reedge(d);
}

static void be300_stowaway_raise_modem_event(struct be300_vrc4173_latch *d)
{
    if (!d || !be300_stowaway_keyboard_enabled())
        return;

    /*
     * The Stowaway dock is physically inserted for the whole boot, but
     * serial.dll does not accept the UART DCD level until its companion-side
     * modem-event path observes AA001054 bits 0x30 and AA001010 reads
     * connected. hardware.txt:189-191 identifies 0xaa001000 as a separate
     * companion block, and hw_dump_vrc4173.txt:39-42 shows real data at
     * 0x0A001010 and 0x0A001054. Raise only the modem sub-event here so the
     * socket can remain present while the DCD transition is delivered when
     * the COM driver enables modem-status interrupts.
     */
    d->pcconnect_dock_connected = true;
    d->pcconnect_commmode_pending |= BE300_COMMMODE_MODEM_PENDING;
    d->stowaway_commmode_events |= BE300_COMMMODE_MODEM_EVENT_BITS;
    be300_pcconnect_irq_update(d);
    be300_pcconnect_trace(d, "stowaway-modem-edge",
        BE300_COMMMODE_STATUS_OFF, 2, be300_pcconnect_commmode_read(d), 0);
    be300_pcconnect_irq_reedge(d);
}

static void be300_pcconnect_maybe_raise_dock_edge(
    struct be300_vrc4173_latch *d)
{
    if (!d)
        return;

    if (be300_stowaway_keyboard_enabled()) {
        if ((d->pcconnect_commmode_pending & BE300_COMMMODE_MODEM_PENDING)
            == 0 && d->stowaway_commmode_events == 0 &&
            stowaway_uart_take_modem_wait_request())
            be300_stowaway_raise_modem_event(d);
        return;
    }

    if (d->pcconnect_dock_connected)
        return;

    /*
     * The transparent bridge models a real dock insertion while the host PC
     * is already polling the serial port, so let the delayed insertion path
     * fire after the Boot.exe reset.
     */
    be300_pcconnect_raise_dock_edge(d, false);
}

static void be300_pcconnect_maybe_raise_uart_rx_level(
    struct be300_vrc4173_latch *d);
static void be300_stowaway_maybe_raise_uart_rx_level(
    struct be300_vrc4173_latch *d);

void be300_pcconnect_poll(void)
{
    if (be300_pcconnect_cable_enabled()) {
        be300_pcconnect_maybe_raise_dock_edge(g_be300_vrc4173_latch);
        be300_pcconnect_maybe_raise_uart_rx_level(g_be300_vrc4173_latch);
        return;
    }

    if (be300_stowaway_keyboard_enabled()) {
        be300_pcconnect_maybe_raise_dock_edge(g_be300_vrc4173_latch);
        be300_stowaway_maybe_raise_uart_rx_level(g_be300_vrc4173_latch);
        be300_pcconnect_irq_update(g_be300_vrc4173_latch);
        be300_pcconnect_irq_reedge(g_be300_vrc4173_latch);
    }
}

void be300_pcconnect_uart_irq_ready(void *opaque)
{
    struct be300_vrc4173_latch *d = opaque;

    if (!d || !be300_pcconnect_cable_enabled() ||
        !d->pcconnect_dock_connected)
        return;

    /*
     * The companion serial path is not the VR4131 internal SIU path:
     * hardware.txt:8 notes serial is handled by the custom companion, and
     * hardware.txt:122-130 routes the Vic/CommMode page through GIRQ0-4.
     * Raise a CommMode edge when the bridged companion SIU needs service
     * after the initial dock-detect edge.  This covers both host RX data and
     * TX-ready service: serial.dll sees the UART IIR only after the OAL
     * dispatches GIRQ0-4 sub-bit 4.
     *
     * Also raise MODEM_EVENT_BITS the way stowaway_signal_uart_irq does.
     * The bridge previously only set MODEM_PENDING, leaving the event
     * sub-bits clear — AtPcCnct.exe appears to need a real event flag
     * (DCD/CTS-change-style) to treat the IRQ as "data ready", not just
     * a generic modem interrupt. Stowaway has set both since day one
     * and works reliably; matching that behavior here.
    */
    d->pcconnect_commmode_pending |= BE300_COMMMODE_MODEM_PENDING;
    d->stowaway_commmode_events |= BE300_COMMMODE_MODEM_EVENT_BITS;
    be300_pcconnect_trace(d, "uart-irq-edge",
        BE300_COMMMODE_STATUS_OFF, 2,
        be300_pcconnect_commmode_read(d), 0);
    be300_pcconnect_irq_update(d);
    be300_pcconnect_irq_reedge(d);
}

static void be300_pcconnect_maybe_raise_uart_rx_level(
    struct be300_vrc4173_latch *d)
{
    size_t rx_count;

    if (!d || !be300_pcconnect_cable_enabled() ||
        !d->pcconnect_dock_connected)
        return;

    rx_count = pcconnect_uart_rx_count();
    if (rx_count == 0) {
        d->pcconnect_rx_wake_count = 0;
        return;
    }
    if ((d->pcconnect_commmode_pending & BE300_COMMMODE_PENDING_MASK) != 0)
        return;
    if (rx_count == d->pcconnect_rx_wake_count)
        return;

    /*
     * The companion SIU receive interrupt is a level condition: while the
     * UART still reports RXRDY, the VRC4173 CommMode modem subsource can
     * become pending again after the OAL consumes the previous dispatch.
     * Treating it as a one-shot edge loses PCConnect's poll bytes if the
     * first SYSINTR fires before serial.dll drains the UART. Re-edge only
     * when the queued RX depth changes; the same unread byte does not create
     * a new hardware edge on every machine poll.
     */
    d->pcconnect_rx_wake_count = rx_count;
    d->pcconnect_commmode_pending |= BE300_COMMMODE_MODEM_PENDING;
    d->stowaway_commmode_events |= BE300_COMMMODE_MODEM_EVENT_BITS;
    be300_pcconnect_trace(d, "uart-rx-level",
        BE300_COMMMODE_STATUS_OFF, 2,
        be300_pcconnect_commmode_read(d), 0);
    be300_pcconnect_irq_update(d);
    be300_pcconnect_irq_reedge(d);
}

static void be300_stowaway_maybe_raise_uart_rx_level(
    struct be300_vrc4173_latch *d)
{
    size_t rx_count;

    if (!d || !be300_stowaway_keyboard_enabled())
        return;

    rx_count = stowaway_uart_rx_count();
    if (rx_count == 0) {
        d->pcconnect_rx_wake_count = 0;
        return;
    }

    if ((d->pcconnect_commmode_pending & BE300_COMMMODE_PENDING_MASK) != 0)
        return;
    if (rx_count == d->pcconnect_rx_wake_count)
        return;

    /*
     * The host key enqueue can be observed by WinCE before GXemul's periodic
     * ns16550 tick has refreshed IIR from the backend RX count.  If that first
     * CommMode service reads IIR_NOPEND, serial.dll clears the companion latch
     * while the UART line later remains asserted.  Re-raise the same GIRQ0-4
     * modem subsource once the RX level is still non-empty.  The companion
     * SIU lives behind the VRC4173 Vic/CommMode block
     * (docs/hardware/hardware.txt:122-130, 190-191), so this models the
     * same level-sensitive serial-dock dispatch path used for PCConnect.
     */
    d->pcconnect_rx_wake_count = rx_count;
    d->pcconnect_commmode_pending |= BE300_COMMMODE_MODEM_PENDING;
    d->stowaway_commmode_events |= BE300_COMMMODE_MODEM_EVENT_BITS;
    be300_pcconnect_trace(d, "stowaway-uart-rx-level",
        BE300_COMMMODE_STATUS_OFF, 2, be300_pcconnect_commmode_read(d), 0);
    be300_pcconnect_irq_update(d);
    be300_pcconnect_irq_reedge(d);
}

static uint16_t be300_pcconnect_write_u16_at(uint32_t off, unsigned len,
    uint64_t val, uint32_t target, uint16_t fallback)
{
    if (off <= target && target + 2u <= off + len) {
        unsigned shift = (unsigned)((target - off) * 8u);
        return (uint16_t)((val >> shift) & 0xffffu);
    }

    return fallback;
}

void be300_pcconnect_note_comm_write(struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len, uint64_t val, uint32_t pc)
{
    uint16_t commmode;
    uint16_t old_pending;
    bool reedge = false;
    uint32_t commsiu;

    if (!d || len == 0)
        return;
    if (!be300_commmode_interrupt_enabled())
        return;

    old_pending = d->pcconnect_commmode_pending;

    if ((off <= BE300_COMMMODE_STATUS_OFF &&
         off + len > BE300_COMMMODE_STATUS_OFF) ||
        (off <= BE300_COMMMODE_SOCKET_OFF &&
         off + len > BE300_COMMMODE_SOCKET_OFF) ||
        (off <= BE300_COMMSIU_CTRL_OFF &&
         off + len > BE300_COMMSIU_CTRL_OFF))
        be300_pcconnect_trace(d, "pre-write", off, len, val, pc);

    if (off <= BE300_COMMMODE_STATUS_OFF &&
        off + len > BE300_COMMMODE_STATUS_OFF) {
        commmode = be300_pcconnect_write_u16_at(off, len, val,
            BE300_COMMMODE_STATUS_OFF, be300_pcconnect_commmode_raw(d));
        /*
         * AA008004 is a pending/mask pair like the PIU interrupt register
         * described in hardware.txt:132-145.  The low byte is interrupt
         * status, and the guest acknowledges observed status bits by
         * writing them back as 1s while preserving the high-byte mask.
         */
        d->pcconnect_commmode_pending &=
            (uint16_t)~(commmode & BE300_COMMMODE_PENDING_MASK);
        if ((old_pending & BE300_COMMMODE_SOCKET_PENDING) != 0 &&
            (d->pcconnect_commmode_pending &
                BE300_COMMMODE_SOCKET_PENDING) == 0 &&
            be300_pcconnect_cable_enabled() &&
            d->pcconnect_dock_connected &&
            !d->pcconnect_modem_after_socket_sent) {
            /*
             * hardware.txt:124-130 documents AA008004 as separate
             * pending/mask sub-bits.  Present the RS-232 socket insertion
             * first, then raise the companion modem-event subsource after
             * the socket bit is acknowledged so sub-bit 4 is not swallowed
             * by the same writeback that clears sub-bit 0.
             */
            d->pcconnect_commmode_pending |= BE300_COMMMODE_MODEM_PENDING;
            d->stowaway_commmode_events |= BE300_COMMMODE_MODEM_EVENT_BITS;
            d->pcconnect_modem_after_socket_sent = true;
        }
        reedge =
            (old_pending & BE300_COMMMODE_SOCKET_PENDING) != 0 &&
            (d->pcconnect_commmode_pending &
                BE300_COMMMODE_SOCKET_PENDING) == 0 &&
            be300_pcconnect_commmode_unmasked(d);
        d->bytes[BE300_COMMMODE_STATUS_OFF] &=
            (uint8_t)~BE300_COMMMODE_PENDING_MASK;
        if ((commmode & BE300_COMMMODE_IRQ_MASK) != 0)
            be300_pcconnect_maybe_raise_dock_edge(d);
    }

    if (off <= BE300_COMMSIU_CTRL_OFF &&
        off + len > BE300_COMMSIU_CTRL_OFF) {
        commsiu = be300_latch_peek_u32(d, BE300_COMMSIU_CTRL_OFF);
        if ((commsiu & BE300_COMMSIU_CTRL_RS232) != 0)
            be300_pcconnect_maybe_raise_dock_edge(d);
    }

    be300_pcconnect_irq_update(d);
    if (reedge) {
        be300_pcconnect_trace(d, "commmode-reedge",
            BE300_COMMMODE_STATUS_OFF, 2,
            be300_pcconnect_commmode_read(d), pc);
        be300_pcconnect_irq_reedge(d);
    }

    if ((off <= BE300_COMMMODE_STATUS_OFF &&
         off + len > BE300_COMMMODE_STATUS_OFF) ||
        (off <= BE300_COMMMODE_SOCKET_OFF &&
         off + len > BE300_COMMMODE_SOCKET_OFF) ||
        (off <= BE300_COMMSIU_CTRL_OFF &&
         off + len > BE300_COMMSIU_CTRL_OFF))
        be300_pcconnect_trace(d, "post-write", off, len, val, pc);
}

uint32_t be300_pcconnect_girq0_source_bits(
    const struct be300_vrc4173_latch *d)
{
    if (!be300_commmode_interrupt_enabled())
        return 0;

    return be300_pcconnect_commmode_unmasked(d) ?
        BE300_GIRQ0_COMMMODE : 0;
}

/*
 * AA001054 holds the serial dock's CommMode modem events, which the guest
 * acknowledges by writing 1s to them (write-1-to-clear); its other bits are
 * stored as written. The write lands in the 0x1000-0x1FFF CF companion
 * path, so acknowledge here and keep the written event bits out of the
 * stored register, or they read back as new events.
 */
void be300_commmode_event_ack(struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len, uint64_t val)
{
    uint16_t written;

    if (!be300_serial_dock_socket_enabled() ||
        off > BE300_COMMMODE_MODEM_EVENT_OFF ||
        off + len <= BE300_COMMMODE_MODEM_EVENT_OFF)
        return;

    written = be300_pcconnect_write_u16_at(off, len, val,
        BE300_COMMMODE_MODEM_EVENT_OFF, 0) & BE300_COMMMODE_MODEM_EVENT_BITS;
    d->bytes[BE300_COMMMODE_MODEM_EVENT_OFF] &= (uint8_t)~written;
    d->bytes[BE300_COMMMODE_MODEM_EVENT_OFF + 1] &= (uint8_t)~(written >> 8);
    d->stowaway_commmode_events &= (uint16_t)~written;
    if (written != 0) {
        d->pcconnect_commmode_pending &=
            (uint16_t)~BE300_COMMMODE_MODEM_PENDING;
        be300_pcconnect_irq_update(d);
    }
}

/* Serial dock CommMode registers, present only with a dock attached. */
bool be300_commmode_status_claims(struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len)
{
    (void)d;
    (void)len;
    return be300_commmode_interrupt_enabled() &&
        off == BE300_COMMMODE_STATUS_OFF;
}

uint64_t be300_commmode_status_read(struct cpu *cpu,
    struct be300_vrc4173_latch *d, uint32_t off, unsigned len)
{
    uint16_t val;

    if (be300_pcconnect_cable_enabled())
        be300_pcconnect_maybe_raise_dock_edge(d);
    val = be300_pcconnect_commmode_read(d);
    be300_pcconnect_trace(d, "read-commmode", off, (uint32_t)len,
        val, (uint32_t)cpu->pc);
    return val;
}

bool be300_commmode_socket_claims(struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len)
{
    (void)d;
    (void)len;
    return be300_serial_dock_socket_enabled() &&
        off == BE300_COMMMODE_SOCKET_OFF;
}

uint64_t be300_commmode_socket_read(struct cpu *cpu,
    struct be300_vrc4173_latch *d, uint32_t off, unsigned len)
{
    uint16_t val;

    if (be300_pcconnect_cable_enabled())
        be300_pcconnect_maybe_raise_dock_edge(d);
    val = be300_pcconnect_socket_read(d);
    be300_pcconnect_trace(d, "read-socket", off, (uint32_t)len,
        val, (uint32_t)cpu->pc);
    return val;
}

bool be300_commmode_serial_socket_claims(struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len)
{
    (void)d;
    (void)len;
    return be300_serial_dock_socket_enabled() &&
        off == BE300_COMMMODE_SERIAL_SOCKET_OFF;
}

uint64_t be300_commmode_serial_socket_read(struct cpu *cpu,
    struct be300_vrc4173_latch *d, uint32_t off, unsigned len)
{
    (void)cpu;
    (void)off;
    (void)len;
    return be300_serial_dock_socket_connected(d) ? 0u : 1u;
}

bool be300_commmode_modem_event_claims(struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len)
{
    (void)d;
    (void)len;
    return be300_serial_dock_socket_enabled() &&
        off == BE300_COMMMODE_MODEM_EVENT_OFF;
}

    /*
     * serial.dll's RS-232 path reads AA001054 as a companion-side
     * modem-event latch before it accepts UART DCD/RLSD for the
     * serial dock. hardware.txt:189-191 labels 0xaa001000 as a
     * separate companion block next to the VRC4173 SIU, and
     * hw_dump_vrc4173.txt:39-42 captures the real
     * 0x0A001010/0x0A001054 latch values.
     */
uint64_t be300_commmode_modem_event_read(struct cpu *cpu,
    struct be300_vrc4173_latch *d, uint32_t off, unsigned len)
{
    (void)cpu;
    (void)off;
    (void)len;
    return be300_serial_dock_commmode_event_read(d);
}
