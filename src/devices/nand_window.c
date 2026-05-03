/*
 *  src/devices/nand_window.c — VRC4173 NAND/CF window device handlers.
 *
 *  Devices registered:
 *    be300_nand:               PA 0x0A00A000-0x0A00D800 (NAND controller +
 *                              card-bridge crossover for PCMCIA windows;
 *                              split into "lo" and "hi" segments around the
 *                              0x0A00A040 button-input gap)
 *    be300_cf_window:          ROM-window CF taskfile alias
 *    be300_companion_ab:       0x0B000000 + 64 KB secondary companion socket
 *                              status latch
 *
 *  Split out of src/be300_devices.c. Public API (declared in be300.h):
 *    be300_register_nand,
 *    be300_register_cf_window,
 *    be300_register_companion_ab_window.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "cpu_mips.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"

#include "be300.h"
#include "be300_probe.h"
#include "devices.h"
#include "hw/cf.h"
#include "hw/nand.h"

#include "devices_internal.h"

#define BE300_KJGPIO_CARD_DETECT_OFF UINT32_C(0xA008)
#define BE300_KJGPIO_CARD_DETECT_INACTIVE UINT64_C(0x00000006)

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
        be300_cf_irq_update(g_be300_vrc4173_latch);
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
        be300_cf_irq_update(g_be300_vrc4173_latch);
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
    be300_cf_irq_update(g_be300_vrc4173_latch);

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
