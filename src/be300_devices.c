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
#include "devices/vrc4173_latch.h"
#include "host_time.h"
#include "hw/cf.h"
#include "hw/nand.h"
#include "hw/siu.h"
#include "hw/usbf.h"
#include "usbredir_dev.h"
#include "pcconnect.h"
#include "ppsh.h"
#include "stowaway.h"
#include "ui.h"

struct be300_vrc4173_latch *g_be300_vrc4173_latch;

uint64_t be300_emulated_ns(const machine_t *m)
{
    uint64_t hz, cycles;

    if (!m || !m->cpu || !m->gxe_machine)
        return 0;
    hz = m->gxe_machine->emulated_hz > 0 ?
        (uint64_t)m->gxe_machine->emulated_hz : UINT64_C(131072000);
    cycles = m->cpu->ninstrs;
    return cycles / hz * UINT64_C(1000000000) +
        cycles % hz * UINT64_C(1000000000) / hz;
}

/*
 * --log-mmio: print one line per dispatched access. Volume-heavy by design
 * (every MMIO touch), matches the flag name in src/main.c usage text.
 */
bool be300_phys_copy(struct cpu *cpu, uint64_t pa, void *buf, size_t len,
    int writeflag)
{
    unsigned char *p = (unsigned char *)buf;

    while (len > 0) {
        size_t chunk = 0x1000u - (size_t)(pa & 0xFFFu);

        if (chunk > len)
            chunk = len;
        if (cpu->memory_rw(cpu, cpu->mem, pa, p, chunk, writeflag,
                PHYSICAL | NO_EXCEPTIONS | CACHE_NONE) != MEMORY_ACCESS_OK)
            return false;
        pa += chunk;
        p += chunk;
        len -= chunk;
    }
    return true;
}

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

machine_t *g_be300_machine = NULL;  /* for PIU cross-device callback */

/*
 * GIRQ0 source enables. Source n of AA000004 has its enable at
 * AA000010 + 4n, bit 3 (USB bit 7 at 0x2C and wave audio bit 8 at 0x30
 * below). The OAL's GIRQ0-0 and GIRQ0-1 handlers mask PCMCIA and the
 * buttons with AA000010/AA000014 &= ~0x08 (docs/hardware/hardware.txt:93-112),
 * InterruptDone writes 0x30, 0x04, 0x0C to set them again (PCs
 * 0x800a9428-0x800a95a0 for 0x10, 0x800a9070-0x800a9114 for 0x14), the boot
 * ROM leaves 0x3C in both (PCs 0xa0076d68-0xa0076d80), and the real idle
 * dump reads 0x0C (hw_dump_vrc4173.txt:25). A disabled source neither
 * raises GIU line 0 nor reads set in AA000004, whose dispatcher serves the
 * lowest set bit. Writes still acknowledge both sources (be300_latch_write).
 */
#define BE300_GIRQ0_PCMCIA_EN   0x0010u
#define BE300_GIRQ0_KEYS_EN     0x0014u
#define BE300_GIRQ0_SOURCE_EN_BIT 0x00000008u

static bool be300_girq0_enabled(struct be300_vrc4173_latch *d, uint32_t en_off)
{
    return (be300_latch_peek_u32(d, en_off) & BE300_GIRQ0_SOURCE_EN_BIT) != 0;
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
        &g_be300_machine->cf[BE300_PRIMARY_CF_SLOT]) != 0 &&
        be300_girq0_enabled(d, BE300_GIRQ0_PCMCIA_EN);
    if (want && !d->cf_irq_asserted) {
        INTERRUPT_ASSERT(d->cf_irq);
        d->cf_irq_asserted = true;
    } else if (!want && d->cf_irq_asserted) {
        INTERRUPT_DEASSERT(d->cf_irq);
        d->cf_irq_asserted = false;
    }
}

/*
 * USB function controller interrupt: docs/BE300_USB_FUNCTION.md.  The OAL
 * GIRQ0 dispatcher takes AA000004 bit 7 to handler 0x800aaab0, which clears
 * the source enable AA00002C bit 3 before returning SYSINTR 0x12 and leaves
 * it to InterruptDone to set it again (real-hardware idle value 0x0C,
 * hw_dump_vrc4173.txt:26).  The dispatcher services only the lowest set
 * AA000004 bit, so the bit must read clear while the enable is off.
 */
#define BE300_GIRQ0_USBF        0x00000080u
#define BE300_USBF_SOURCE_EN    0x002Cu
#define BE300_USBF_SOURCE_EN_BIT 0x00000008u

static uint32_t be300_usbf_girq0_source_bits(struct be300_vrc4173_latch *d)
{
    if (!d || !usbf_irq_pending(&d->usbf) ||
        !(be300_latch_peek_u32(d, BE300_USBF_SOURCE_EN) &
          BE300_USBF_SOURCE_EN_BIT))
        return 0;
    return BE300_GIRQ0_USBF;
}

static void be300_usbf_irq_update(struct be300_vrc4173_latch *d)
{
    bool want;

    if (!d || !d->usb_irq_connected)
        return;

    want = be300_usbf_girq0_source_bits(d) != 0;
    if (want && !d->usb_irq_asserted) {
        INTERRUPT_ASSERT(d->usb_irq);
        d->usb_irq_asserted = true;
    } else if (!want && d->usb_irq_asserted) {
        INTERRUPT_DEASSERT(d->usb_irq);
        d->usb_irq_asserted = false;
    }
}

/*
 * Wave-output DMA interrupt: docs/BE300_AUDIO.md.  The OAL GIRQ0 dispatcher
 * takes AA000004 bit 8 to handler 0x800a8dcc, which clears the source enable
 * AA000030 bit 3 and returns SYSINTR 0x14 (wavedev.dll) unless AA00089C
 * bit 7 is set.  Real-hardware idle value of AA000030 is 0x0C
 * (hw_dump_vrc4173.txt:27), i.e. the source enabled.
 */
static uint32_t be300_aiu_girq0_source_bits(struct be300_vrc4173_latch *d)
{
    if (!d || !g_be300_machine || !aiu_irq_pending(&g_be300_machine->aiu) ||
        !(be300_latch_peek_u32(d, AIU_SOURCE_EN_OFF) & AIU_SOURCE_EN_BIT))
        return 0;
    return AIU_GIRQ0_SOURCE;
}

static void be300_aiu_irq_update(struct be300_vrc4173_latch *d)
{
    bool want;

    if (!d || !d->aiu_irq_connected)
        return;

    want = be300_aiu_girq0_source_bits(d) != 0;
    if (want && !d->aiu_irq_asserted) {
        INTERRUPT_ASSERT(d->aiu_irq);
        d->aiu_irq_asserted = true;
    } else if (!want && d->aiu_irq_asserted) {
        INTERRUPT_DEASSERT(d->aiu_irq);
        d->aiu_irq_asserted = false;
    }
}

static void be300_usbf_irq_changed(void *ctx)
{
    be300_usbf_irq_update((struct be300_vrc4173_latch *)ctx);
}

static uint8_t *be300_usbf_ram(void *ctx, uint32_t pa, uint32_t len)
{
    (void)ctx;
    (void)len;
    if (!g_be300_machine || !g_be300_machine->gxe_machine)
        return NULL;
    return memory_paddr_to_hostaddr(g_be300_machine->gxe_machine->memory,
        pa, MEM_WRITE);
}

void be300_vrc4173_update_audio_irq(void)
{
    be300_aiu_irq_update(g_be300_vrc4173_latch);
}

void be300_vrc4173_update_cf_irq(void)
{
    be300_cf_irq_update(g_be300_vrc4173_latch);
}

/*
 * Companion clock/power/wake block, 0x1100-0x113F: per-unit enables the
 * drivers set and clear (0x1104/0x1118 audio, 0x1114 USB, 0x1128 CMM) and
 * the 0x1120/0x112C words the interrupt handlers write 1 to
 * (docs/hardware/hardware.txt:15, docs/BE300_AUDIO.md, BE300_USB_FUNCTION.md).
 * Plain storage, mirrored every 0x40 through 0x11FF as the hardware dump
 * shows (hw_dump_vrc4173.txt:49-56). It is not part of the CF socket, so a
 * card change leaves it alone.
 */
#define BE300_CMU_BASE  0x1100u
#define BE300_CMU_SIZE  0x0040u
#define BE300_CMU_END   0x1200u

static uint32_t be300_cmu_fold(uint32_t off)
{
    return BE300_CMU_BASE + ((off - BE300_CMU_BASE) & (BE300_CMU_SIZE - 1u));
}

/* 0x1CC0-0x1CDF: the companion DMA block nanddisk.dll programs. */
#define BE300_CDMA_BASE 0x1CC0u
#define BE300_CDMA_END  0x1CE0u

struct be300_vrc4173_segment {
    struct be300_vrc4173_latch *latch;
    uint32_t offset_in_latch;    /* offset of this segment within the latch */
};


/*
 * VRC4173 companion dispatch. be300_vrc4173_regions[] lists the blocks
 * with their own behaviour, in precedence order: the first entry whose
 * claims() accepts an access and that has a handler for its direction
 * takes it. Anything left is the plain latch (be300_latch_read/write).
 */
typedef struct {
    const char *name;           /* be300_probe_note_mmio tag, NULL: none */
    enum be300_mmio_class mmio_class;
    bool (*claims)(struct be300_vrc4173_latch *d, uint32_t off,
        unsigned len);
    uint64_t (*read)(struct cpu *cpu, struct be300_vrc4173_latch *d,
        uint32_t off, unsigned len);
    void (*write)(struct cpu *cpu, struct be300_vrc4173_latch *d,
        uint32_t off, unsigned len, uint64_t val, const unsigned char *data);
} be300_vrc4173_region_t;

/* AIU status: the wave DMA model's live status (src/hw/aiu.c). */
static bool aiu_status_claims(struct be300_vrc4173_latch *d, uint32_t off,
    unsigned len)
{
    (void)d;
    (void)len;
    return g_be300_machine && off == AIU_REG_STATUS;
}

static uint64_t aiu_status_read(struct cpu *cpu,
    struct be300_vrc4173_latch *d, uint32_t off, unsigned len)
{
    (void)cpu;
    (void)d;
    (void)off;
    (void)len;
    return aiu_read_status(&g_be300_machine->aiu);
}

/* 0x0004: GIRQ0 source bits, ORed from every companion source. */
static bool girq0_src_claims(struct be300_vrc4173_latch *d, uint32_t off,
    unsigned len)
{
    (void)d;
    (void)len;
    return g_be300_machine && off == 0x0004u;
}

static uint64_t girq0_src_read(struct cpu *cpu,
    struct be300_vrc4173_latch *d, uint32_t off, unsigned len)
{
    uint64_t val = be300_latch_peek_u32(d, off);

    /*
     * hardware.txt:32 identifies bit 0 here as the cascaded PCMCIA
     * source.  If no CF image is attached, there is no card-backed
     * source to report even if a guest write left bit 0 latched.
     */
    if (!cf_present(&g_be300_machine->cf[BE300_PRIMARY_CF_SLOT]))
        val &= ~(uint64_t)1u;
    val &= ~(uint64_t)BUTTON_GIRQ0_SOURCE;
    if (be300_girq0_enabled(d, BE300_GIRQ0_PCMCIA_EN))
        val |= cf_giu_source_bits(&g_be300_machine->cf[BE300_PRIMARY_CF_SLOT]);
    else
        val &= ~(uint64_t)1u;
    val |= be300_pcconnect_girq0_source_bits(d)
        | be300_usbf_girq0_source_bits(d)
        | be300_aiu_girq0_source_bits(d)
        | button_girq0_source_bits(
            (const struct be300_input_device *)
            g_be300_machine->button_device)
        | piu_girq0_source_bits(
            (const struct be300_input_device *)
            g_be300_machine->touch_device);
    be300_pcconnect_trace(d, "read-girq0", off, (uint32_t)len,
        val, (uint32_t)cpu->pc);
    return val;
}

static bool cmu_claims(struct be300_vrc4173_latch *d, uint32_t off,
    unsigned len)
{
    (void)d;
    (void)len;
    return off >= BE300_CMU_BASE && off < BE300_CMU_END;
}

static uint64_t cmu_read(struct cpu *cpu, struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len)
{
    uint32_t at = be300_cmu_fold(off);

    if (at + len > BE300_CMU_BASE + BE300_CMU_SIZE)
        return 0;
    (void)cpu;
    uint64_t value = 0;
    for (unsigned i = 0; i < len; i++) {
        uint8_t byte = g_be300_machine && lcd_handles(at + i, 1)
            ? (uint8_t)lcd_read(&g_be300_machine->lcd, at + i, 1)
            : d->bytes[at + i];
        value |= (uint64_t)byte << (8 * i);
    }
    return value;
}

static void update_buzzer(void)
{
    uint32_t period, duty;
    if (!g_be300_machine) return;
    bool enabled = lcd_buzzer_signal(&g_be300_machine->lcd,&period,&duty);
#ifndef __EMSCRIPTEN__
    enabled = enabled && g_be300_machine->cfg.enable_audio;
#endif
    ui_audio_set_buzzer(period,duty,enabled);
}

static void cmu_write(struct cpu *cpu, struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len, uint64_t val, const unsigned char *data)
{
    uint32_t at = be300_cmu_fold(off);

    (void)cpu;
    (void)val;
    if (at + len <= BE300_CMU_BASE + BE300_CMU_SIZE) {
        memcpy(&d->bytes[at], data, len);
        for (unsigned i = 0; i < len; i++)
            if (g_be300_machine && lcd_handles(at + i, 1))
                lcd_write(&g_be300_machine->lcd, at + i, 1, data[i]);
        update_buzzer();
    }
}

/* Recovered SPL 80F034A0 / ddi.dll 01A55900, 01A56A5C.
 * Shared gates stay in CMU dispatch so its existing aliases agree. */
static bool lcd_claims(struct be300_vrc4173_latch *d, uint32_t off,
    unsigned len)
{
    (void)d;
    return g_be300_machine && off < BE300_CMU_BASE && lcd_handles(off, len);
}

static uint64_t lcd_region_read(struct cpu *cpu, struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len)
{
    (void)cpu;
    (void)d;
    return lcd_read(&g_be300_machine->lcd, off, len);
}

static void lcd_region_write(struct cpu *cpu, struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len, uint64_t val, const unsigned char *data)
{
    (void)cpu;
    (void)d;
    (void)data;
    lcd_write(&g_be300_machine->lcd, off, len, val);
    update_buzzer();
}

/* 0x1CD0: DMA-pending status; reading it runs the armed transfer. */
static bool cdma_status_claims(struct be300_vrc4173_latch *d, uint32_t off,
    unsigned len)
{
    (void)d;
    (void)len;
    return g_be300_machine && off == 0x1CD0u;
}

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
static uint64_t cdma_status_read(struct cpu *cpu,
    struct be300_vrc4173_latch *d, uint32_t off, unsigned len)
{
    uint64_t val = memory_readmax64(cpu, &d->bytes[off], len);
    uint32_t latched = be300_latch_peek_u32(d, 0x1CD0);

    if ((latched & 1u) != 0 && cpu && cpu->mem &&
        g_be300_machine->nand.stream_active) {
        uint32_t dest_pa = be300_latch_peek_u32(d, 0x1CDC);
        uint32_t xfer_len = be300_latch_peek_u32(d, 0x1CD8);
        if (xfer_len > 0 && xfer_len <= 0x2000u) {
            unsigned char buf[0x2000];

            nand_stream_read_dma(&g_be300_machine->nand, buf, xfer_len);
            be300_phys_copy(cpu, dest_pa, buf, xfer_len, MEM_WRITE);
        }
        /* clear pending so subsequent polls don't re-trigger */
        d->bytes[0x1CD0] &= ~(uint8_t)1u;
    }
    return val & ~(uint64_t)1u;
}

static bool cdma_claims(struct be300_vrc4173_latch *d, uint32_t off,
    unsigned len)
{
    (void)d;
    (void)len;
    return off >= BE300_CDMA_BASE && off < BE300_CDMA_END;
}

static uint64_t cdma_read(struct cpu *cpu, struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len)
{
    return memory_readmax64(cpu, &d->bytes[off], len);
}

static void cdma_write(struct cpu *cpu, struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len, uint64_t val, const unsigned char *data)
{
    (void)cpu;
    (void)val;
    memcpy(&d->bytes[off], data, len);
}

static bool nand_restore_claims(struct be300_vrc4173_latch *d, uint32_t off,
    unsigned len)
{
    (void)d;
    (void)len;
    return g_be300_machine && nand_restore_handles_offset(off);
}

static uint64_t nand_restore_region_read(struct cpu *cpu,
    struct be300_vrc4173_latch *d, uint32_t off, unsigned len)
{
    (void)cpu;
    (void)d;
    return nand_restore_read(&g_be300_machine->nand, off, len);
}

static void nand_restore_region_write(struct cpu *cpu,
    struct be300_vrc4173_latch *d, uint32_t off, unsigned len, uint64_t val,
    const unsigned char *data)
{
    (void)cpu;
    (void)d;
    (void)data;
    nand_restore_write(&g_be300_machine->nand, off, len, val);
}

static bool usbf_claims(struct be300_vrc4173_latch *d, uint32_t off,
    unsigned len)
{
    (void)d;
    (void)len;
    return usbf_handles(off);
}

static uint64_t usbf_region_read(struct cpu *cpu,
    struct be300_vrc4173_latch *d, uint32_t off, unsigned len)
{
    (void)cpu;
    (void)len;
    return usbf_read(&d->usbf, off);
}

static void usbf_region_write(struct cpu *cpu, struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len, uint64_t val, const unsigned char *data)
{
    (void)cpu;
    (void)len;
    (void)data;
    usbf_write(&d->usbf, off, (uint32_t)val);
}

/* 0x1000-0x1FFF not claimed above: the CF socket companion (src/hw/cf.c). */
static bool cf_companion_claims(struct be300_vrc4173_latch *d, uint32_t off,
    unsigned len)
{
    (void)d;
    (void)len;
    return g_be300_machine && off >= 0x1000u && off < 0x2000u;
}

static uint64_t cf_companion_region_read(struct cpu *cpu,
    struct be300_vrc4173_latch *d, uint32_t off, unsigned len)
{
    uint64_t val;

    (void)cpu;
    val = cf_companion_read(&g_be300_machine->cf[BE300_PRIMARY_CF_SLOT],
        off - 0x1000u, len);
    return be300_pcconnect_pccard_status_read(d,
        cf_present(&g_be300_machine->cf[BE300_PRIMARY_CF_SLOT]),
        off, len, val);
}

static void cf_companion_region_write(struct cpu *cpu,
    struct be300_vrc4173_latch *d, uint32_t off, unsigned len, uint64_t val,
    const unsigned char *data)
{
    (void)cpu;
    cf_companion_write(&g_be300_machine->cf[BE300_PRIMARY_CF_SLOT],
        off - 0x1000u, len, val);
    memcpy(&d->bytes[off], data, len);
    be300_commmode_event_ack(d, off, len, val);
}

    /*
     * Runtime PCMCIA scans also probe PA 0x0A00E000 through this
     * generic companion latch path (PC 0x0198A490).  That page is a
     * card-memory alias, not ordinary latch RAM; once the PCMCIA
     * bridge has enabled card windows, no-media reads must see the
     * pulled-up attribute bus instead of latch zeroes.
     */
static bool pcmcia_empty_attr_claims(struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len)
{
    (void)d;
    (void)len;
    return g_be300_machine &&
        cf_pcmcia_windows_enabled(
            &g_be300_machine->cf[BE300_PRIMARY_CF_SLOT]) &&
        off >= 0xE000u && off < 0xE800u;
}

static uint64_t pcmcia_empty_attr_read(struct cpu *cpu,
    struct be300_vrc4173_latch *d, uint32_t off, unsigned len)
{
    uint64_t val = UINT64_MAX;

    (void)cpu;
    (void)d;
    (void)off;
    if (len < sizeof(val))
        val >>= (sizeof(val) - len) * 8u;
    return val;
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
     * 0x112C that the interrupt handlers write.
     */
static bool intr_aggr_claims(struct be300_vrc4173_latch *d, uint32_t off,
    unsigned len)
{
    (void)d;
    (void)len;
    return off == 0x8A0u || off == 0xA00u;
}

    /*
     * WORKAROUND: VRC4173 drawing-engine block at
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
     * insufficient to identify panel timing. ddi.dll uses this block
     * for drawing; panel parameters are at 0xC00 (see BE300_LCD.md).
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
static bool ddi_busy_claims(struct be300_vrc4173_latch *d, uint32_t off,
    unsigned len)
{
    (void)d;
    (void)len;
    return off == 0x234u;
}

/* Shared by the two stubs above: the latched word with bit 0 clear. */
static uint64_t latched_bit0_clear_read(struct cpu *cpu,
    struct be300_vrc4173_latch *d, uint32_t off, unsigned len)
{
    (void)cpu;
    (void)len;
    return (uint64_t)(be300_latch_peek_u32(d, off) & ~(uint32_t)1u);
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
static bool scmcu_claims(struct be300_vrc4173_latch *d, uint32_t off,
    unsigned len)
{
    (void)d;
    (void)len;
    return off >= 0x7800u && off < 0x7840u;
}

static uint64_t scmcu_read(struct cpu *cpu, struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len)
{
    (void)cpu;
    (void)d;
    (void)off;
    (void)len;
    return 0;   /* WORKAROUND: instant MCU completion */
}

static const be300_vrc4173_region_t be300_vrc4173_regions[] = {
    { NULL, BE300_MMIO_CLASS_KNOWN, aiu_status_claims,
      aiu_status_read, NULL },
    { "vrc4173-giu-src", BE300_MMIO_CLASS_KNOWN, girq0_src_claims,
      girq0_src_read, NULL },
    { "vrc4173-commmode", BE300_MMIO_CLASS_KNOWN,
      be300_commmode_status_claims, be300_commmode_status_read, NULL },
    { "vrc4173-commmode-socket", BE300_MMIO_CLASS_KNOWN,
      be300_commmode_socket_claims, be300_commmode_socket_read, NULL },
    { "vrc4173-commmode-serial-socket", BE300_MMIO_CLASS_KNOWN,
      be300_commmode_serial_socket_claims,
      be300_commmode_serial_socket_read, NULL },
    { "vrc4173-commmode-modem-event", BE300_MMIO_CLASS_KNOWN,
      be300_commmode_modem_event_claims,
      be300_commmode_modem_event_read, NULL },
    { "be300-lcd", BE300_MMIO_CLASS_KNOWN, lcd_claims,
      lcd_region_read, lcd_region_write },
    { "vrc4173-cmu", BE300_MMIO_CLASS_KNOWN, cmu_claims,
      cmu_read, cmu_write },
    { "vrc4173-dma-done-synth", BE300_MMIO_CLASS_STUBBED,
      cdma_status_claims, cdma_status_read, NULL },
    { "vrc4173-dma", BE300_MMIO_CLASS_KNOWN, cdma_claims,
      cdma_read, cdma_write },
    { "vrc4173-nand-restore", BE300_MMIO_CLASS_KNOWN, nand_restore_claims,
      nand_restore_region_read, nand_restore_region_write },
    { "vrc4173-usbf", BE300_MMIO_CLASS_KNOWN, usbf_claims,
      usbf_region_read, usbf_region_write },
    { "vrc4173-cf-companion", BE300_MMIO_CLASS_KNOWN, cf_companion_claims,
      cf_companion_region_read, cf_companion_region_write },
    { "vrc4173-pcmcia-empty-attr", BE300_MMIO_CLASS_KNOWN,
      pcmcia_empty_attr_claims, pcmcia_empty_attr_read, NULL },
    { "vrc4173-intr-aggr-stub", BE300_MMIO_CLASS_STUBBED, intr_aggr_claims,
      latched_bit0_clear_read, NULL },
    { "vrc4173-ddi-ctrl-busy", BE300_MMIO_CLASS_STUBBED, ddi_busy_claims,
      latched_bit0_clear_read, NULL },
    { "vrc4173-scmcu-stub", BE300_MMIO_CLASS_STUBBED, scmcu_claims,
      scmcu_read, NULL },
};

/* The plain latch: stored bytes, with the side effects of the low block. */
static void be300_latch_write(struct cpu *cpu, struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len, uint64_t val, const unsigned char *data)
{
    if (g_be300_machine && (off == 0x0010u || off == 0x0014u)) {
        cf_clear_irq(&g_be300_machine->cf[BE300_PRIMARY_CF_SLOT]);
        be300_cf_irq_update(d);
    }
    /* GIRQ0-1 keyboard dispatcher acknowledges through AA000014;
     * see docs/hardware/hardware.txt:107-112. */
    if (g_be300_machine && off <= 0x0014u && off + len > 0x0014u)
        button_ack_keyboard_source(
            (struct be300_input_device *)g_be300_machine->button_device);

    /*
     * Level-2 interrupt status registers 0x062-0x077 are
     * write-1-to-clear; SYSINT1REG at 0x060 is read-only
     * (VR4131/VRC4173 interrupt status registers). Offsets from 0x1000
     * up never get here: be300_vrc4173_regions[] claims them first.
     */
    if (off >= 0x060 && off < 0x078) {
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
    if (off <= BE300_GIRQ0_PCMCIA_EN && off + len > BE300_GIRQ0_PCMCIA_EN)
        be300_cf_irq_update(d);
    if (g_be300_machine && off <= BE300_GIRQ0_KEYS_EN && off + len > BE300_GIRQ0_KEYS_EN)
        button_set_source_enabled(
            (struct be300_input_device *)g_be300_machine->button_device,
            be300_girq0_enabled(d, BE300_GIRQ0_KEYS_EN));
    if (off <= BE300_USBF_SOURCE_EN &&
        off + len > BE300_USBF_SOURCE_EN)
        be300_usbf_irq_update(d);
    if (g_be300_machine && aiu_handles_write(off, len)) {
        aiu_note_write(g_be300_machine, off, len, (uint32_t)val);
        be300_aiu_irq_update(d);
    }
    if (off <= AIU_SOURCE_EN_OFF && off + len > AIU_SOURCE_EN_OFF)
        be300_aiu_irq_update(d);
    be300_pcconnect_note_comm_write(d, off, len, val, (uint32_t)cpu->pc);
    if (off <= 0x0A00u && off + len > 0x0A00u)
        be300_vrc4173_a00_blit_maybe(cpu, d);
    if (off <= 0x0234u && off + len > 0x0234u)
        be300_vrc4173_200_display_op_maybe(d);
}

static int be300_vrc4173_handle(struct cpu *cpu, struct memory *mem,
    uint64_t relative_addr, unsigned char *data, size_t len, int writeflag,
    void *extra)
{
    struct be300_vrc4173_segment *seg = (struct be300_vrc4173_segment *)extra;
    struct be300_vrc4173_latch *d = seg->latch;
    uint32_t off = seg->offset_in_latch + (uint32_t)relative_addr;
    uint64_t val = 0;

    (void)mem;
    if (off + len > VRC4173_LATCH_SIZE)
        return 0;
    if (writeflag == MEM_WRITE)
        val = memory_readmax64(cpu, data, len);

    for (size_t i = 0;
         i < sizeof(be300_vrc4173_regions) / sizeof(be300_vrc4173_regions[0]);
         i++) {
        const be300_vrc4173_region_t *r = &be300_vrc4173_regions[i];

        if (!(writeflag == MEM_WRITE ? r->write != NULL : r->read != NULL) ||
            !r->claims(d, off, (unsigned)len))
            continue;
        if (r->name)
            be300_probe_note_mmio(r->name, off,
                writeflag == MEM_WRITE ? 'W' : 'R', (uint32_t)len,
                (uint64_t)(uint32_t)cpu->pc, r->mmio_class);
        if (writeflag == MEM_WRITE)
            r->write(cpu, d, off, (unsigned)len, val, data);
        else
            memory_writemax64(cpu, data, len,
                r->read(cpu, d, off, (unsigned)len));
        return 1;
    }

    if (writeflag == MEM_WRITE) {
        be300_latch_write(cpu, d, off, (unsigned)len, val, data);
    } else {
        be300_probe_note_mmio("vrc4173-latch", off, 'R',
            (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
            BE300_MMIO_CLASS_LATCHED);
        memcpy(data, &d->bytes[off], len);
    }
    return 1;
}

/* --log-mmio logs after the access, so a read shows the value returned. */
DEVICE_ACCESS(be300_vrc4173)
{
    struct be300_vrc4173_segment *seg = (struct be300_vrc4173_segment *)extra;
    uint32_t pc = (uint32_t)cpu->pc;
    int r = be300_vrc4173_handle(cpu, mem, relative_addr, data, len, writeflag,
        extra);

    if (r && seg && seg->latch->log_mmio)
        be300_log_mmio("be300_vrc4173", writeflag, seg->offset_in_latch + (uint32_t)relative_addr, (unsigned)len, data, pc);
    return r;
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
    lcd_init(&m->lcd);
    latch->pcconnect_insert_delay_ms = be300_pcconnect_connect_delay_ms();
    latch->usbf.ram = be300_usbf_ram;
    latch->usbf.irq_changed = be300_usbf_irq_changed;
    latch->usbf.ctx = latch;
    usbf_init(&latch->usbf);
    if (usbredir_dev_active())
        usbredir_dev_bind(&latch->usbf);

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
         * Clock/power/wake block 0x1100-0x113F and the DMA block's
         * 0x1CC0-0x1CCC words, as read on hardware
         * (hw_dump_vrc4173.txt:49-52, :183). LCD-owned gate words
         * start off instead of inheriting the post-boot dump (BE300_LCD.md).
         */
        static const struct { uint16_t off; uint32_t val; } cmu_regs[] = {
            { 0x1100, 0 }, { 0x1104, 1 }, { 0x1108, 1 }, { 0x110C, 0 },
            { 0x1110, 0 }, { 0x1114, 1 }, { 0x1118, 1 }, { 0x111C, 1 },
            { 0x1120, 1 }, { 0x1124, 0 }, { 0x1128, 0 }, { 0x112C, 1 },
            { 0x1130, 0 }, { 0x1134, 0 }, { 0x1138, 1 }, { 0x113C, 1 },
            { 0x1CC0, 0x00000001 }, { 0x1CC4, 0x00000201 },
            { 0x1CC8, 0x00001000 }, { 0x1CCC, 0x00000000 },
        };
        for (unsigned i = 0; i < sizeof(cmu_regs)/sizeof(cmu_regs[0]); i++) {
            uint32_t v = cmu_regs[i].val;
            memcpy(&latch->bytes[cmu_regs[i].off], &v, 4);
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

        /* One VRC4173 GIRQ0 gate pin per source, numbered after its
         * AA000004 bit (machine_hpcmips.c be300_girq0_init). */
        snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i.giu.0.girq0.%i",
            gxm->path, gxm->bootstrap_cpu, 8, 0);
        INTERRUPT_CONNECT(tmps, latch->cf_irq);
        latch->cf_irq_connected = true;
        latch->cf_irq_asserted = false;

        snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i.giu.0.girq0.%i",
            gxm->path, gxm->bootstrap_cpu, 8, 7);
        INTERRUPT_CONNECT(tmps, latch->usb_irq);
        latch->usb_irq_connected = true;
        latch->usb_irq_asserted = false;

        snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i.giu.0.girq0.%i",
            gxm->path, gxm->bootstrap_cpu, 8, 8);
        INTERRUPT_CONNECT(tmps, latch->aiu_irq);
        latch->aiu_irq_connected = true;
        latch->aiu_irq_asserted = false;
    }

    if (m->cfg.enable_stowaway_keyboard || m->cfg.pcconnect_bridge) {
        char tmps[200];

        snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i.giu.0.girq0.%i",
            gxm->path, gxm->bootstrap_cpu, 8, 4);
        INTERRUPT_CONNECT(tmps, latch->pcconnect_irq);
        latch->pcconnect_irq_connected = true;
        latch->pcconnect_irq_asserted = false;
        if (m->cfg.pcconnect_bridge)
            pcconnect_set_rx_ready_callback(be300_pcconnect_uart_irq_ready,
                latch);
        if (m->cfg.enable_stowaway_keyboard) {
            latch->pcconnect_dock_connected = true;
            latch->serial_dock_pccard_status_pending = true;
        }
        be300_pcconnect_irq_update(latch);
    }

    (void)enable_ppsh;  /* device registration handled by be300_register_wince_aux above */
}
