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
#include "hw/cf.h"
#include "hw/nand.h"
#include "hw/siu.h"
#include "pcconnect.h"
#include "ppsh.h"
#include "ui.h"

#define BE300_NS_PER_MS 1000000ULL
#define BE300_KJGPIO_CARD_DETECT_OFF UINT32_C(0xA008)
#define BE300_KJGPIO_CARD_DETECT_INACTIVE UINT64_C(0x00000006)

static uint64_t be300_host_monotonic_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/*
 * --log-mmio: print one line per dispatched access. Volume-heavy by design
 * (every MMIO touch), matches the flag name in src/main.c usage text.
 */
static void be300_log_mmio(const char *name, int writeflag, uint32_t off,
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
 *  VRC4173 NAND flash controller device.
 *
 *  Covers the NAND register space at PA 0x0A00A000 - 0x0A00D800.
 *  This is registered with GXemul's memory subsystem so that memory
 *  accesses from the emulated CPU are dispatched here.
 */

struct be300_nand_device {
    nand_state_t *nand;
    cf_state_t   *cf;
    bool          log_mmio;
    uint32_t      reg_offset;   /* byte offset of this segment from 0x0A00A000 */
};

struct be300_cf_window_device {
    cf_state_t *cf;
    bool        log_mmio;
};

struct be300_companion_ab_device {
    cf_state_t *cf;
    bool        log_mmio;
    uint8_t     bytes[0x10000];
};

struct be300_vrc4173_latch;
static struct be300_vrc4173_latch *g_be300_vrc4173_latch;
static void be300_cf_irq_update(struct be300_vrc4173_latch *d);
static void be300_pcconnect_reset_for_cpu_reset(
    struct be300_vrc4173_latch *d);

DEVICE_ACCESS(be300_nand)
{
    struct be300_nand_device *d = (struct be300_nand_device *)extra;
    uint32_t offset = (uint32_t)relative_addr + 0xA000 + d->reg_offset;
    uint32_t pc = (uint32_t)cpu->pc;

    if (d->log_mmio)
        be300_log_mmio("be300_nand", writeflag, offset, (unsigned)len, data, pc);

    be300_probe_note_mmio("vrc4173-nand", offset,
        writeflag == MEM_WRITE ? 'W' : 'R',
        (uint32_t)len, (uint64_t)pc, BE300_MMIO_CLASS_KNOWN);

    /*
     * pcmcia.dll enables a runtime card-memory window by writing bit 2 to
     * PA 0x0A00A01C immediately before scanning PA 0x0A00D000 for CIS
     * tuples (observed at PC 0x0198C048).  The bridge register name is
     * still unknown, but once this bit is set the D000/C000 pages decode
     * as PCMCIA card windows rather than ROM-era NAND direct-I/O.  This
     * must happen even with no CF image attached so the no-media path sees
     * the real pulled-up card bus documented in
     * docs/HARDWARE_GROUND_TRUTH.md:72-79.
     *
     * TODO(2026-04-26): replace this with named BE-300/VRC4173 PCMCIA
     * bridge registers when the map is identified.
     */
    if (writeflag == MEM_WRITE && d->cf &&
        offset == 0xA01Cu && len == 4) {
        uint64_t val = memory_readmax64(cpu, data, len);
        if (val & 0x04u)
            cf_note_pcmcia_window_enable(d->cf);
    }

    /*
     * KjCMU warm-reset trigger at PA 0x0A00A0C4 + 0x0A00A0C8.
     *
     * Citation chain (TODO 2026-04-20: confirm via VRC4173 UM once the
     * KjCMU register map is located — the Casio "Kj" companion block is
     * not described in NEC's U14579EJ2V0UM00):
     *   - docs/hardware/hardware.txt:192 identifies PA 0x0A00A000 as
     *     "KjCMU Base on companion" (Casio custom block, clock/reset
     *     control is the conventional role of a CMU-adjacent region).
     *   - docs/hardware/hw_dump_vrc4173.txt:524 shows real-hw quiescent
     *     values PA 0x0A00A0C4=0x3 and PA 0x0A00A0C8=0xFFFF, distinct
     *     from the magic pair NK writes (7 and 10) — consistent with
     *     "write magic => trigger => register returns to quiescent
     *     state" semantics rather than plain latches.
     *   - NK 0x8007A140..0x8007A178 (FUN_8007A140) writes
     *     `7 -> 0xA0C4` then `10 -> 0xA0C8` and falls through into
     *     `jal 0x8007A178` -- the *only* self-jal in all 6 MB of NK
     *     (Pass 30 handoff §1.3). This is the canonical WinCE
     *     halt-forever-waiting-for-reset idiom; without the warm-reset
     *     the CPU spins 555M times in 40 s at that PC (Pass 30 §1.1)
     *     and Boot.exe's CASIO reboot path never advances.
     *
     * We require the exact two-write sequence (7->0xA0C4 immediately
     * followed by 10->0xA0C8) to fire the reset; any other write to
     * 0xA0C4 disarms. mips_cpu_cold_reset(cpu) reinitialises CP0 per
     * the cold-boot state (STATUS=BEV|ERL, PC=0xBFC00000) -- the same
     * helper the PMU SOFTRST path uses (gxemul/src/devices/dev_vr41xx.c
     * :845-853). After the reset, ROM re-runs, the second invocation
     * of Boot.exe sees \Windows\Initialized.$$$ (already created before
     * the reboot call -- Pass 28), takes the already-initialised branch,
     * and signals 0x3B ready.
     */
    if (writeflag == MEM_WRITE && (offset == 0xA0C4u || offset == 0xA0C8u)) {
        static int kjcmu_reset_armed = 0;
        uint64_t val = memory_readmax64(cpu, data, len);
        if (offset == 0xA0C4u) {
            kjcmu_reset_armed = (val == 7);
        } else if (kjcmu_reset_armed && val == 10) {
            kjcmu_reset_armed = 0;
            fprintf(stderr,
                "[KjCMU] warm reset triggered at pc=%08x "
                "(PA 0x0A00A0C4<=7, PA 0x0A00A0C8<=10)\n",
                pc);
            be300_pcconnect_reset_for_cpu_reset(g_be300_vrc4173_latch);
            mips_cpu_cold_reset(cpu);
            return 1;
        } else {
            kjcmu_reset_armed = 0;
        }
        /* fall through to the normal latch path for read-back consistency */
    }

    if (offset >= 0xA03Cu && offset < 0xA040u && d->cf) {
        if (writeflag == MEM_WRITE) {
            uint64_t val = memory_readmax64(cpu, data, len);
            if (val == 0 || (val & 0x22u) == 0)
                cf_clear_irq(d->cf);
            be300_cf_irq_update(g_be300_vrc4173_latch);
        } else {
            uint64_t val = cf_card_state_bits(d->cf);
            memory_writemax64(cpu, data, len, val);
            /*
             * The GIRQ0-0 dispatcher (hardware.txt:91-101) inspects bits
             * 0x22 once to dispatch SYSINTR_PCMCIA_STATE; the edge cause
             * does not persist across reads on real hardware (only the
             * card-present level bit 0 stays set).  Clear the
             * state-change pending flag after the cause has been read so
             * subsequent dispatcher passes see level-only and the GIU
             * source bit deasserts via cf_giu_source_bits.
             */
            cf_consume_state_change(d->cf);
            be300_cf_irq_update(g_be300_vrc4173_latch);
        }
        return 1;
    }

    /*
     * PCMCIA card attribute-memory alias at PA 0x0A00D000.
     *
     * The real inserted-card VRC4173 survey calls out a detailed dump at
     * PA 0x0A00D000 (docs/hardware/hw_dump_vrc4173.txt:13), and WinCE
     * pcmcia.dll programs socket 0's first 4 KB card window to
     * 0x0A00D000-0x0A00DFFF (runtime PC 0x0198C5D8..0x0198C618).
     * CardGetFirstTuple then reads it through the byte-wide
     * attribute-memory helper, which applies the PC Card every-other-byte
     * stride before looking for CISTPL_FUNCID.
     *
     * The same physical page is also the ROM-era NAND direct-I/O window
     * during cold boot, so keep NAND ownership until pcmcia.dll has
     * initialized the CF companion block.  Once that happens, the bridge
     * decode belongs to the card window and reads expose the CF CIS; writes
     * are ignored because the seeded CIS is ROM-like attribute memory.
     *
     * TODO(2026-04-26): replace this decode handoff with a named model of
     * the BE-300/VRC4173 PCMCIA window-translation registers once the
     * companion bridge map is identified.
     */
    if (d->cf && cf_pcmcia_windows_enabled(d->cf) &&
        offset >= 0xD000u && offset < 0xD800u) {
        if (writeflag == MEM_READ) {
            uint64_t val = cf_cis_read(d->cf, offset - 0xD000u,
                (unsigned)len);
            memory_writemax64(cpu, data, len, val);
        }
        return 1;
    }

    /*
     * PCMCIA card I/O window at PA 0x0A00C000.
     *
     * Real-hardware diffs show activity at PA 0x0A00C170 on the VRC4173
     * C000 page (docs/hardware/hw_dump_diffs.txt:1538), and the ROM restore
     * path already uses 0x0A00C170/0x0A00C376 as the CF ATA taskfile window.
     * WinCE pcmcia.dll maps the same host page with VirtualCopy during
     * CardMapWindow; atadisk.dll then polls the secondary ATA status byte at
     * mapped offset 0x177 and the alternate-status byte at 0x376.
     *
     * Do this only after pcmcia.dll has enabled card windows so cold-boot
     * NAND direct-I/O and restore-boot CF visibility keep their existing
     * semantics.  The restore path's boot-visible gate intentionally models
     * ROM/NANDWRITER discovery; the runtime PCMCIA card I/O window must
     * expose the inserted CF taskfile directly.
     *
     * TODO(2026-04-26): fold this into a named BE-300/VRC4173 PCMCIA bridge
     * model once the socket window-translation register layout is identified.
     */
    if (d->cf && cf_pcmcia_windows_enabled(d->cf) &&
        cf_is_ne2000(d->cf) &&
        offset >= 0xC000u && offset < 0xC400u) {
        if (writeflag == MEM_WRITE) {
            uint64_t val = memory_readmax64(cpu, data, len);
            cf_window_write(d->cf, offset, (unsigned)len, val);
        } else {
            uint64_t val = cf_window_read(d->cf, offset, (unsigned)len);
            memory_writemax64(cpu, data, len, val);
        }
        return 1;
    }

    if (d->cf && cf_pcmcia_windows_enabled(d->cf) &&
        ((offset >= 0xC170u && offset < 0xC178u) ||
         offset == 0xC376u)) {
        if (writeflag == MEM_WRITE) {
            uint64_t val = memory_readmax64(cpu, data, len);
            cf_window_write(d->cf, offset, (unsigned)len, val);
        } else {
            uint64_t val = cf_window_read(d->cf, offset, (unsigned)len);
            memory_writemax64(cpu, data, len, val);
        }
        return 1;
    }

    if (d->cf && cf_boot_handles_rom_offset(d->cf, offset)) {
        if (writeflag == MEM_WRITE) {
            uint64_t val = memory_readmax64(cpu, data, len);
            cf_boot_write(d->cf, offset, (unsigned)len, val);
        } else {
            uint64_t val = cf_boot_read(d->cf, offset, (unsigned)len);
            memory_writemax64(cpu, data, len, val);
        }
        return 1;
    }

    if (writeflag == MEM_WRITE) {
        uint64_t val = memory_readmax64(cpu, data, len);
        nand_write(d->nand, offset, (unsigned)len, val, pc);
    } else {
        uint64_t val = nand_read(d->nand, offset, (unsigned)len, pc);
        /*
         * PA 0x0A00A008 is also sampled by pcmcia.dll's socket-0
         * CardGetStatus callback at UM 0x0198C3C8.  The inserted-card
         * hardware dump shows 0x2F9 here, with detect bits 1/2 clear
         * (docs/hardware/hw_dump_vrc4173.txt:516).  With no host CF image,
         * those active-low detect inputs must read inactive so card_ex.dll
         * does not publish a card-present status-bar notification.
         */
        if (d->cf && !cf_present(d->cf) && len > 0 &&
            offset <= BE300_KJGPIO_CARD_DETECT_OFF &&
            offset + (uint32_t)len > BE300_KJGPIO_CARD_DETECT_OFF) {
            unsigned shift =
                (unsigned)((BE300_KJGPIO_CARD_DETECT_OFF - offset) * 8u);
            val |= BE300_KJGPIO_CARD_DETECT_INACTIVE << shift;
        }
        memory_writemax64(cpu, data, len, val);
    }

    return 1;
}

DEVICE_ACCESS(be300_cf_window)
{
    struct be300_cf_window_device *d =
        (struct be300_cf_window_device *)extra;
    uint32_t offset = (uint32_t)relative_addr;

    if (d->log_mmio)
        be300_log_mmio("be300_cf_window", writeflag, offset,
            (unsigned)len, data, (uint32_t)cpu->pc);

    be300_probe_note_mmio("cf-window", offset,
        writeflag == MEM_WRITE ? 'W' : 'R',
        (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
        BE300_MMIO_CLASS_KNOWN);

    if (writeflag == MEM_WRITE) {
        uint64_t val = memory_readmax64(cpu, data, len);
        cf_window_write(d->cf, offset, (unsigned)len, val);
    } else {
        uint64_t val = cf_window_read(d->cf, offset, (unsigned)len);
        memory_writemax64(cpu, data, len, val);
    }

    return 1;
}

static bool companion_ab_no_card_status_byte(uint32_t off)
{
    switch (off) {
    case 0x0108u:
    case 0x010Cu:
    case 0x0110u:
    case 0x0114u:
    case 0x0118u:
        return true;
    default:
        return false;
    }
}

static bool companion_ab_no_card_jacket_present_byte(uint32_t off)
{
    return off == 0x0150u;
}

DEVICE_ACCESS(be300_companion_ab)
{
    struct be300_companion_ab_device *d =
        (struct be300_companion_ab_device *)extra;
    uint32_t off = (uint32_t)relative_addr;

    if (!d || off + len > sizeof(d->bytes))
        return 0;

    if (d->log_mmio)
        be300_log_mmio("be300_companion_ab", writeflag, off,
            (unsigned)len, data, (uint32_t)cpu->pc);

    if (writeflag == MEM_WRITE) {
        be300_probe_note_mmio("vrc4173-companion-ab", off, 'W',
            (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
            BE300_MMIO_CLASS_LATCHED);
        memcpy(&d->bytes[off], data, len);
    } else {
        uint8_t buf[8];

        if (len > sizeof(buf))
            return 0;

        memcpy(buf, &d->bytes[off], len);

        /*
         * The attached CF card is reported by the primary companion CF
         * status page at 0x0A001000 and by the card attribute window.
         * pcmcia.dll also maps PA 0x0B000100 as another socket status
         * block, and card_ex.dll treats bit 6 clear in these status bytes
         * as card media present.  Keep this secondary block in settled
         * no-card state even when --cf is attached; otherwise the guest
         * sees a second synthetic card and opens the "Unidentified PCCard
         * Adapter" prompt for Socket 1.  Bits 4 and 5 are state-change
         * bits in pcmcia.dll's socket worker, so clear them instead of
         * preserving the guest-written latch value.
         *
         * PA 0x0B000150 is the exception: pcmcia.dll's PDJacketGetState
         * path at UM 0x0198ACF0 treats bit 6 set there as jacket/card
         * state bit 0x4.  Keep that bit clear so no --cf boots model an
         * absent secondary adapter, not an unknown card insertion.
         *
         * TODO(2026-04-25): replace this with named VRC4173/BE-300
         * PCMCIA bridge semantics once the secondary companion socket map
         * is identified.
         */
        for (size_t i = 0; i < len; i++) {
            if (companion_ab_no_card_status_byte(off + (uint32_t)i))
                buf[i] = (uint8_t)((buf[i] & ~(uint8_t)0x30u) | 0x40u);
            if (companion_ab_no_card_jacket_present_byte(off + (uint32_t)i))
                buf[i] &= ~(uint8_t)0x70u;
        }

        be300_probe_note_mmio("vrc4173-companion-ab", off, 'R',
            (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
            BE300_MMIO_CLASS_STUBBED);
        memcpy(data, buf, len);
    }

    return 1;
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
    bool     pcconnect_insert_armed;
    uint32_t pcconnect_insert_delay_ms;
    uint64_t pcconnect_insert_deadline_ns;
};

static machine_t *g_be300_machine = NULL;  /* for PIU cross-device callback */

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

static void be300_cf_irq_update(struct be300_vrc4173_latch *d)
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
#define BE300_COMMSIU_CTRL_RS232   0x0008u
#define BE300_PCC_CONNECT_DELAY_DEFAULT_MS 1000u

static bool be300_pcconnect_cable_enabled(void)
{
    return g_be300_machine &&
        g_be300_machine->cfg.enable_pcconnect_time_sync;
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
    if (!d || !be300_pcconnect_cable_enabled() || !d->pcconnect_insert_armed)
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

    if (be300_pcconnect_cable_enabled()) {
        /*
         * socket.dll maps the Vic/CommMode page, then reads
         * ReadPortDataEx(0, 2, 0x1f) from AA008010.  The WinCE 3.0
         * socket table maps raw 0x0007 to an empty no-driver entry,
         * raw 0x0008 to serial.dll, and raw 0x000c/0x000d to USB/VCom
         * entries.  hardware.txt:88-102 documents AA008004 as the
         * CommMode GIRQ0-4 pending/mask register, and hardware.txt:189-191
         * places this page next to the companion SIU.  For the serial
         * PC Connect option, do not expose a socket until the emulated
         * cable edge; after that edge, expose the RS-232 socket.  Preserve
         * the other latched bits.
         */
        v &= (uint16_t)~BE300_COMMMODE_SOCKET_VALUE_MASK;
        v |= d->pcconnect_dock_connected ?
            BE300_COMMMODE_SOCKET_RS232 : BE300_COMMMODE_SOCKET_NONE;
    }

    return v;
}

static uint64_t be300_pcconnect_pccard_status_read(
    const struct be300_vrc4173_latch *d, bool cf_attached, uint32_t off,
    unsigned len, uint64_t val)
{
    unsigned shift;

    if (!d || !be300_pcconnect_cable_enabled() ||
        cf_attached || !d->pcconnect_dock_connected || len == 0 ||
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

    want = be300_pcconnect_cable_enabled() &&
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
    if (!be300_pcconnect_cable_enabled() ||
        !be300_pcconnect_commmode_unmasked(d))
        return;

    INTERRUPT_DEASSERT(d->pcconnect_irq);
    INTERRUPT_ASSERT(d->pcconnect_irq);
}

static void be300_pcconnect_arm_insert_after_reset(
    struct be300_vrc4173_latch *d)
{
    if (!d || !be300_pcconnect_cable_enabled())
        return;

    d->pcconnect_insert_armed = true;
    d->pcconnect_insert_deadline_ns = be300_host_monotonic_ns() +
        (uint64_t)d->pcconnect_insert_delay_ms * BE300_NS_PER_MS;
}

static void be300_pcconnect_reset_for_cpu_reset(
    struct be300_vrc4173_latch *d)
{
    if (!d)
        return;
    if (!be300_pcconnect_cable_enabled())
        return;

    /*
     * KjCMU resets the CPU while the VRC4173-side latch state remains in
     * host memory.  The PC Connect option represents a host-side cable
     * insertion, so keep the reboot path as "not docked" and schedule the
     * cable edge after the normal Boot.exe reset.  This avoids presenting
     * an already-docked CommMode state to the second-boot OAL, which takes
     * the software-shutdown/HIBERNATE path before user PC Connect monitors
     * exist.
    */
    d->pcconnect_dock_connected = false;
    d->pcconnect_commmode_pending = 0;
    pcconnect_set_cable_connected(false);
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
    if (!d || !be300_pcconnect_cable_enabled() ||
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
    d->pcconnect_commmode_pending |= BE300_COMMMODE_PENDING_MASK;
    pcconnect_set_cable_connected(true);
    be300_pcconnect_irq_update(d);
    be300_pcconnect_trace(d, force ? "dock-edge-uart" : "dock-edge",
        BE300_COMMMODE_STATUS_OFF, 2, be300_pcconnect_commmode_read(d), 0);
}

static void be300_pcconnect_maybe_raise_dock_edge(
    struct be300_vrc4173_latch *d)
{
    if (!d || d->pcconnect_dock_connected)
        return;

    be300_pcconnect_raise_dock_edge(d, pcconnect_guest_uart_ready());
}

void be300_pcconnect_poll(void)
{
    if (!be300_pcconnect_cable_enabled())
        return;

    be300_pcconnect_maybe_raise_dock_edge(g_be300_vrc4173_latch);
}

static void be300_pcconnect_uart_rx_ready(void *opaque)
{
    struct be300_vrc4173_latch *d = opaque;

    if (!d || !be300_pcconnect_cable_enabled() ||
        !d->pcconnect_dock_connected)
        return;

    /*
     * The companion serial path is not the VR4131 internal SIU path:
     * hardware.txt:8 notes serial is handled by the custom companion, and
     * hardware.txt:122-130 routes the Vic/CommMode page through GIRQ0-4.
     * Raise a CommMode edge when host RX data becomes available so the
     * guest's serial-side waiters get a companion interrupt after the
     * initial dock-detect edge.
     */
    d->pcconnect_commmode_pending |= BE300_COMMMODE_MODEM_PENDING;
    be300_pcconnect_trace(d, "uart-rx-edge",
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
    if (!be300_pcconnect_cable_enabled())
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
     * AA008004 sub-bit.  Sub-bit 4's handler returns SYSINTR 0x23 without
     * writing AA008004, so consume the latched modem-detect bit when the
     * dispatcher observes it as the selected source.  Sub-bit 0 has an
     * explicit writeback in its handler and is cleared in the write path.
     */
    if (pc == 0x800b6db4u &&
        (active & BE300_COMMMODE_SOCKET_PENDING) == 0 &&
        (active & BE300_COMMMODE_MODEM_PENDING) != 0) {
        d->pcconnect_commmode_pending &=
            (uint16_t)~BE300_COMMMODE_MODEM_PENDING;
        be300_pcconnect_trace(d, "commmode-modem-dispatch",
            BE300_COMMMODE_STATUS_OFF, 2, val, pc);
        be300_pcconnect_irq_update(d);
    }
}

static uint32_t be300_pcconnect_girq0_source_bits(
    const struct be300_vrc4173_latch *d)
{
    if (!be300_pcconnect_cable_enabled())
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
    uint32_t old_cmm;
    uint32_t new_cmm;
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
    old_cmm = be300_buzzer_peek_le32(b->cmm);

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
    new_cmm = be300_buzzer_peek_le32(b->cmm);

    if ((new_control & 1u) && !(old_control & 1u)) {
        ui_buzzer_pulse(be300_buzzer_tone_hz(b), be300_buzzer_tone_ms(b));
        return;
    }

    if ((new_control & 1u) && touched_blg &&
        !be300_buzzer_range_overlap(off, len, 0x098Cu, 4u)) {
        ui_buzzer_pulse(be300_buzzer_tone_hz(b), be300_buzzer_tone_ms(b));
        return;
    }

    if (touched_cmm && new_cmm != old_cmm && (new_cmm & 1u))
        ui_buzzer_pulse(be300_buzzer_tone_hz(b), be300_buzzer_tone_ms(b));
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
    memcpy(dst, src, len);
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
     * TODO 2026-04-25: capture BEDiag traces for the remaining non-zero
     * modes; copy/ROP modes in this block are deliberately left
     * unimplemented until observed on the boot path with enough register
     * evidence.
     */
    if (width == 0 || height == 0 ||
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
    }

complete:
    d->bytes[0x0234] &= ~(uint8_t)1u;   /* trigger/busy complete */
}

struct be300_vrc4173_segment {
    struct be300_vrc4173_latch *latch;
    uint32_t offset_in_latch;    /* offset of this segment within the latch */
};

/* Forward declarations for cross-device callbacks */
struct be300_input_device;
static uint32_t piu_girq0_source_bits(const struct be300_input_device *d);
static void piu_update_state(struct be300_input_device *d);
static uint32_t button_girq0_source_bits(const struct be300_input_device *d);
static void button_ack_keyboard_source(struct be300_input_device *d);
#define BUTTON_GIRQ0_SOURCE     0x00000002u

#define WINCE_AUX_BASE  0x0C000120ULL
#define WINCE_AUX_SIZE  0x00000500u
#define PPSH_QUEUE_CAP  4096u
#define PPSH_TEXT_RUN_CAP 128u

struct be300_wince_aux {
    uint8_t bytes[WINCE_AUX_SIZE];
    bool    log_mmio;
    uint16_t ppsh_status_520;
    uint16_t ppsh_data;
         /* response data at offset 0x000 */
    bool     ppsh_response_pending; /* true after cmd dispatch until data read */
    bool     ppsh_enabled;    /* --ppsh: enable PPSH debug shell probe */
    bool     ppsh_ack_pending;
    bool     ppsh_tx_valid;
    uint32_t ppsh_idle_poll_count;
    bool     ppsh_text_emitting;
    uint8_t  ppsh_tx_byte;
    uint8_t  ppsh_host_queue[PPSH_QUEUE_CAP];
    size_t   ppsh_host_q_head;
    size_t   ppsh_host_q_tail;
    size_t   ppsh_host_q_count;
    size_t   ppsh_guest_byte_count;
    size_t   ppsh_host_byte_count;
    size_t   ppsh_raw_log_count;
    char     ppsh_text_run[PPSH_TEXT_RUN_CAP];
    size_t   ppsh_text_run_len;
};

static struct be300_wince_aux *g_be300_wince_aux = NULL;

static bool ppsh_is_printable(uint8_t byte)
{
    return (byte >= 0x20 && byte <= 0x7eu)
        || byte == '\r'
        || byte == '\n'
        || byte == '\t';
}

static void ppsh_refresh_status(struct be300_wince_aux *d)
{
    uint16_t status;

    if (!d)
        return;

    if (!d->ppsh_enabled) {
        d->ppsh_status_520 = 0x0000;
        memcpy(&d->bytes[0x400], &d->ppsh_status_520, sizeof(d->ppsh_status_520));
        return;
    }

    status = d->ppsh_response_pending ? 0x2322u : 0x2320u;
    if (d->ppsh_ack_pending || d->ppsh_host_q_count > 0)
        status |= 0x1000u;

    d->ppsh_status_520 = status;
    memcpy(&d->bytes[0x400], &d->ppsh_status_520, sizeof(d->ppsh_status_520));
}

static void ppsh_text_break(struct be300_wince_aux *d)
{
    if (!d)
        return;

    d->ppsh_text_run_len = 0;
    d->ppsh_text_emitting = false;
}

static void ppsh_guest_note_text_byte(struct be300_wince_aux *d, uint8_t byte)
{
    if (!d)
        return;

    if (!ppsh_is_printable(byte)) {
        ppsh_text_break(d);
        return;
    }

    if (d->ppsh_text_run_len >= PPSH_TEXT_RUN_CAP - 1)
        ppsh_text_break(d);

    d->ppsh_text_run[d->ppsh_text_run_len++] = (char)byte;
    d->ppsh_text_run[d->ppsh_text_run_len] = '\0';

    if (!d->ppsh_text_emitting && d->ppsh_text_run_len >= 4) {
        fwrite(d->ppsh_text_run, 1, d->ppsh_text_run_len, stdout);
        fflush(stdout);
        d->ppsh_text_run_len = 0;
        d->ppsh_text_emitting = true;
        return;
    }

    if (d->ppsh_text_emitting) {
        fputc(byte, stdout);
        fflush(stdout);
        d->ppsh_text_run_len = 0;
    }
}

static void ppsh_guest_submit_byte(struct be300_wince_aux *d, uint8_t byte,
    uint32_t pc)
{
    if (!d)
        return;

    d->ppsh_guest_byte_count++;
    (void)pc;

    ppsh_guest_note_text_byte(d, byte);
}

static size_t ppsh_queue_host_bytes(struct be300_wince_aux *d,
    const uint8_t *buf, size_t len)
{
    size_t queued = 0;

    if (!d || !d->ppsh_enabled || !buf)
        return 0;

    while (queued < len && d->ppsh_host_q_count < PPSH_QUEUE_CAP) {
        d->ppsh_host_queue[d->ppsh_host_q_head] = buf[queued];
        d->ppsh_host_q_head = (d->ppsh_host_q_head + 1u) % PPSH_QUEUE_CAP;
        d->ppsh_host_q_count++;
        queued++;
    }

    if (queued > 0)
        d->ppsh_host_byte_count += queued;

    ppsh_refresh_status(d);
    return queued;
}

static bool ppsh_pop_host_byte(struct be300_wince_aux *d, uint8_t *out)
{
    if (!d || !out || d->ppsh_host_q_count == 0)
        return false;

    *out = d->ppsh_host_queue[d->ppsh_host_q_tail];
    d->ppsh_host_q_tail = (d->ppsh_host_q_tail + 1u) % PPSH_QUEUE_CAP;
    d->ppsh_host_q_count--;
    return true;
}

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

        if (be300_pcconnect_cable_enabled() &&
            off == BE300_COMMMODE_STATUS_OFF) {
            uint16_t val;

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

        if (be300_pcconnect_cable_enabled() &&
            off == BE300_COMMMODE_SOCKET_OFF) {
            uint16_t val;

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

DEVICE_ACCESS(be300_wince_aux)
{
    struct be300_wince_aux *d = (struct be300_wince_aux *)extra;
    uint32_t off = (uint32_t)relative_addr;
    uint64_t val;

    if (off + len > WINCE_AUX_SIZE)
        return 0;

    if (d->log_mmio)
        be300_log_mmio("be300_wince_aux", writeflag, off, (unsigned)len,
            data, (uint32_t)cpu->pc);

    be300_probe_note_mmio(d->ppsh_enabled ? "ppsh-active" : "ppsh-stub",
        off, writeflag == MEM_WRITE ? 'W' : 'R', (uint32_t)len,
        (uint64_t)(uint32_t)cpu->pc,
        d->ppsh_enabled ? BE300_MMIO_CLASS_KNOWN : BE300_MMIO_CLASS_STUBBED);

    /*
     * When PPSH is disabled, model the parallel port controller as
     * present but with no host connected. On real BE-300 hardware
     * the VRC4173 parallel port registers are always readable —
     * only the absence of a debug HOST causes the protocol probe
     * to fail, not the absence of the hardware itself.
     *
     * Level-1 (offset 0x004): write-readback test against the
     * bytes[] array. Returns whatever was last written, which
     * won't match 0x2320 → Level-1 fails cleanly.
     *
     * Level-2 (offsets 0x000 / 0x400): writes are absorbed.
     * Reads from offset 0x400 return 0x2320 ("ready") after a
     * small number of polls, simulating the controller completing
     * the command internally. Reads from offset 0x000 return 0
     * (no data from host). The protocol probe sees the "ready"
     * status but wrong data → decides "no PPFS" quickly instead
     * of spinning through millions of timeout iterations.
     */
    if (!d->ppsh_enabled && (off == 0x000 || off == 0x400)) {
        if (writeflag == MEM_WRITE) {
            memcpy(&d->bytes[off], data, len);
            if (off == 0x400 && d->ppsh_idle_poll_count < 32)
                d->ppsh_idle_poll_count = 0;
            return 1;
        }
        if (off == 0x400) {
            d->ppsh_idle_poll_count++;
            uint16_t status = 0x2320;
            data[0] = status & 0xFF;
            if (len > 1) data[1] = (status >> 8) & 0xFF;
            return 1;
        }
        data[0] = 0xFF;
        if (len > 1) data[1] = 0xFF;
        for (size_t i = 2; i < len; i++) data[i] = 0xFF;
        return 1;
    }

    if (writeflag == MEM_WRITE) {
        val = memory_readmax64(cpu, data, len);
        memcpy(&d->bytes[off], data, len);

        if (off == 0x000) {
            d->ppsh_data = (uint16_t)val;
            d->ppsh_tx_byte = (uint8_t)(val & 0xffu);
            d->ppsh_tx_valid = true;
        }

        /*
         * NK's PPSH companion-MCU handshake uses 0x0C000520 as a small
         * status machine, not a constant ID register:
         *   0x3330: controller-ID probe, expect 0x2320 bits
         *   0x1100: command dispatch, wait until bit 1 becomes set
         *   0x9100/0x9900: completion phase, wait until bit 1 clears
         *
         * Model the observed status transitions directly so the guest can
         * drive the same poll loops as on hardware.
         */
        if (off == 0x400 && len >= 2) {
            uint16_t cmd = (uint16_t)val;
            switch (cmd) {
            case 0x3330:
                ppsh_refresh_status(d);
                break;
            case 0x1100:
                d->ppsh_response_pending = true;
                if (d->ppsh_enabled && d->ppsh_tx_valid) {
                    ppsh_guest_submit_byte(d, d->ppsh_tx_byte,
                        (uint32_t)cpu->pc);
                }
                d->ppsh_tx_valid = false;
                ppsh_refresh_status(d);
                break;
            case 0x9100:
                d->ppsh_response_pending = true;
                ppsh_refresh_status(d);
                break;
            case 0x9900:
                d->ppsh_response_pending = false;
                d->ppsh_ack_pending = true;
                ppsh_refresh_status(d);
                break;
            default:
                d->ppsh_response_pending = false;
                ppsh_refresh_status(d);
                break;
            }
        }

    } else {
        memcpy(data, &d->bytes[off], len);
        val = memory_readmax64(cpu, data, len);

        /*
         * PPSH data register at offset 0x000 (PA 0x0C000120).
         * Returns response data; reading clears bit 0x1000 (data_avail)
         * from the status register, matching real MCU mailbox semantics.
         */
        if (off == 0x000) {
            uint16_t word = d->ppsh_data;
            uint8_t byte = 0;

            if (d->ppsh_ack_pending) {
                word = 0x0100;
                d->ppsh_ack_pending = false;
            } else if (ppsh_pop_host_byte(d, &byte)) {
                word = (uint16_t)byte << 8;
            }

            if (len >= 2)
                memory_writemax64(cpu, data, len, word);
            else
                memory_writemax64(cpu, data, len, (word >> 8) & 0xffu);
            val = len >= 2 ? word : ((word >> 8) & 0xffu);
            ppsh_refresh_status(d);
        }

        /*
         * PPSH status register at offset 0x400 (PA 0x0C000520).
         * Reads return the current emulated companion-MCU status word.
         */
        if (off == 0x400 && len >= 2) {
            ppsh_refresh_status(d);
            val = d->ppsh_status_520;
            memory_writemax64(cpu, data, len, val);
        }
    }
    (void)val;

    return 1;
}

bool be300_ppsh_transport_ready(void)
{
    return g_be300_wince_aux
        && g_be300_wince_aux->ppsh_enabled;
}

size_t be300_ppsh_queue_host_input(const uint8_t *buf, size_t len)
{
    return ppsh_queue_host_bytes(g_be300_wince_aux, buf, len);
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
 *  VR4131 SIU / DSIU extension registers (hardware.txt:201-202,
 *  VR4131 UM §20 / §22).
 *
 *  hardware.txt lists:
 *      0xaf000800  SIU Base on vr4131
 *      0xaf000820  DSIU Base on vr4131
 *
 *  dev_vr41xx.c caps its own claimed range at DEV_VR41XX_LENGTH=0x800
 *  and then registers two ns16550 devices (at baseaddr+0x800 for SIU
 *  and baseaddr+0x820 for DSIU). Both are DEV_NS16550_LENGTH=8 with
 *  default addr_mult=1, so the ns16550s cover only offsets 0x00..0x07
 *  of each block.
 *
 *  The VR4131 SIU/DSIU actually extend to offset 0x0E (SCR) on SIU and
 *  further on DSIU (per VR4131 UM). gwes.exe driver chain at UM
 *  0x0194xxxx pokes MCR at SIU+0x08, LSR at SIU+0x0A, and DSIU+0x38 —
 *  all beyond the ns16550 8-byte window — and gets "non-existent paddr"
 *  warnings from gxemul core.
 *
 *  Pass 22 fix: register our own device windows for the ranges the
 *  ns16550 does not cover. SIU extensions (+0x08..+0x1F) are dispatched
 *  to siu_read/siu_write (which uses 2-byte stride — see
 *  `offset &= ~1u` in siu.c). DSIU extensions (+0x08..+0x5F) are
 *  stubbed: reads return 0, writes are accepted silently. If a driver
 *  polls a DSIU status bit that doesn't naturally clear, a later pass
 *  can expand beyond a stub.
 */

struct be300_siu_device {
    siu_state_t *siu;
    uint32_t     base_offset; /* added to relative_addr before siu_read/write */
    bool         log_mmio;
};

DEVICE_ACCESS(be300_vr4131_siu)
{
    struct be300_siu_device *d = (struct be300_siu_device *)extra;
    uint32_t siu_offset = d->base_offset + (uint32_t)relative_addr;
    uint32_t pc = (uint32_t)cpu->pc;

    if (d->log_mmio)
        be300_log_mmio("be300_vr4131_siu", writeflag, siu_offset,
            (unsigned)len, data, pc);

    be300_probe_note_mmio("vr4131-siu-ext", siu_offset,
        writeflag == MEM_WRITE ? 'W' : 'R',
        (uint32_t)len, (uint64_t)pc, BE300_MMIO_CLASS_KNOWN);

    if (writeflag == MEM_WRITE) {
        uint64_t val = memory_readmax64(cpu, data, len);
        siu_write(d->siu, siu_offset, (unsigned)len, (uint32_t)val);
    } else {
        uint64_t val = siu_read(d->siu, siu_offset, (unsigned)len);
        memory_writemax64(cpu, data, len, val);
    }

    return 1;
}

DEVICE_ACCESS(be300_vr4131_dsiu)
{
    struct be300_siu_device *d = (struct be300_siu_device *)extra;
    uint32_t offset = d->base_offset + (uint32_t)relative_addr;
    uint32_t pc = (uint32_t)cpu->pc;

    if (d->log_mmio)
        be300_log_mmio("be300_vr4131_dsiu", writeflag, offset,
            (unsigned)len, data, pc);

    be300_probe_note_mmio("vr4131-dsiu-stub", offset,
        writeflag == MEM_WRITE ? 'W' : 'R',
        (uint32_t)len, (uint64_t)pc, BE300_MMIO_CLASS_STUBBED);

    if (writeflag == MEM_WRITE) {
        /* Accept silently. */
    } else {
        memory_writemax64(cpu, data, len, 0);
    }

    return 1;
}

void be300_register_vr4131_siu(struct machine *gxm, siu_state_t *siu,
                               bool log_mmio)
{
    struct be300_siu_device *siu_dev, *dsiu_dev;

    /* SIU extension window: 0x0F000808..0x0F00081F (ns16550 already
     * covers 0x0F000800..0x0F000807). siu_read/write expects offset
     * from SIU base, so base_offset=0x08. */
    CHECK_ALLOCATION(siu_dev = malloc(sizeof(struct be300_siu_device)));
    siu_dev->siu = siu;
    siu_dev->base_offset = 0x08;
    siu_dev->log_mmio = log_mmio;
    memory_device_register(gxm->memory, "be300_vr4131_siu_ext",
        0x0F000808ULL, 0x18,
        dev_be300_vr4131_siu_access, (void *)siu_dev, DM_DEFAULT, NULL);

    /* DSIU extension window: 0x0F000828..0x0F00087F (ns16550 covers
     * 0x0F000820..0x0F000827). Stubbed. */
    CHECK_ALLOCATION(dsiu_dev = malloc(sizeof(struct be300_siu_device)));
    dsiu_dev->siu = siu;
    dsiu_dev->base_offset = 0x08;
    dsiu_dev->log_mmio = log_mmio;
    memory_device_register(gxm->memory, "be300_vr4131_dsiu_ext",
        0x0F000828ULL, 0x58,
        dev_be300_vr4131_dsiu_access, (void *)dsiu_dev, DM_DEFAULT, NULL);
}


/*
 *  be300_register_nand():
 *
 *  Register the NAND flash controller as a GXemul device.
 *  Called from machine_be300.c after NAND image is loaded.
 */
void be300_register_nand(struct machine *gxm, nand_state_t *nand,
                         cf_state_t *cf, bool log_mmio)
{
    /*
     * NAND registers span PA 0x0A00A000-0x0A00D800.
     * PA 0x0A00A040-0x0A00A050 is reserved for the input (button) device,
     * so we register NAND as two non-overlapping segments around that gap.
     *
     * nand_lo: 0x0A00A000, size 0x40  (offsets 0xA000..0xA03F)
     * nand_hi: 0x0A00A050, size 0x37B0 (offsets 0xA050..0xD800)
     */
    struct be300_nand_device *lo, *hi;
    CHECK_ALLOCATION(lo = malloc(sizeof(struct be300_nand_device)));
    lo->nand = nand;  lo->cf = cf;  lo->log_mmio = log_mmio;  lo->reg_offset = 0x0000;
    memory_device_register(gxm->memory, "be300_nand_lo",
        0x0A00A000ULL, 0x40,
        dev_be300_nand_access, (void *)lo, DM_DEFAULT, NULL);

    CHECK_ALLOCATION(hi = malloc(sizeof(struct be300_nand_device)));
    hi->nand = nand;  hi->cf = cf;  hi->log_mmio = log_mmio;  hi->reg_offset = 0x0050;
    memory_device_register(gxm->memory, "be300_nand_hi",
        0x0A00A050ULL, 0x37B0,
        dev_be300_nand_access, (void *)hi, DM_DEFAULT, NULL);
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
    struct be300_wince_aux *aux;
    CHECK_ALLOCATION(latch = calloc(1, sizeof(struct be300_vrc4173_latch)));
    latch->log_mmio = log_mmio;
    g_be300_vrc4173_latch = latch;
    g_be300_machine = m;
    CHECK_ALLOCATION(aux = calloc(1, sizeof(struct be300_wince_aux)));
    aux->log_mmio = log_mmio;
    aux->ppsh_enabled = enable_ppsh;
    g_be300_wince_aux = aux;
    ppsh_refresh_status(aux);
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
         * show the same sparse 0,1,1,1 pattern at 0x0A000120/0x0A000520.
         */
        {
            static const struct {
                uint32_t off;
                uint32_t val;
            } aux_regs[] = {
                { 0x000, 0x00000000 },
                { 0x004, 0x00000001 },
                { 0x008, 0x00000001 },
                { 0x00C, 0x00000001 },
                { 0x400, 0x00000000 },
                { 0x404, 0x00000001 },
                { 0x408, 0x00000001 },
                { 0x40C, 0x00000001 },
            };

            for (unsigned i = 0; i < sizeof(aux_regs) / sizeof(aux_regs[0]); i++)
                memcpy(&aux->bytes[aux_regs[i].off], &aux_regs[i].val, 4);

            ppsh_refresh_status(aux);
        }
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

    if (m->cfg.enable_pcconnect_time_sync) {
        char tmps[200];

        snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i.giu.%i",
            gxm->path, gxm->bootstrap_cpu, 8, 0);
        INTERRUPT_CONNECT(tmps, latch->pcconnect_irq);
        latch->pcconnect_irq_connected = true;
        latch->pcconnect_irq_asserted = false;
        pcconnect_set_rx_ready_callback(be300_pcconnect_uart_rx_ready, latch);
    }

    if (enable_ppsh) {
        memory_device_register(gxm->memory, "be300_wince_aux",
            WINCE_AUX_BASE, WINCE_AUX_SIZE,
            dev_be300_wince_aux_access, (void *)aux, DM_DEFAULT, NULL);
    } else {
        uint64_t page_base = WINCE_AUX_BASE & ~(uint64_t)0xFFF;
        uint32_t page_size = 0x1000;
        dev_ram_init(gxm, page_base, page_size,
            DEV_RAM_RAM, 0, "ppsh_idle_ram");
        unsigned char *p = memory_paddr_to_hostaddr(gxm->memory,
            page_base, MEM_WRITE);
        if (p) {
            uint32_t off = (uint32_t)(WINCE_AUX_BASE - page_base);
            memset(p + off, 0xFF, WINCE_AUX_SIZE);
            p[off + 0x400] = 0x20;
            p[off + 0x401] = 0x23;
        }
    }
}

void be300_register_cf_window(struct machine *gxm, machine_t *m, bool log_mmio)
{
    struct be300_cf_window_device *d;

    CHECK_ALLOCATION(d = calloc(1, sizeof(*d)));
    d->cf = &m->cf[BE300_PRIMARY_CF_SLOT];
    d->log_mmio = log_mmio;

    memory_device_register(gxm->memory, "be300_cf_window",
        PA_ROM_BASE, CF_WINDOW_SIZE,
        dev_be300_cf_window_access, (void *)d, DM_DEFAULT, NULL);
}

void be300_register_companion_ab_window(struct machine *gxm, machine_t *m,
    bool log_mmio)
{
    struct be300_companion_ab_device *d;

    CHECK_ALLOCATION(d = calloc(1, sizeof(*d)));
    d->cf = &m->cf[BE300_PRIMARY_CF_SLOT];
    d->log_mmio = log_mmio;

    memory_device_register(gxm->memory, "be300_companion_ab_window",
        0x0B000000ULL, sizeof(d->bytes),
        dev_be300_companion_ab_access, (void *)d,
        DM_DEFAULT, NULL);
}

/*
 *  PCMCIA card attribute-memory window at PA 0x0B400000-0x0B700000.
 *
 *  Background: pcmcia.dll has two card-window read helpers that walk
 *  attribute memory looking for CIS tuples:
 *    - 0x0198a3b0 / lbu @ 0x0198a3f0 = `pcmcia_window_read_byte(base,
 *      offset*2)` — flooded PA 0x0B600000+ when no card was attached.
 *    - 0x0198a488 / lbu @ 0x0198a490 = `simple_read_byte(base+offset)`
 *      — flooded PA 0x0B400000+0x1F5 once the 0x0B600000 stub shifted
 *      the scan to a second polling loop.
 *
 *  No-card behavior (preserved from Pass 48): real BE-300 with no card
 *  inserted reads the attribute-memory window's pulled-up bus as 0xFF.
 *  PC Card Standard Release 8 §3.2.10 defines `0xFF = CISTPL_END`, so a
 *  real CIS scanner reads one 0xFF byte and stops; without that fill,
 *  unmapped reads return 0 (CISTPL_NULL) and the parser walks forever.
 *
 *  Card-present behavior (added 2026-04-25 for `--cf`): when a CF image
 *  is attached, route reads inside the window into the seeded CIS held
 *  by `cf_state_t` so pcmcia.dll can identify the card by FUNCID/MANFID
 *  and dispatch to atadisk.dll instead of prompting the user for a
 *  driver name. PCMCIA attribute memory is byte-wide on every other
 *  byte (PC Card Standard Release 8 §4.7.4); `cf_cis_read` already
 *  encodes that stride.
 *
 *  Range: 0x0B400000-0x0B700000 (3 MB) covers both observed CIS-scan
 *  loops plus margin for any future PCMCIA bridge programmed elsewhere
 *  in the lower 0x0B0xxxxx-0x0BFxxxxx range. `be300_companion_ab_window`
 *  covers 0x0B000000-0x0B010000 separately (companion-chip secondary
 *  decode window — different semantics, must remain RAM-backed).
 *
 *  TODO(2026-04-25): the *complete* fix is to model the VRC4173 PCMCIA
 *  host bridge — its window-translation registers determine where the
 *  card window actually decodes. Without that, pcmcia.dll programs
 *  default windows that land at 0x0B4-0x0B6. This shim is good enough
 *  to expose the seeded CIS to the guest; revisit once the bridge is
 *  modeled.
 */
struct be300_pcmcia_attr_device {
    cf_state_t *cf;
};

DEVICE_ACCESS(be300_pcmcia_attr)
{
    struct be300_pcmcia_attr_device *d =
        (struct be300_pcmcia_attr_device *)extra;

    (void)cpu; (void)mem;
    if (writeflag != MEM_READ) {
        /* Writes are silently dropped (host-bus pull-up has no backing
         * and the seeded CIS is treated as ROM). */
        return 1;
    }

    if (d && cf_present(d->cf)) {
        /*
         * pcmcia.dll uses two helpers against this window:
         *   - PC 0x0198a490 simple_read_byte(base+N): direct byte-addressed
         *     CIS at base 0x0B400000.
         *   - PC 0x0198a3f0 pcmcia_window_read_byte(base, N*2): striped
         *     attribute-memory CIS at base 0x0B600000 (relative_addr
         *     here lands at 0x200000+).
         * Map both onto the seeded CIS image accordingly. PC Card
         * Standard Release 8 §4.7.4 documents the every-other-byte
         * stride in attribute-memory mode.
         */
        for (size_t i = 0; i < len; i++) {
            uint32_t off = (uint32_t)relative_addr + (uint32_t)i;
            uint32_t cis_idx;
            if (off >= 0x200000u) {
                uint32_t local = off - 0x200000u;
                cis_idx = (local & 1u) ? 0xFFFFFFFFu : (local >> 1);
            } else {
                cis_idx = off;
            }
            data[i] = cf_cis_read_byte(d->cf, cis_idx);
        }
    } else {
        memset(data, 0xFF, len);
    }
    return 1;
}

void be300_register_pcmcia_attr_window(struct machine *gxm, machine_t *m)
{
    struct be300_pcmcia_attr_device *d;

    CHECK_ALLOCATION(d = calloc(1, sizeof(*d)));
    d->cf = m ? &m->cf[BE300_PRIMARY_CF_SLOT] : NULL;

    /*
     * The CIS window is a read-only view of the card's attribute memory, but
     * the CF socket status settles after the guest reaches the FUNCID tuple.
     * Keep reads dispatched so that transition cannot be optimized away.
     */
    memory_device_register(gxm->memory, "be300_pcmcia_attr",
        0x0B400000ULL, 0x300000ULL,
        dev_be300_pcmcia_attr_access, (void *)d,
        DM_DEFAULT, NULL);
}


/*
 *  Input devices: touchpanel and buttons.
 *
 *  The BE-300 touchpanel sits at VRC4173 PA 0x0A000300 (size 0x60).
 *  The button registers sit at VRC4173 PA 0x0A00A040 (size 0x10).
 *
 *  Both are registered for all boot modes (Linux ELF and NAND).
 *  For NAND boots the latch and NAND device have been pre-split to leave
 *  these address ranges free; for Linux boots they were unclaimed.
 *
 *  Reads return live input state from machine_t.  Writes are ignored.
 */

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

static uint32_t button_girq0_source_bits(const struct be300_input_device *d)
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

static void button_ack_keyboard_source(struct be300_input_device *d)
{
    if (!d || !d->button_irq_pending)
        return;

    d->button_irq_pending = false;
    button_irq_update(d, false);
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
    d->button_irq_pending = true;
    button_irq_update(d, true);
}

static uint32_t piu_girq0_source_bits(const struct be300_input_device *d)
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
static void piu_update_state(struct be300_input_device *d)
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
     * Keep the low halfword at its quiescent value and expose the live
     * host button bitmap in bytes 2/3, which are the bytes used by the
     * existing BE-300 input mapping.
     */
    reg[0x00] = 0xffu;
    reg[0x01] = 0x9eu;
    reg[0x02] = m->btn_set1;
    reg[0x03] = m->btn_set2;

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
