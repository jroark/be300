/*
 *  be300_devices.c — GXemul DEVICE_ACCESS wrappers for BE-300 peripherals.
 *
 *  Registers our hardware peripheral state structs as GXemul memory-mapped
 *  devices at the VRC4131/VRC4173 address ranges.
 *
 *  GXemul's native dev_vr41xx.c covers VR4131 internal I/O only up to
 *  0x0F000800 (DEV_VR41XX_LENGTH). The VR4131 SIU (0x0F000800) and DSIU
 *  (0x0F000820) are registered here.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu.h"
#include "cop0.h"
#include "cpu_mips.h"   /* mips_cpu_cold_reset() for KjCMU warm-reset trigger */
#include "machine.h"
#include "memory.h"
#include "misc.h"

#include "be300.h"
#include "be300_probe.h"
#include "devices.h"
#include "devices/devices_internal.h"
#include "hw/cf.h"
#include "hw/nand.h"
#include "hw/siu.h"
#include "pcconnect.h"
#include "ppsh.h"
#include "stowaway.h"
#include "ui.h"

#define BE300_NS_PER_MS 1000000ULL

struct be300_vrc4173_latch *g_be300_vrc4173_latch;

uint64_t be300_host_monotonic_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/*
 * --log-mmio: print one line per dispatched access. Volume-heavy by design
 * (every MMIO touch), matches the flag name in src/main.c usage text.
 */
void be300_log_mmio(const char *name, int writeflag, uint32_t off,
    unsigned len, const unsigned char *data, uint32_t pc)
{
    uint64_t val = 0;
    unsigned i;
    unsigned n = len > 8 ? 8 : len;
    for (i = 0; i < n; i++)
        val |= (uint64_t)data[i] << (8u * i);
    fprintf(stderr, "[MMIO] pc=%08X %s %s @%X len=%u %s 0x%0*" PRIX64 "\n",
        pc, writeflag == MEM_WRITE ? "wr" : "rd", name, off, len,
        writeflag == MEM_WRITE ? "<=" : "=>", (int)(n * 2),
        (uint64_t)val);
}

/*
 *  VRC4173 catch-all latch device.
 *
 *  The VRC4173 companion chip has many register blocks. The SPL reads
 *  board ID, NAND status, NE2000 config, and other registers during init.
 *  This latch captures all VRC4173 accesses not handled by the NAND device
 *  or GXemul's ns16550 (SIU).  Writes are stored, reads return last value.
 *
 *  Special addresses:
 *    0x0A0C0 (board ID) returns 0x7100 (BE-300 identifier)
 */

#define VRC4173_LATCH_BASE   0x0A000000ULL
#define VRC4173_LATCH_SIZE   0x00020000     /* 128KB covers all VRC4173 space */
/* Casio SDK buzzer.h ranges: hardware.txt:186 and hardware.txt:188. */
#define BE300_BUZZER_BLG_OFF 0x0980u
#define BE300_BUZZER_BLG_LEN 0x0068u
#define BE300_BUZZER_CMM_OFF 0x1128u
#define BE300_BUZZER_CMM_LEN 0x0004u

struct be300_buzzer_state {
    uint8_t blg[BE300_BUZZER_BLG_LEN];
    uint8_t cmm[BE300_BUZZER_CMM_LEN];
};

struct be300_vrc4173_latch {
    uint8_t  bytes[0x20000];
    bool     log_mmio;
    struct be300_buzzer_state buzzer;
    uint32_t usb_intr_status;
    uint32_t usb_intr_enable;
    uint32_t usb_port_status[2];
    struct interrupt pcconnect_irq;
    bool     pcconnect_irq_connected;
    bool     pcconnect_irq_asserted;
    struct interrupt cf_irq;
    bool     cf_irq_connected;
    bool     cf_irq_asserted;
    bool     pcconnect_dock_connected;
    uint16_t pcconnect_commmode_pending;
    uint16_t stowaway_commmode_events;
    bool     pcconnect_modem_after_socket_sent;
    size_t   pcconnect_rx_wake_count;
    bool     pcconnect_insert_armed;
    uint32_t pcconnect_insert_delay_ms;
    uint64_t pcconnect_insert_deadline_ns;
};

machine_t *g_be300_machine = NULL;  /* for PIU cross-device callback */

static uint32_t be300_latch_peek_u32(struct be300_vrc4173_latch *d,
    uint32_t off)
{
    if (!d || off + 4u > VRC4173_LATCH_SIZE)
        return 0;

    return (uint32_t)d->bytes[off + 0u]
         | ((uint32_t)d->bytes[off + 1u] << 8)
         | ((uint32_t)d->bytes[off + 2u] << 16)
         | ((uint32_t)d->bytes[off + 3u] << 24);
}

static uint16_t be300_latch_peek_u16(const struct be300_vrc4173_latch *d,
    uint32_t off)
{
    if (!d || off + 2u > VRC4173_LATCH_SIZE)
        return 0;

    return (uint16_t)d->bytes[off + 0u]
         | ((uint16_t)d->bytes[off + 1u] << 8);
}

static void be300_latch_poke_u32(struct be300_vrc4173_latch *d,
    uint32_t off, uint32_t val)
{
    if (!d || off + 4u > VRC4173_LATCH_SIZE)
        return;

    d->bytes[off + 0u] = (uint8_t)(val >> 0);
    d->bytes[off + 1u] = (uint8_t)(val >> 8);
    d->bytes[off + 2u] = (uint8_t)(val >> 16);
    d->bytes[off + 3u] = (uint8_t)(val >> 24);
}

void be300_cf_irq_update(struct be300_vrc4173_latch *d)
{
    bool want;

    if (!d || !d->cf_irq_connected || !g_be300_machine)
        return;

    /*
     * hardware.txt:15-32 and :65-101 route PCMCIA as
     * SYSINT1.GIU -> GIUINTLREG.GIRQ0 -> AA000004 bit 0 -> AA00A03C.
     * cf_giu_source_bits() supplies the AA000004 bit; this drives the
     * corresponding level into the VR4131 GIU interrupt line.
     */
    want = cf_giu_source_bits(
        &g_be300_machine->cf[BE300_PRIMARY_CF_SLOT]) != 0;
    if (want && !d->cf_irq_asserted) {
        INTERRUPT_ASSERT(d->cf_irq);
        d->cf_irq_asserted = true;
    } else if (!want && d->cf_irq_asserted) {
        INTERRUPT_DEASSERT(d->cf_irq);
        d->cf_irq_asserted = false;
    }
}

void be300_vrc4173_update_cf_irq(void)
{
    be300_cf_irq_update(g_be300_vrc4173_latch);
}

static uint32_t be300_buzzer_peek_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void be300_buzzer_seed(struct be300_vrc4173_latch *d)
{
    /*
     * Real-hardware idle values:
     *   docs/hardware/hw_dump_vrc4173.txt:1084 -> BlgReg first 4 words
     *   docs/hardware/hw_dump_vrc4173.txt:48   -> CmmReg word at 0x1128
     */
    static const struct {
        uint16_t off;
        uint32_t val;
    } blg_seed[] = {
        { 0x0000u, 0x00000001u },
        { 0x0004u, 0x00000202u },
        { 0x0008u, 0x00000F70u },
        { 0x000Cu, 0x00000000u },
    };
    uint32_t cmm = 0x00000001u;

    if (!d)
        return;

    for (unsigned i = 0; i < sizeof(blg_seed) / sizeof(blg_seed[0]); i++) {
        uint16_t off = blg_seed[i].off;
        uint32_t v = blg_seed[i].val;
        if (off + 4u <= BE300_BUZZER_BLG_LEN) {
            memcpy(&d->buzzer.blg[off], &v, 4);
            memcpy(&d->bytes[BE300_BUZZER_BLG_OFF + off], &v, 4);
        }
    }

    memcpy(d->buzzer.cmm, &cmm, sizeof(cmm));
    memcpy(&d->bytes[BE300_BUZZER_CMM_OFF], &cmm, sizeof(cmm));
}

static bool be300_buzzer_range_overlap(uint32_t off, unsigned len,
    uint32_t range_off, uint32_t range_len)
{
    uint64_t a0 = off;
    uint64_t a1 = a0 + len;
    uint64_t b0 = range_off;
    uint64_t b1 = b0 + range_len;

    return len > 0 && a0 < b1 && b0 < a1;
}

static uint32_t be300_buzzer_tone_hz(const struct be300_buzzer_state *b)
{
    uint32_t period = be300_buzzer_peek_le32(&b->blg[0x0008]) & 0xffffu;
    uint32_t hz;

    if (period == 0)
        return 1200;

    /*
     * docs/hardware/hw_dump_vrc4173.txt:1084 captures 0x0F70 in the
     * timing register; treating it as a 4 MHz divisor yields roughly
     * 1 kHz, consistent with a piezo notification tone. This conversion
     * only affects host audio rendering, not guest-visible register values.
     */
    hz = 4000000u / period;
    if (hz < 80)
        hz = 80;
    if (hz > 6000)
        hz = 6000;
    return hz;
}

static uint32_t be300_buzzer_tone_ms(const struct be300_buzzer_state *b)
{
    uint32_t cfg = be300_buzzer_peek_le32(&b->blg[0x0004]);
    uint32_t ms = 45u + ((cfg & 0xffu) * 8u);

    if (ms < 35)
        ms = 35;
    if (ms > 220)
        ms = 220;
    return ms;
}

#define VRC4173_USB_OP_BASE       0x1440u
#define VRC4173_USB_OP_END        0x14A0u
#define VRC4173_USB_HC_REVISION   0x0010u
#define VRC4173_USB_INTR_RHSC     0x00000040u
#define VRC4173_USB_INTR_MIE      0x80000000u
#define VRC4173_USB_PORT_CCS      0x00000001u
#define VRC4173_USB_PORT_PES      0x00000002u
#define VRC4173_USB_PORT_PSS      0x00000004u
#define VRC4173_USB_PORT_POCI     0x00000008u
#define VRC4173_USB_PORT_PRS      0x00000010u
#define VRC4173_USB_PORT_PPS      0x00000100u
#define VRC4173_USB_PORT_LSDA     0x00000200u
#define VRC4173_USB_PORT_CSC      0x00010000u
#define VRC4173_USB_PORT_PESC     0x00020000u
#define VRC4173_USB_PORT_PSSC     0x00040000u
#define VRC4173_USB_PORT_POCIC    0x00080000u
#define VRC4173_USB_PORT_PRSC     0x00100000u
#define VRC4173_USB_PORT_CHANGE_MASK \
    (VRC4173_USB_PORT_CSC | VRC4173_USB_PORT_PESC | \
     VRC4173_USB_PORT_PSSC | VRC4173_USB_PORT_POCIC | \
     VRC4173_USB_PORT_PRSC)
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

static uint32_t be300_pcconnect_connect_delay_ms(void)
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

static void be300_pcconnect_trace(const struct be300_vrc4173_latch *d,
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
         * places this page next to the companion SIU.  For the serial
         * PC Connect option, do not expose a socket until the emulated
         * cable edge; after that edge, expose the RS-232 socket.  The
         * Stowaway keyboard dock is already physically inserted when
         * --stowaway-keyboard is selected, and its driver uses this same
         * socket.dll path before accepting the UART DCD level.  Preserve
         * the other latched bits.
         */
        v &= (uint16_t)~BE300_COMMMODE_SOCKET_VALUE_MASK;
        v |= be300_serial_dock_socket_connected(d) ?
            BE300_COMMMODE_SOCKET_RS232 : BE300_COMMMODE_SOCKET_NONE;
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

static uint64_t be300_pcconnect_pccard_status_read(
    const struct be300_vrc4173_latch *d, bool cf_attached, uint32_t off,
    unsigned len, uint64_t val)
{
    unsigned shift;

    if (!d || !be300_serial_dock_socket_enabled() ||
        cf_attached || !be300_serial_dock_socket_connected(d) || len == 0 ||
        off > BE300_PCCARD_STATUS_OFF ||
        off + len <= BE300_PCCARD_STATUS_OFF)
        return val;

    /*
     * pcmcia.dll maps AA001B00 and waits for AA001B50 bit 3 after
     * enabling the socket path via AA000144 bit 5.  The 0x1000-0x1fff
     * VRC4173 range is currently backed by the CF companion model, so
     * expose the PC Connect dock-ready level at the actual read boundary
     * instead of seeding the generic latch byte array.  When --cf is
     * attached, the CF model owns this status byte and intentionally
     * returns the inserted-card edge only once before settling back to the
     * real inserted-CF dump's zero value; do not keep reasserting it from
     * the PC Connect dock path.
     */
    shift = (unsigned)((BE300_PCCARD_STATUS_OFF - off) * 8u);
    return val | ((uint64_t)BE300_PCCARD_SOCKET_READY << shift);
}

static bool be300_pcconnect_commmode_unmasked(
    const struct be300_vrc4173_latch *d)
{
    uint16_t v = be300_pcconnect_commmode_read(d);

    return (v & (v >> 8) & BE300_COMMMODE_PENDING_MASK) != 0;
}

static void be300_pcconnect_irq_update(struct be300_vrc4173_latch *d)
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
    pcconnect_reset_guest_serial();
    d->usb_intr_status = 0;
    d->usb_port_status[0] &= ~VRC4173_USB_PORT_CHANGE_MASK;
    d->usb_port_status[1] &= ~VRC4173_USB_PORT_CHANGE_MASK;
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

bool be300_pcconnect_irq_is_asserted(void)
{
    struct be300_vrc4173_latch *d = g_be300_vrc4173_latch;
    return d && d->pcconnect_irq_asserted;
}

void be300_pcconnect_poll(void)
{
    if (be300_pcconnect_cable_enabled()) {
        be300_pcconnect_maybe_raise_dock_edge(g_be300_vrc4173_latch);
        be300_pcconnect_maybe_raise_uart_rx_level(g_be300_vrc4173_latch);
        return;
    }

    if (be300_stowaway_keyboard_enabled()) {
        be300_pcconnect_maybe_raise_dock_edge(g_be300_vrc4173_latch);
        be300_pcconnect_irq_update(g_be300_vrc4173_latch);
        be300_pcconnect_irq_reedge(g_be300_vrc4173_latch);
    }
}

static void be300_pcconnect_uart_irq_ready(void *opaque)
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

static uint16_t be300_pcconnect_write_u16_at(uint32_t off, unsigned len,
    uint64_t val, uint32_t target, uint16_t fallback)
{
    if (off <= target && target + 2u <= off + len) {
        unsigned shift = (unsigned)((target - off) * 8u);
        return (uint16_t)((val >> shift) & 0xffffu);
    }

    return fallback;
}

static void be300_pcconnect_note_comm_write(struct be300_vrc4173_latch *d,
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

static void be300_pcconnect_note_comm_read(struct be300_vrc4173_latch *d,
    uint16_t val, uint32_t pc)
{
    uint16_t active;

    if (!d)
        return;

    active = val & (uint16_t)(val >> 8) & BE300_COMMMODE_PENDING_MASK;

    /*
     * The NK GIRQ0-4 dispatcher at 0x800b6db4 selects the lowest active
     * AA008004 sub-bit. Sub-bit 4 is the route into serial.dll's modem/event
     * IST; once the OAL has dispatched it, drop the CommMode routing bit and
     * let AA001054 plus the UART RX level re-edge it if work remains.
     * hardware.txt:122-130 documents the GIRQ0-4 sub-bits, and
     * hardware.txt:189-191 places AA001054 in the companion serial block.
     */
    if (pc == 0x800b6db4u &&
        (active & BE300_COMMMODE_SOCKET_PENDING) == 0 &&
        (active & BE300_COMMMODE_MODEM_PENDING) != 0) {
        d->pcconnect_commmode_pending &=
            (uint16_t)~BE300_COMMMODE_MODEM_PENDING;
        be300_pcconnect_irq_update(d);
        be300_pcconnect_trace(d, "commmode-modem-dispatch",
            BE300_COMMMODE_STATUS_OFF, 2, val, pc);
        return;
    }
}

static uint32_t be300_pcconnect_girq0_source_bits(
    const struct be300_vrc4173_latch *d)
{
    if (!be300_commmode_interrupt_enabled())
        return 0;

    return be300_pcconnect_commmode_unmasked(d) ?
        BE300_GIRQ0_COMMMODE : 0;
}

static bool be300_vrc4173_usb_op_offset(uint32_t off)
{
    return off >= VRC4173_USB_OP_BASE && off < VRC4173_USB_OP_END;
}

static unsigned be300_vrc4173_usb_op_index(uint32_t off)
{
    return (unsigned)((off - VRC4173_USB_OP_BASE) >> 2);
}

static uint32_t be300_vrc4173_usb_port_read(
    struct be300_vrc4173_latch *d, unsigned port)
{
    uint32_t status;

    if (!d || port >= 2)
        return 0;

    status = d->usb_port_status[port];
    status &= ~(VRC4173_USB_PORT_CCS | VRC4173_USB_PORT_PES |
        VRC4173_USB_PORT_PSS | VRC4173_USB_PORT_PRS |
        VRC4173_USB_PORT_PPS | VRC4173_USB_PORT_LSDA);

    return status;
}

static void be300_vrc4173_usb_refresh_intr(struct be300_vrc4173_latch *d)
{
    if (!d)
        return;

    if ((be300_vrc4173_usb_port_read(d, 0) & VRC4173_USB_PORT_CHANGE_MASK) ||
        (be300_vrc4173_usb_port_read(d, 1) & VRC4173_USB_PORT_CHANGE_MASK))
        d->usb_intr_status |= VRC4173_USB_INTR_RHSC;
    else
        d->usb_intr_status &= ~VRC4173_USB_INTR_RHSC;

    be300_latch_poke_u32(d, VRC4173_USB_OP_BASE + 0x0Cu,
        d->usb_intr_status);
    be300_latch_poke_u32(d, VRC4173_USB_OP_BASE + 0x10u,
        d->usb_intr_enable);
    be300_latch_poke_u32(d, VRC4173_USB_OP_BASE + 0x14u,
        d->usb_intr_enable);
    be300_latch_poke_u32(d, VRC4173_USB_OP_BASE + 0x54u,
        be300_vrc4173_usb_port_read(d, 0));
    be300_latch_poke_u32(d, VRC4173_USB_OP_BASE + 0x58u,
        be300_vrc4173_usb_port_read(d, 1));
}

static uint32_t be300_vrc4173_usb_read(struct be300_vrc4173_latch *d,
    uint32_t off)
{
    unsigned idx = be300_vrc4173_usb_op_index(off);

    be300_vrc4173_usb_refresh_intr(d);

    switch (idx) {
    case 0x00 / 4:
        return VRC4173_USB_HC_REVISION;
    case 0x0C / 4:
        return d->usb_intr_status;
    case 0x10 / 4:
    case 0x14 / 4:
        return d->usb_intr_enable;
    case 0x50 / 4:
        return be300_latch_peek_u32(d, off);
    case 0x54 / 4:
        return be300_vrc4173_usb_port_read(d, 0);
    case 0x58 / 4:
        return be300_vrc4173_usb_port_read(d, 1);
    default:
        return be300_latch_peek_u32(d, off);
    }
}

static void be300_vrc4173_usb_write(struct be300_vrc4173_latch *d,
    uint32_t off, uint32_t val)
{
    unsigned idx = be300_vrc4173_usb_op_index(off);

    /*
     * VRC4173 UM §14.3 exposes an OpenHCI 1.0 operational-register
     * window. usb.dll maps it at 0x0A001440. Serial PC Connect is routed
     * through the Vic/CommMode page and companion SIU, so the OHCI root hub
     * remains disconnected unless a separate USB-device model is added.
     */
    switch (idx) {
    case 0x00 / 4:
        /* HcRevision is read-only on the HC side. */
        break;
    case 0x08 / 4:
        be300_latch_poke_u32(d, off, val & ~1u);
        break;
    case 0x0C / 4:
        /*
         * VRC4173 UM §14.3.5 says HcInterruptStatus bits clear when 0 is
         * written. Also accept write-1-to-clear for driver compatibility.
         */
        if (val == 0)
            d->usb_intr_status = 0;
        else
            d->usb_intr_status &= ~val;
        break;
    case 0x10 / 4:
        d->usb_intr_enable |= val;
        break;
    case 0x14 / 4:
        d->usb_intr_enable &= ~val;
        break;
    case 0x50 / 4:
        if (val & 0x00010000u) {
            d->usb_port_status[0] |= VRC4173_USB_PORT_PPS;
            d->usb_port_status[1] |= VRC4173_USB_PORT_PPS;
        }
        if (val & 0x00000001u) {
            d->usb_port_status[0] &= ~VRC4173_USB_PORT_PPS;
            d->usb_port_status[1] &= ~VRC4173_USB_PORT_PPS;
        }
        be300_latch_poke_u32(d, off, val);
        break;
    case 0x54 / 4:
    case 0x58 / 4: {
        unsigned port = idx == (0x54 / 4) ? 0u : 1u;
        uint32_t status = d->usb_port_status[port];

        status &= ~(val & VRC4173_USB_PORT_CHANGE_MASK);
        if (val & VRC4173_USB_PORT_PPS)
            status |= VRC4173_USB_PORT_PPS;
        if (val & VRC4173_USB_PORT_LSDA)
            status &= ~VRC4173_USB_PORT_PPS;
        if ((val & VRC4173_USB_PORT_PES) &&
            (be300_vrc4173_usb_port_read(d, port) & VRC4173_USB_PORT_CCS))
            status |= VRC4173_USB_PORT_PES;
        if (val & VRC4173_USB_PORT_CCS)
            status &= ~VRC4173_USB_PORT_PES;
        if ((val & VRC4173_USB_PORT_PRS) &&
            (be300_vrc4173_usb_port_read(d, port) & VRC4173_USB_PORT_CCS))
            status |= VRC4173_USB_PORT_PES | VRC4173_USB_PORT_PRSC;
        if (val & VRC4173_USB_PORT_PSS)
            status |= VRC4173_USB_PORT_PSS;
        if (val & VRC4173_USB_PORT_POCI)
            status &= ~VRC4173_USB_PORT_PSS;

        d->usb_port_status[port] = status;
        break;
    }
    default:
        be300_latch_poke_u32(d, off, val);
        break;
    }

    be300_vrc4173_usb_refresh_intr(d);
}

static void be300_buzzer_note_write(struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len, const unsigned char *data)
{
    struct be300_buzzer_state *b;
    uint32_t old_control;
    uint32_t new_control;
    bool touched_blg;
    bool touched_cmm;

    if (!d || !data || len == 0)
        return;

    touched_blg = be300_buzzer_range_overlap(off, len,
        BE300_BUZZER_BLG_OFF, BE300_BUZZER_BLG_LEN);
    touched_cmm = be300_buzzer_range_overlap(off, len,
        BE300_BUZZER_CMM_OFF, BE300_BUZZER_CMM_LEN);
    if (!touched_blg && !touched_cmm)
        return;

    b = &d->buzzer;
    old_control = be300_buzzer_peek_le32(&b->blg[0]);

    for (unsigned i = 0; i < len; i++) {
        uint32_t byte_off = off + i;

        if (byte_off >= BE300_BUZZER_BLG_OFF &&
            byte_off < BE300_BUZZER_BLG_OFF + BE300_BUZZER_BLG_LEN) {
            uint32_t rel = byte_off - BE300_BUZZER_BLG_OFF;
            b->blg[rel] = data[i];
        }
        if (byte_off >= BE300_BUZZER_CMM_OFF &&
            byte_off < BE300_BUZZER_CMM_OFF + BE300_BUZZER_CMM_LEN) {
            uint32_t rel = byte_off - BE300_BUZZER_CMM_OFF;
            b->cmm[rel] = data[i];
        }
    }

    new_control = be300_buzzer_peek_le32(&b->blg[0]);

    /*
     * The real idle dump seeds BLG[0] and CMM bit 0 as set. Boot-time driver
     * configuration rewrites those registers without making real hardware
     * beep, so host audio should only model the explicit off->on edge.
     */
    if ((new_control & 1u) && !(old_control & 1u)) {
        ui_buzzer_pulse(be300_buzzer_tone_hz(b), be300_buzzer_tone_ms(b));
        return;
    }
}

static bool be300_reg_addr_to_pa(uint32_t addr, uint64_t *pa_out)
{
    if (!pa_out)
        return false;

    if (addr >= 0x80000000u && addr < 0xC0000000u)
        *pa_out = (uint64_t)(addr & 0x1FFFFFFFu);
    else
        *pa_out = (uint64_t)addr;

    return true;
}

static void be300_fb_mark_dirty(machine_t *m, uint32_t off, uint32_t len)
{
    struct vfb_data *fb;
    uint32_t bpp, stride, start_y, end_y, x1, x2;

    if (!m || !m->gxe_machine || !m->gxe_machine->fb || len == 0)
        return;

    fb = m->gxe_machine->fb;
    if (fb->bit_depth <= 0 || fb->bytes_per_line <= 0 ||
        fb->xsize <= 0 || fb->ysize <= 0)
        return;

    bpp = (uint32_t)((fb->bit_depth + 7) / 8);
    stride = (uint32_t)fb->bytes_per_line;
    if (bpp == 0 || stride == 0)
        return;

    start_y = off / stride;
    end_y = (off + len - 1u) / stride;
    if (start_y >= (uint32_t)fb->ysize)
        return;
    if (end_y >= (uint32_t)fb->ysize)
        end_y = (uint32_t)fb->ysize - 1u;

    if (start_y == end_y) {
        x1 = (off % stride) / bpp;
        x2 = ((off + len - 1u) % stride) / bpp;
    } else {
        x1 = 0;
        x2 = (uint32_t)fb->xsize - 1u;
    }

    if (x1 >= (uint32_t)fb->xsize)
        x1 = (uint32_t)fb->xsize - 1u;
    if (x2 >= (uint32_t)fb->xsize)
        x2 = (uint32_t)fb->xsize - 1u;

    if (fb->update_x1 > (int)x1) fb->update_x1 = (int)x1;
    if (fb->update_y1 > (int)start_y) fb->update_y1 = (int)start_y;
    if (fb->update_x2 < (int)x2) fb->update_x2 = (int)x2;
    if (fb->update_y2 < (int)end_y) fb->update_y2 = (int)end_y;
}

static void be300_vrc4173_a00_blit_maybe(struct cpu *cpu,
    struct be300_vrc4173_latch *d)
{
    machine_t *m = g_be300_machine;
    struct vfb_data *fb;
    uint32_t status, command, count_words, src_reg, dst_reg;
    uint32_t dst_off;
    uint64_t src_pa;
    size_t len, fb_size;
    unsigned char *src, *dst;

    if (!cpu || !cpu->mem || !d || !m || !m->gxe_machine ||
        !m->gxe_machine->fb)
        return;

    status = be300_latch_peek_u32(d, 0x0A00);
    if ((status & 1u) == 0)
        return;

    command = be300_latch_peek_u32(d, 0x0A04);
    if ((command & 0x81u) != 0x81u)
        return;

    count_words = be300_latch_peek_u32(d, 0x0A08);
    src_reg = be300_latch_peek_u32(d, 0x0A10);
    dst_reg = be300_latch_peek_u32(d, 0x0A14);

    /*
     * VRC4173/Casio graphics copy engine at PA 0x0A000A00.
     *
     * docs/hardware/hw_dump_vrc4173.txt:1085-1086 show this block
     * populated after boot (`0x0A000A00: 00000000 00000001 00000018 ...`,
     * `0x0A000A10: 00005A00 00027D7C ...`): idle status, mode 1, word
     * count, source scratch address, and framebuffer byte offset.
     * ddi.dll's row blitter at UM 0x01A53AD0..0x01A53D04 programs the
     * same block, stages a source row into low SDRAM (0x5800/0x5A00),
     * writes command 0x81, then triggers by writing bit 0 at 0xA00.
     *
     * Model the observed command as an immediate SDRAM-to-framebuffer copy.
     * TODO 2026-04-25: confirm the full bit assignments and completion
     * timing with a BEDiag hardware trace; this currently covers the only
     * command shape seen on the WinCE 3.0 boot path.
     */
    if (count_words == 0)
        goto complete;

    if (!be300_reg_addr_to_pa(src_reg, &src_pa))
        goto complete;

    fb = m->gxe_machine->fb;
    if (!fb->framebuffer || fb->framebuffer_size == 0)
        goto complete;

    fb_size = fb->framebuffer_size;
    if ((uint64_t)dst_reg >= PA_VRC4173_FB &&
        (uint64_t)dst_reg < PA_VRC4173_FB + (uint64_t)fb_size) {
        dst_off = dst_reg - PA_VRC4173_FB;
    } else if (((uint64_t)dst_reg & 0x1FFFFFFFu) >= PA_VRC4173_FB &&
        ((uint64_t)dst_reg & 0x1FFFFFFFu) <
            PA_VRC4173_FB + (uint64_t)fb_size) {
        dst_off = (uint32_t)(((uint64_t)dst_reg & 0x1FFFFFFFu) -
            PA_VRC4173_FB);
    } else {
        dst_off = dst_reg;
    }

    if ((uint64_t)dst_off >= (uint64_t)fb_size)
        goto complete;

    len = (size_t)count_words * 4u;
    if (len == 0)
        goto complete;
    if (len > fb_size - (size_t)dst_off)
        len = fb_size - (size_t)dst_off;

    src = memory_paddr_to_hostaddr(cpu->mem, src_pa, MEM_READ);
    if (!src)
        goto complete;

    dst = fb->framebuffer + dst_off;
    /*
     * memmove (not memcpy): WinCE GDI also drives this engine for
     * scroll/redraw with src and dst both inside the VRC4173 framebuffer
     * window (PA 0x0A200000+). memory_paddr_to_hostaddr() resolves both
     * to the same fb->framebuffer host buffer and the regions overlap.
     * memcpy() is undefined for overlap; memmove() is required.
     * Cold-boot SDRAM->FB paths are non-overlapping so behavior is
     * byte-identical there.
     */
    memmove(dst, src, len);
    be300_fb_mark_dirty(m, dst_off, (uint32_t)len);

complete:
    d->bytes[0x0A00] &= ~(uint8_t)1u;   /* trigger/busy complete */
    d->bytes[0x0A04] &= ~(uint8_t)0x80u; /* command bit self-clears */
}

static void be300_vrc4173_200_display_op_maybe(
    struct be300_vrc4173_latch *d)
{
    machine_t *m = g_be300_machine;
    struct vfb_data *fb;
    uint32_t mode, dst_off, width, height, color, src_off, bit_off;
    uint32_t stride, bpp;

    if (!d || !m || !m->gxe_machine || !m->gxe_machine->fb)
        return;

    fb = m->gxe_machine->fb;
    if (!fb->framebuffer || fb->framebuffer_size == 0 ||
        fb->bit_depth != 16 || fb->bytes_per_line <= 0)
        goto complete;

    if ((be300_latch_peek_u32(d, 0x0234) & 1u) == 0)
        return;

    mode = be300_latch_peek_u32(d, 0x0200);
    dst_off = (be300_latch_peek_u32(d, 0x0210) & 0xFFFFu)
        | ((be300_latch_peek_u32(d, 0x0214) & 0xFFFFu) << 16);
    width = be300_latch_peek_u32(d, 0x0208);
    height = be300_latch_peek_u32(d, 0x020C);
    color = be300_latch_peek_u32(d, 0x0204) & 0xFFFFu;
    stride = (uint32_t)fb->bytes_per_line;
    bpp = 2;

    /*
     * VRC4173/Casio display fill engine at PA 0x0A000200.
     *
     * The real-hardware dump identifies this as display-related:
     * docs/hardware/hw_dump_vrc4173.txt:977-982 shows the block populated,
     * including PA 0x0A000220 = 0x1E0 (240 visible pixels * 2 bytes).
     * ddi.dll's solid-fill path at UM 0x01A53EC8..0x01A53FA0 programs
     * destination byte offset at 0x210/0x214, width/height at 0x208/0x20C,
     * RGB565 color at 0x204, mode 0 at 0x200, then starts the operation by
     * writing 1 to 0x234. Model that mode as an immediate framebuffer fill.
     *
     * The same dump also captures mode 2 immediately below the seed row
     * (PA 0x0A000200 = 2, 0x0208/0x020C = 4x9, 0x0210 = 0xB414,
     * 0x0220 = 0x1E0, 0x0238 = 1). ddi.dll's glyph path at UM
     * 0x01A53848..0x01A53AAC programs mode 2, stages 1-bpp glyph rows into
     * the framebuffer row padding at byte offset 0x1E0, then starts the
     * block via 0x0234. Model that observed mode as transparent mono
     * expansion from the staged padding rows into the visible destination.
     *
     * Mode 1 is FB-to-FB block copy. ddi.dll's copy path at
     * UM 0x01A53D44..0x01A53EBC programs destination at 0x210/0x214,
     * source at 0x218/0x21C, width/height at 0x208/0x20C, then
     * starts through 0x234. The mode register is 1 plus optional
     * 0x400/0x800 start-corner bits; when those bits are set the driver
     * writes the right/bottom corner, and the hardware iterates from that
     * corner so overlapping scrolls preserve source pixels. Browser page
     * scrolling uses this path for 32-row framebuffer moves.
     *
     * TODO 2026-05-02: capture BEDiag traces for any other non-zero modes;
     * unobserved ROP modes remain deliberately unimplemented.
     */
    if (width == 0 || height == 0)
        goto complete;
    if ((mode & 0xFFu) != 1u &&
        (uint64_t)dst_off >= (uint64_t)fb->framebuffer_size)
        goto complete;

    if (mode == 0) {
        for (uint32_t y = 0; y < height; y++) {
            uint64_t row_off = (uint64_t)dst_off + (uint64_t)y * stride;
            if (row_off >= (uint64_t)fb->framebuffer_size)
                break;

            size_t row_left = fb->framebuffer_size - (size_t)row_off;
            uint32_t row_width = width;
            if ((uint64_t)row_width * bpp > row_left)
                row_width = (uint32_t)(row_left / bpp);

            uint16_t *row = (uint16_t *)(void *)(fb->framebuffer + row_off);
            for (uint32_t x = 0; x < row_width; x++)
                row[x] = (uint16_t)color;
        }

        {
            uint64_t len64 = (uint64_t)(height - 1u) * stride
                + (uint64_t)width * bpp;
            uint32_t len = len64 > UINT32_MAX ? UINT32_MAX : (uint32_t)len64;
            be300_fb_mark_dirty(m, dst_off, len);
        }
    } else if (mode == 2) {
        src_off = (be300_latch_peek_u32(d, 0x0220) & 0xFFFFu)
            | ((be300_latch_peek_u32(d, 0x0224) & 0xFFFFu) << 16);
        bit_off = be300_latch_peek_u32(d, 0x0228) & 7u;

        for (uint32_t y = 0; y < height; y++) {
            uint64_t dst_row_off = (uint64_t)dst_off + (uint64_t)y * stride;
            uint64_t src_row_off = (uint64_t)src_off + (uint64_t)y * stride;
            uint32_t row_width = width;
            uint8_t *src_row;
            uint16_t *dst_row;

            if (dst_row_off >= (uint64_t)fb->framebuffer_size ||
                src_row_off >= (uint64_t)fb->framebuffer_size)
                break;
            if ((uint64_t)row_width * bpp >
                (uint64_t)fb->framebuffer_size - dst_row_off)
                row_width = (uint32_t)(((uint64_t)fb->framebuffer_size -
                    dst_row_off) / bpp);

            src_row = fb->framebuffer + src_row_off;
            dst_row = (uint16_t *)(void *)(fb->framebuffer + dst_row_off);
            for (uint32_t x = 0; x < row_width; x++) {
                uint32_t bit = bit_off + x;
                uint64_t src_byte_off = src_row_off + (bit >> 3);
                uint8_t mask;

                if (src_byte_off >= (uint64_t)fb->framebuffer_size)
                    break;

                mask = (uint8_t)(0x80u >> (bit & 7u));
                if (src_row[bit >> 3] & mask)
                    dst_row[x] = (uint16_t)color;
            }
        }

        {
            uint64_t len64 = (uint64_t)(height - 1u) * stride
                + (uint64_t)width * bpp;
            uint32_t len = len64 > UINT32_MAX ? UINT32_MAX : (uint32_t)len64;
            be300_fb_mark_dirty(m, dst_off, len);
        }
    } else if ((mode & 0xFFu) == 1u) {
        uint32_t dst_byte = dst_off;
        uint32_t src_byte = (be300_latch_peek_u32(d, 0x0218) & 0xFFFFu)
            | ((be300_latch_peek_u32(d, 0x021C) & 0xFFFFu) << 16);
        bool h_start_right = (mode & 0x400u) != 0;
        bool v_start_bottom = (mode & 0x800u) != 0;
        bool reverse_y;
        size_t fb_size = fb->framebuffer_size;
        size_t row_bytes;

        if (h_start_right) {
            uint64_t back = (uint64_t)(width - 1u) * bpp;
            if ((uint64_t)src_byte < back || (uint64_t)dst_byte < back)
                goto complete;
            src_byte = (uint32_t)((uint64_t)src_byte - back);
            dst_byte = (uint32_t)((uint64_t)dst_byte - back);
        }
        if (v_start_bottom) {
            uint64_t back = (uint64_t)(height - 1u) * stride;
            if ((uint64_t)src_byte < back || (uint64_t)dst_byte < back)
                goto complete;
            src_byte = (uint32_t)((uint64_t)src_byte - back);
            dst_byte = (uint32_t)((uint64_t)dst_byte - back);
        }

        if ((uint64_t)src_byte >= (uint64_t)fb_size ||
            (uint64_t)dst_byte >= (uint64_t)fb_size)
            goto complete;

        row_bytes = (size_t)width * bpp;
        reverse_y = dst_byte > src_byte;
        for (uint32_t yi = 0; yi < height; yi++) {
            uint32_t y = reverse_y ? (height - 1u - yi) : yi;
            uint64_t src_row_off = (uint64_t)src_byte + (uint64_t)y * stride;
            uint64_t dst_row_off = (uint64_t)dst_byte + (uint64_t)y * stride;
            size_t this_row_bytes = row_bytes;

            if (src_row_off >= (uint64_t)fb_size ||
                dst_row_off >= (uint64_t)fb_size)
                continue;
            if (this_row_bytes > fb_size - (size_t)src_row_off)
                this_row_bytes = fb_size - (size_t)src_row_off;
            if (this_row_bytes > fb_size - (size_t)dst_row_off)
                this_row_bytes = fb_size - (size_t)dst_row_off;
            memmove(fb->framebuffer + dst_row_off,
                fb->framebuffer + src_row_off, this_row_bytes);
        }

        {
            uint64_t len64 = (uint64_t)(height - 1u) * stride
                + (uint64_t)width * bpp;
            uint32_t len = len64 > UINT32_MAX ? UINT32_MAX : (uint32_t)len64;
            be300_fb_mark_dirty(m, dst_byte, len);
        }
    }

complete:
    d->bytes[0x0234] &= ~(uint8_t)1u;   /* trigger/busy complete */
}

struct be300_vrc4173_segment {
    struct be300_vrc4173_latch *latch;
    uint32_t offset_in_latch;    /* offset of this segment within the latch */
};


DEVICE_ACCESS(be300_vrc4173)
{
    struct be300_vrc4173_segment *seg = (struct be300_vrc4173_segment *)extra;
    struct be300_vrc4173_latch *d = seg->latch;
    uint32_t off = seg->offset_in_latch + (uint32_t)relative_addr;

    if (off + len > VRC4173_LATCH_SIZE)
        return 0;

    if (d->log_mmio)
        be300_log_mmio("be300_vrc4173", writeflag, off, (unsigned)len,
            data, (uint32_t)cpu->pc);

    if (writeflag == MEM_WRITE) {
        uint64_t val = memory_readmax64(cpu, data, len);
        bool suspend_latch = false;

        be300_buzzer_note_write(d, off, (unsigned)len, data);

        if (g_be300_machine &&
            nand_restore_handles_offset(off)) {
            be300_probe_note_mmio("vrc4173-nand-restore", off, 'W',
                (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
                BE300_MMIO_CLASS_KNOWN);
            nand_restore_write(&g_be300_machine->nand, off, (unsigned)len,
                val);
            return 1;
        }

        if (be300_vrc4173_usb_op_offset(off)) {
            be300_probe_note_mmio("vrc4173-usbu-ohci", off, 'W',
                (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
                BE300_MMIO_CLASS_KNOWN);
            be300_vrc4173_usb_write(d, off, (uint32_t)val);
            return 1;
        }

        if (g_be300_machine && off >= 0x1000u && off < 0x2000u) {
            be300_probe_note_mmio("vrc4173-cf-companion", off, 'W',
                (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
                BE300_MMIO_CLASS_KNOWN);
            cf_companion_write(
                &g_be300_machine->cf[BE300_PRIMARY_CF_SLOT],
                off - 0x1000u, (unsigned)len, val);
            memcpy(&d->bytes[off], data, len);
            return 1;
        }

        if (g_be300_machine && (off == 0x0010u || off == 0x0014u)) {
            cf_clear_irq(&g_be300_machine->cf[BE300_PRIMARY_CF_SLOT]);
            be300_cf_irq_update(d);
        }
        /* GIRQ0-1 keyboard dispatcher acknowledges through AA000014;
         * see docs/hardware/hardware.txt:107-112. */
        if (g_be300_machine && off <= 0x0014u && off + len > 0x0014u)
            button_ack_keyboard_source(
                (struct be300_input_device *)g_be300_machine->button_device);

        if ((off <= 0x1120 && off + len > 0x1120) ||
            (off <= 0x112C && off + len > 0x112C) ||
            (off <= 0x1B20 && off + len > 0x1B20)) {
            suspend_latch = true;
        }

        /*
         * VRC4173 interrupt status registers use write-1-to-clear:
         * writing a 1-bit clears that bit.  Without this, the WinCE
         * interrupt dispatch loop writes 1 to clear interrupt sources
         * but the latch keeps returning 1 on subsequent reads.
         *
         * W1C registers (offsets from VRC4173 base 0x0A000000):
         *   0x060  SYSINT1REG — read-only aggregate interrupt status
         *   0x062-0x06A  Level-2 status registers (PIU, AIU, KIU, GIU)
         *   0x1120 GIU interrupt status / clear
         *   0x112C GIU interrupt status / clear
         *   0x1B00 INTSTAT1 (primary interrupt status)
         *   0x1B10 INTSTAT1 secondary read
         *   0x1B20 INTMASK1 / interrupt acknowledge
         */
        if (suspend_latch) {
            be300_probe_note_mmio("vrc4173-latch", off, 'W',
                (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
                BE300_MMIO_CLASS_LATCHED);
            memcpy(&d->bytes[off], data, len);
        } else if ((off >= 0x060 && off < 0x078) ||
            (off >= 0x1100 && off < 0x1140) ||
            (off >= 0x1B00 && off < 0x1B30)) {
            be300_probe_note_mmio("vrc4173-intr-w1c", off, 'W',
                (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
                BE300_MMIO_CLASS_KNOWN);
            /* W1C: clear bits that are written as 1 */
            for (size_t i = 0; i < len && (off + i) < VRC4173_LATCH_SIZE; i++) {
                uint32_t byte_off = off + (uint32_t)i;
                if (byte_off == 0x060u || byte_off == 0x061u)
                    continue;
                d->bytes[byte_off] &= ~data[i];
            }
        } else {
            be300_probe_note_mmio("vrc4173-latch", off, 'W',
                (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
                BE300_MMIO_CLASS_LATCHED);
            memcpy(&d->bytes[off], data, len);
        }
        be300_pcconnect_note_comm_write(d, off, (unsigned)len, val,
            (uint32_t)cpu->pc);
        if (be300_serial_dock_socket_enabled() &&
            off <= BE300_COMMMODE_MODEM_EVENT_OFF &&
            off + len > BE300_COMMMODE_MODEM_EVENT_OFF) {
            uint16_t written = be300_pcconnect_write_u16_at(off,
                (unsigned)len, val, BE300_COMMMODE_MODEM_EVENT_OFF, 0);
            d->stowaway_commmode_events &=
                (uint16_t)~(written & BE300_COMMMODE_MODEM_EVENT_BITS);
            if ((written & BE300_COMMMODE_MODEM_EVENT_BITS) != 0) {
                d->pcconnect_commmode_pending &=
                    (uint16_t)~BE300_COMMMODE_MODEM_PENDING;
                be300_pcconnect_irq_update(d);
            }
        }
        if (off <= 0x0A00u && off + len > 0x0A00u)
            be300_vrc4173_a00_blit_maybe(cpu, d);
        if (off <= 0x0234u && off + len > 0x0234u)
            be300_vrc4173_200_display_op_maybe(d);
        /* PIU control registers at offsets 0x000-0x05F: re-evaluate
         * scan sequencer state when NK.exe configures the PIU. */
        if (off < 0x060 && g_be300_machine && g_be300_machine->touch_device)
            piu_update_state(
                (struct be300_input_device *)g_be300_machine->touch_device);
    } else {
        if (g_be300_machine && off == 0x0004u) {
            be300_probe_note_mmio("vrc4173-giu-src", off, 'R',
                (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
                BE300_MMIO_CLASS_KNOWN);
            uint64_t val = be300_latch_peek_u32(d, off);

            /*
             * hardware.txt:32 identifies bit 0 here as the cascaded PCMCIA
             * source.  If no CF image is attached, there is no card-backed
             * source to report even if a guest write left bit 0 latched.
             */
            if (!cf_present(&g_be300_machine->cf[BE300_PRIMARY_CF_SLOT]))
                val &= ~(uint64_t)1u;
            val &= ~(uint64_t)BUTTON_GIRQ0_SOURCE;
            val |= cf_giu_source_bits(
                    &g_be300_machine->cf[BE300_PRIMARY_CF_SLOT])
                | be300_pcconnect_girq0_source_bits(d)
                | button_girq0_source_bits(
                    (const struct be300_input_device *)
                    g_be300_machine->button_device)
                | piu_girq0_source_bits(
                    (const struct be300_input_device *)
                    g_be300_machine->touch_device);
            be300_pcconnect_trace(d, "read-girq0", off, (uint32_t)len,
                val, (uint32_t)cpu->pc);
            memory_writemax64(cpu, data, len, val);
            return 1;
        }

        if (be300_commmode_interrupt_enabled() &&
            off == BE300_COMMMODE_STATUS_OFF) {
            uint16_t val;

            if (be300_pcconnect_cable_enabled())
                be300_pcconnect_maybe_raise_dock_edge(d);
            val = be300_pcconnect_commmode_read(d);
            be300_pcconnect_trace(d, "read-commmode", off, (uint32_t)len,
                val, (uint32_t)cpu->pc);
            be300_probe_note_mmio("vrc4173-commmode", off, 'R',
                (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
                BE300_MMIO_CLASS_KNOWN);
            memory_writemax64(cpu, data, len, val);
            be300_pcconnect_note_comm_read(d, val, (uint32_t)cpu->pc);
            return 1;
        }

        if (be300_serial_dock_socket_enabled() &&
            off == BE300_COMMMODE_SOCKET_OFF) {
            uint16_t val;

            if (be300_pcconnect_cable_enabled())
                be300_pcconnect_maybe_raise_dock_edge(d);
            val = be300_pcconnect_socket_read(d);
            be300_pcconnect_trace(d, "read-socket", off, (uint32_t)len,
                val, (uint32_t)cpu->pc);
            be300_probe_note_mmio("vrc4173-commmode-socket", off, 'R',
                (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
                BE300_MMIO_CLASS_KNOWN);
            memory_writemax64(cpu, data, len, val);
            return 1;
        }

        if (be300_serial_dock_socket_enabled() &&
            off == BE300_COMMMODE_SERIAL_SOCKET_OFF) {
            uint16_t val = be300_serial_dock_socket_connected(d) ? 0u : 1u;

            be300_probe_note_mmio("vrc4173-commmode-serial-socket",
                off, 'R', (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
                BE300_MMIO_CLASS_KNOWN);
            memory_writemax64(cpu, data, len, val);
            return 1;
        }

        if (be300_serial_dock_socket_enabled() &&
            off == BE300_COMMMODE_MODEM_EVENT_OFF) {
            uint16_t val = be300_serial_dock_commmode_event_read(d);

            /*
             * serial.dll's RS-232 path reads AA001054 as a companion-side
             * modem-event latch before it accepts UART DCD/RLSD for the
             * serial dock. hardware.txt:189-191 labels 0xaa001000 as a
             * separate companion block next to the VRC4173 SIU, and
             * hw_dump_vrc4173.txt:39-42 captures the real
             * 0x0A001010/0x0A001054 latch values.
             */
            be300_probe_note_mmio("vrc4173-commmode-modem-event", off, 'R',
                (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
                BE300_MMIO_CLASS_KNOWN);
            memory_writemax64(cpu, data, len, val);
            return 1;
        }

        if (g_be300_machine &&
            nand_restore_handles_offset(off)) {
            be300_probe_note_mmio("vrc4173-nand-restore", off, 'R',
                (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
                BE300_MMIO_CLASS_KNOWN);
            uint64_t val = nand_restore_read(&g_be300_machine->nand, off,
                (unsigned)len);
            memory_writemax64(cpu, data, len, val);
            return 1;
        }

        if (be300_vrc4173_usb_op_offset(off)) {
            uint32_t val = be300_vrc4173_usb_read(d, off);
            be300_probe_note_mmio("vrc4173-usbu-ohci", off, 'R',
                (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
                BE300_MMIO_CLASS_KNOWN);
            memory_writemax64(cpu, data, len, (uint64_t)val);
            return 1;
        }

        if (g_be300_machine && off >= 0x1000u && off < 0x2000u) {
            uint64_t val = cf_companion_read(
                &g_be300_machine->cf[BE300_PRIMARY_CF_SLOT],
                off - 0x1000u, (unsigned)len);
            val = be300_pcconnect_pccard_status_read(d,
                cf_present(&g_be300_machine->cf[BE300_PRIMARY_CF_SLOT]),
                off, (unsigned)len, val);
            /*
             * Pass 18/19 (2026-04-19): offset 0x1CD0 is the VRC4173
             * DMA-pending status bit that nanddisk.dll's CheckDMAEnd at
             * UM 0x019A55B0 polls for DMA completion. Real hw always
             * reads 0 once the transfer finishes (hw_dump_vrc4173.txt:524
             * shows 0x0A001CD0 = 0 even after NK runs).
             *
             * Pass 19 extends this: before returning 0, perform the
             * actual DMA transfer. The driver programs dest at 0x1CDC
             * and length at 0x1CD8, then arms via write-1-to-0x1CD0,
             * then programs XFER engine (MODE=5 stream at 0xAA00A464,
             * KICK at 0xAA00A460) which sets nand_state.stream_active.
             * By the time CheckDMAEnd polls 0x1CD0, stream is armed;
             * we copy `xfer_len` bytes from the NAND stream to the
             * destination physical address, then report "DMA done".
             */
            if (off == 0x1CD0u) {
                be300_probe_note_mmio("vrc4173-dma-done-synth", off, 'R',
                    (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
                    BE300_MMIO_CLASS_STUBBED);
                uint32_t latched = be300_latch_peek_u32(d, 0x1CD0);
                if ((latched & 1u) != 0 && cpu && cpu->mem &&
                    g_be300_machine->nand.stream_active) {
                    uint32_t dest_pa = be300_latch_peek_u32(d, 0x1CDC);
                    uint32_t xfer_len = be300_latch_peek_u32(d, 0x1CD8);
                    if (xfer_len > 0 && xfer_len <= 0x2000u) {
                        unsigned char *host = memory_paddr_to_hostaddr(
                            cpu->mem, (uint64_t)dest_pa, MEM_WRITE);
                        if (host)
                            nand_stream_read_dma(&g_be300_machine->nand,
                                host, xfer_len);
                    }
                    /* clear pending so subsequent polls don't re-trigger */
                    d->bytes[0x1CD0] &= ~(uint8_t)1u;
                }
                val &= ~(uint64_t)1u;
            } else {
                be300_probe_note_mmio("vrc4173-cf-companion", off, 'R',
                    (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
                    BE300_MMIO_CLASS_KNOWN);
            }
            memory_writemax64(cpu, data, len, val);
            return 1;
        }

        /*
         * Runtime PCMCIA scans also probe PA 0x0A00E000 through this
         * generic companion latch path (PC 0x0198A490).  That page is a
         * card-memory alias, not ordinary latch RAM; once the PCMCIA
         * bridge has enabled card windows, no-media reads must see the
         * pulled-up attribute bus instead of latch zeroes.
         */
        if (g_be300_machine &&
            cf_pcmcia_windows_enabled(
                &g_be300_machine->cf[BE300_PRIMARY_CF_SLOT]) &&
            off >= 0xE000u && off < 0xE800u) {
            uint64_t val = UINT64_MAX;
            if ((unsigned)len < sizeof(val))
                val >>= (unsigned)((sizeof(val) - (unsigned)len) * 8u);
            be300_probe_note_mmio("vrc4173-pcmcia-empty-attr", off, 'R',
                (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
                BE300_MMIO_CLASS_KNOWN);
            memory_writemax64(cpu, data, len, val);
            return 1;
        }

        /*
         * WORKAROUND: VRC4173 interrupt-aggregate status at
         * PA 0x0A0008A0 and PA 0x0A000A00.
         *
         * NK's OAL idle helper at VA 0x80079920..0x80079958 reads bit 0
         * of these two VRC4173 offsets (plus PA 0x0A00130C which is
         * modeled elsewhere as CF-companion). If any of the three has
         * bit 0 set, the helper skips `c0 0x22` SUSPEND and takes the
         * "handle pending" path (VA 0x800799C4); if all three read as
         * zero, it issues SUSPEND and blocks until the next IRQ.
         *
         * Real-HW snapshot (docs/hardware/hw_dump_vrc4173.txt):
         *     0x0A0008A0: 00000000
         *     0x0A000A00: 00000000
         * Both read as 0 on quiesced hardware — consistent with
         * interrupt-aggregate-status regs whose written value does not
         * latch back into the read side.
         *
         * Our catch-all latch returns whatever ddi.dll most recently
         * WROTE to these addresses (PCs 0x01911D04 and 0x01A53D04
         * observed — see Pass 36 --mmio-coverage). That makes the idle
         * helper always take the "pending" path, never SUSPEND, and
         * execute its 30-ish-instruction body 32 M times per wall
         * second — blocking ddi.dll's user-mode event-wait at PC
         * 0x01A53C84.
         *
         * Fix: reads at 0x8A0 and 0xA00 return latched & ~1u. Writes
         * still latch unchanged so non-bit-0 fields continue to round
         * trip. Pattern is the same as the 0x234 stub above and
         * matches the real-HW read value (0) for bit 0.
         *
         * TODO 2026-04-23: VRC4173 UM does not document these
         * addresses. Confirm via BEDiag trace; if the block turns out
         * to be a real interrupt-status aggregate, upgrade to a full
         * model driven by the lower-level enables at 0x1120 / 0x1B20 /
         * 0x112C that the ack path already clears.
         */
        if (off == 0x8A0u || off == 0xA00u) {
            uint32_t raw = be300_latch_peek_u32(d, off);
            uint64_t val = (uint64_t)(raw & ~(uint32_t)1u);
            memory_writemax64(cpu, data, len, val);
            be300_probe_note_mmio("vrc4173-intr-aggr-stub", off, 'R',
                (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
                BE300_MMIO_CLASS_STUBBED);
            return 1;
        }

        /*
         * WORKAROUND: VRC4173 display-controller-like block at
         * PA 0x0A000200..0x0A000234 (undocumented in VRC4173 UM).
         *
         * ddi.dll initialises offsets 0x200/0x204/0x208/0x20C/0x210/
         * 0x214/0x234 from PCs 0x01A53F00..0x01A53F88, then at PC
         * 0x01A5382C does:
         *     andi $25, $24, 1 ; bnel $25, $0, -2
         * spinning until bit 0 of *(0x0A000234) clears. With the
         * catch-all latch returning the last-written value, bit 0
         * never clears and ddi.dll spins forever (Pass 35: ~525M
         * reads/30s observed via --mmio-coverage).
         *
         * Real HW dumps at docs/hardware/hw_dump_vrc4173.txt in the
         * 0x00000200..0x00000234 rows show volatile non-zero values
         * consistent with a live display / LCD timing controller.
         * VRC4173 UM (docs/hardware/U14579EJ2V0UM00.pdf) has no
         * chapter covering offset 0x200; chapters are BCU/DMAAU/DCU/
         * CMU/ICU/GIU/PIU/AIU/KIU/PS2U/CARDU/USBU/AC97U only. This
         * appears to be a Casio-specific extension.
         *
         * We model bit 0 of 0x234 as a self-clearing "busy" flag
         * (instant-completion semantics — same pattern as ScCmcu at
         * 0x7800.. and PIUCNTREG bit 0 self-clear in Pass 25). Other
         * bits of 0x234 continue to latch on write / read-back so
         * future bit assignments do not silently break.
         *
         * TODO 2026-04-23: no VRC4173 UM citation for this block.
         * Confirm real-HW semantics via BEDiag or a captured boot
         * trace and either upgrade this to a full model or keep the
         * stub with a hardware citation.
         */
        if (off == 0x234u) {
            uint32_t raw = be300_latch_peek_u32(d, off);
            uint64_t val = (uint64_t)(raw & ~(uint32_t)1u);
            memory_writemax64(cpu, data, len, val);
            be300_probe_note_mmio("vrc4173-ddi-ctrl-busy", off, 'R',
                (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
                BE300_MMIO_CLASS_STUBBED);
            return 1;
        }

        /*
         * WORKAROUND: ScCmcu registers (Casio companion MCU,
         * PA 0x0A007800-0x0A00783F).
         *
         * The ROM MIPS16 boot dispatcher at 0x9FC00C20 writes a
         * command byte (e.g. 0x35) to register 0x7834, then polls
         * it until the value drops below 3 (meaning "command
         * complete").  Register 0x7800 follows the same pattern
         * with command 0x5C.
         *
         * On real hardware, a companion microcontroller receives
         * the command, processes it (e.g. power sequencing, battery
         * check), and clears the register when done.  We don't
         * emulate the companion MCU, so we return 0 on all reads
         * to simulate instant completion.  Without this, the ROM
         * poll loop spins forever waiting for the MCU to respond.
         */
        if (off >= 0x7800 && off < 0x7840) {
            be300_probe_note_mmio("vrc4173-scmcu-stub", off, 'R',
                (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
                BE300_MMIO_CLASS_STUBBED);
            memset(data, 0, len);  /* WORKAROUND: instant MCU completion */
        } else {
            be300_probe_note_mmio("vrc4173-latch", off, 'R',
                (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
                BE300_MMIO_CLASS_LATCHED);
            memcpy(data, &d->bytes[off], len);
        }
    }

    return 1;
}


bool be300_vrc4173_latch_read_u32(uint32_t pa, uint32_t *out)
{
    uint32_t off;

    if (!out || !g_be300_vrc4173_latch)
        return false;
    if (pa < (uint32_t)VRC4173_LATCH_BASE)
        return false;

    off = pa - (uint32_t)VRC4173_LATCH_BASE;
    if (off + 4u > VRC4173_LATCH_SIZE)
        return false;

    *out = (uint32_t)g_be300_vrc4173_latch->bytes[off + 0u]
         | ((uint32_t)g_be300_vrc4173_latch->bytes[off + 1u] << 8)
         | ((uint32_t)g_be300_vrc4173_latch->bytes[off + 2u] << 16)
         | ((uint32_t)g_be300_vrc4173_latch->bytes[off + 3u] << 24);
    return true;
}


/*
 *  be300_register_vrc4173_latch():
 *
 *  Register the VRC4173 catch-all latch as two non-overlapping segments
 *  that avoid the NAND device range (0x0A00A000-0x0A00D800) and the
 *  ns16550 SIU range (~0x0A008680).
 *
 *  Segment A: 0x0A000000 - 0x0A008000 (below SIU/NAND)
 *  Segment B: 0x0A00E000 - 0x0A020000 (above NAND)
 */
void be300_register_vrc4173_latch(struct machine *gxm, machine_t *m,
                                  bool log_mmio,
                                  bool enable_ppsh)
{
    struct be300_vrc4173_latch *latch;
    CHECK_ALLOCATION(latch = calloc(1, sizeof(struct be300_vrc4173_latch)));
    latch->log_mmio = log_mmio;
    g_be300_vrc4173_latch = latch;
    g_be300_machine = m;
    be300_register_wince_aux(gxm, log_mmio, enable_ppsh);
    be300_buzzer_seed(latch);
    latch->pcconnect_insert_delay_ms = be300_pcconnect_connect_delay_ms();
    be300_latch_poke_u32(latch, VRC4173_USB_OP_BASE + 0x00u,
        VRC4173_USB_HC_REVISION);

    /*
     * Pre-populate latch with real hardware register values from
     * hardware_survey/HardwareDump.txt and HardwareDump6.txt.
     * The OEMInit callbacks read these registers to initialize
     * display, touch, keyboard, audio, power, etc.
     */
    {
        /* VRC4173 Core (PA 0x0A000000, offset 0x0000, 64 words) */
        /* Offset 0x000 = PIUCNTREG: 0x002 = PIUPWR (hw dump, hardware.txt:239) */
        static const uint32_t core_regs[] = {
            0x00000002, 0x00000000, 0x00000001, 0x00000000,
            0x0000003C, 0x0000000C, 0x0000000C, 0x00000000,
            0x0000000C, 0x00000000, 0x0000000C, 0x0000000C,
            0x0000000C, 0x0000003C, 0x00000000, 0x00000000,
            0x0000000C, 0x00000000, 0x00000000, 0x00000000,
            0x00000000, 0x00000000, 0x00000000, 0x00000000,
            0x00000000, 0x00000000, 0x00000000, 0x00000000,
            0x00000000, 0x00000000, 0x00000000, 0x00000000,
            0x00000000, 0x00000000, 0x00000001, 0x00000000,
            0x0000000C, 0x0000000C, 0x0000000C, 0x00000000,
            0x0000000C, 0x00000000, 0x0000000C, 0x0000000C,
            0x0000000C, 0x0000003C, 0x00000000, 0x00000000,
            0x0000000C, 0x00000000, 0x00000000, 0x00000000,
            0x00000000, 0x00000000, 0x00000000, 0x00000000,
            0x00000000, 0x00000000, 0x00000000, 0x00000000,
            0x00000000, 0x00000000, 0x00000000, 0x00000000,
        };
        for (unsigned i = 0; i < sizeof(core_regs)/4; i++) {
            uint32_t v = core_regs[i];
            memcpy(&latch->bytes[i * 4], &v, 4);
        }

        /*
         * Audio/AIU-adjacent register snapshot from real hardware.
         *
         * The .NET wavedev driver maps these offsets through user VA
         * 0x001B0000 and expects the status bits seen in the hardware
         * survey instead of an all-zero block.
         */
        static const struct { uint16_t off; uint32_t val; } audio_regs[] = {
            { 0x0390, 0x00000E22 },
            { 0x0398, 0x0000DD00 },
            { 0x039C, 0x0000DD00 },
            { 0x03A8, 0x0000DD00 },
            { 0x03AC, 0x0000DD00 },
            { 0x03B0, 0x0000DD00 },
            { 0x03B4, 0x0000DD00 },
            { 0x03B8, 0x0000DD00 },
            { 0x03BC, 0x0000DD00 },
            { 0x03C0, 0x0000D007 },
            { 0x03C4, 0x00000001 },
            { 0x03C8, 0x00000003 },
            { 0x03CC, 0x00000000 },
            { 0x03D4, 0x00070000 },
            { 0x03F4, 0x00000007 },
            { 0x0880, 0x00000004 },
            { 0x0884, 0x00000001 },
            { 0x0888, 0x00000003 },
            { 0x088C, 0x00000001 },
            { 0x0890, 0x00000000 },
            { 0x0894, 0x00000088 },
            { 0x0898, 0x00000000 },
            { 0x089C, 0x00000000 },
            { 0x08A0, 0x00000000 },
            { 0x08A4, 0x00000000 },
            { 0x08A8, 0x00000000 },
            { 0x08AC, 0x00000004 },
            { 0x08B0, 0x0004D000 },
            { 0x08B4, 0x0004D5FC },
            { 0x08B8, 0x0004D000 },
            { 0x08BC, 0x0004D5FC },
            { 0x08C0, 0x00000001 },
            { 0x08C4, 0x00000001 },
            { 0x08C8, 0x00000001 },
            { 0x08CC, 0x00000001 },
            { 0x1114, 0x00000001 },
            { 0x1118, 0x00000001 },
            { 0x111C, 0x00000001 },
        };
        for (unsigned i = 0; i < sizeof(audio_regs)/sizeof(audio_regs[0]); i++) {
            uint32_t v = audio_regs[i].val;
            memcpy(&latch->bytes[audio_regs[i].off], &v, 4);
        }

        /* SIU/AIU area (PA 0x0A008000, offset 0x8000) */
        static const struct { uint16_t off; uint32_t val; } siu_regs[] = {
            { 0x8000, 0x0000200C },  /* master control */
            { 0x8004, 0x00001100 },  /* status */
            { 0x8010, 0x0000000C },  /* configuration */
            { 0x8014, 0x00000002 },  /* mode */
            { 0x8040, 0x0000000A },  /* interrupt mask */
        };
        for (unsigned i = 0; i < sizeof(siu_regs)/sizeof(siu_regs[0]); i++) {
            uint32_t v = siu_regs[i].val;
            memcpy(&latch->bytes[siu_regs[i].off], &v, 4);
        }

        /*
         * Sparse companion wake/interrupt latches from the hardware dump.
         *
         * The low-power helper at 0x80079898 toggles 0x1120, 0x112C,
         * and 0x1B20 around SUSPEND using 0/1 stores, so seed the
         * observed enable/state bits rather than leaving the whole page
         * zeroed.
         */
        static const struct { uint16_t off; uint32_t val; } wake_regs[] = {
            { 0x1120, 0x00000001 },
            { 0x1128, 0x00000001 },
            { 0x112C, 0x00000001 },
            { 0x1138, 0x00000001 },
            { 0x113C, 0x00000001 },
            { 0x1B10, 0x00000048 },
            { 0x1B14, 0x00000001 },
            { 0x1B20, 0x00000001 },
            { 0x1B2C, 0x00000001 },
        };
        for (unsigned i = 0; i < sizeof(wake_regs)/sizeof(wake_regs[0]); i++) {
            uint32_t v = wake_regs[i].val;
            memcpy(&latch->bytes[wake_regs[i].off], &v, 4);
        }

        /* Board ID: offset 0x0A0C0 = 0x7100 (BE-300 identifier) */
        {
            uint32_t v = 0x00007100;
            memcpy(&latch->bytes[0x0A0C0], &v, 4);
        }

        /*
         * Cold-boot WinCE later reaches a second alias of the companion
         * command block at 0x0C000120/0x0C000520. Real hardware surveys
         * show the same sparse 0,1,1,1 pattern at 0x0A000120/0x0A000520
         * — be300_register_wince_aux() seeded the matching wince_aux side.
         */
    }

    /*
     * Register latch segments that don't overlap with:
     *   touch input device  at 0x0A000300-0x0A000360 (carved from vrc4173_0)
     *   ns16550 SIU         at 0x0A008680 (32 bytes, addr_mult=4)
     *   NAND device         at 0x0A00A000-0x0A00D800
     *   button input device at 0x0A00A040-0x0A00A050 (carved from NAND gap)
     *
     * vrc4173_0 is split into two segments around the touch device range:
     *   vrc4173_0a: 0x0A000000..0x0A000300  (size 0x0300)
     *   vrc4173_0b: 0x0A000360..0x0A008680  (size 0x8320)
     *   Verify: 0x0A000360 + 0x8320 = 0x0A008680 = SIU base. No collision.
     */
    struct {
        const char *name;
        uint64_t    base;
        uint64_t    size;
        uint32_t    offset;
    } segs[] = {
        { "vrc4173_0a", 0x0A000000ULL, 0x0300,  0x0000 },  /* below touch regs */
        { "vrc4173_0b", 0x0A000360ULL, 0x8320,  0x0360 },  /* above touch regs, below SIU */
        { "vrc4173_1",  0x0A0086C0ULL, 0x1940,  0x86C0 },  /* SIU..NAND gap */
        { "vrc4173_2",  0x0A00E000ULL, 0x12000, 0xE000 },  /* above NAND */
    };

    for (int i = 0; i < 4; i++) {
        struct be300_vrc4173_segment *seg;
        CHECK_ALLOCATION(seg = malloc(sizeof(struct be300_vrc4173_segment)));
        seg->latch = latch;
        seg->offset_in_latch = segs[i].offset;
        memory_device_register(gxm->memory, segs[i].name,
            segs[i].base, segs[i].size,
            dev_be300_vrc4173_access, (void *)seg, DM_DEFAULT, NULL);
    }

    {
        char tmps[200];

        snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i.giu.%i",
            gxm->path, gxm->bootstrap_cpu, 8, 0);
        INTERRUPT_CONNECT(tmps, latch->cf_irq);
        latch->cf_irq_connected = true;
        latch->cf_irq_asserted = false;
    }

    if (m->cfg.enable_stowaway_keyboard || m->cfg.pcconnect_bridge) {
        char tmps[200];

        snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i.giu.%i",
            gxm->path, gxm->bootstrap_cpu, 8, 0);
        INTERRUPT_CONNECT(tmps, latch->pcconnect_irq);
        latch->pcconnect_irq_connected = true;
        latch->pcconnect_irq_asserted = false;
        if (m->cfg.pcconnect_bridge)
            pcconnect_set_rx_ready_callback(be300_pcconnect_uart_irq_ready,
                latch);
        if (m->cfg.enable_stowaway_keyboard)
            latch->pcconnect_dock_connected = true;
        be300_pcconnect_irq_update(latch);
    }

    (void)enable_ppsh;  /* device registration handled by be300_register_wince_aux above */
}
