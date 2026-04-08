/*
 *  be300_devices.c — GXemul DEVICE_ACCESS wrappers for BE-300 peripherals.
 *
 *  Registers our hw/*.c peripheral state structs as GXemul memory-mapped
 *  devices at the VRC4173 companion chip address range.
 *
 *  The VR4131 internal I/O (BCU, CMU, PMU, ICU, RTC, GPIO, SIU) is handled
 *  by GXemul's native dev_vr41xx.c. This file handles the VRC4173 NAND
 *  flash controller which dev_vr41xx doesn't cover.
 */

#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "cop0.h"
#include "cpu_mips.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"

#include "be300.h"
#include "hw/nand.h"
#include "wince_boot.h"

/*
 *  VRC4173 NAND flash controller device.
 *
 *  Covers the NAND register space at PA 0x0A00A000 - 0x0A00D800.
 *  This is registered with GXemul's memory subsystem so that memory
 *  accesses from the emulated CPU are dispatched here.
 */

struct be300_nand_device {
    nand_state_t *nand;
    bool          log_mmio;
    uint32_t      reg_offset;   /* byte offset of this segment from 0x0A00A000 */
};

DEVICE_ACCESS(be300_nand)
{
    struct be300_nand_device *d = (struct be300_nand_device *)extra;
    uint32_t offset = (uint32_t)relative_addr + 0xA000 + d->reg_offset;
    uint32_t pc = (uint32_t)cpu->pc;

    if (writeflag == MEM_WRITE) {
        uint64_t val = memory_readmax64(cpu, data, len);
        nand_write(d->nand, offset, (unsigned)len, val, d->log_mmio, pc);

        /*
         *  WORKAROUND: Simulate the DMA-complete interrupt side effect.
         *
         *  The ROM's MIPS16 path writes 0 to 0xC376 as a clear-to-idle
         *  step before it issues the actual read trigger through cmd byte 7
         *  = 0xEC.  So only the 0xEC trigger should synthesize the old
         *  Count/$s3 side effects; doing it on 0xC376 corrupts the control
         *  flow we just recovered from Ghidra.
         */
        if (offset >= NAND_DMA_BASE && offset < NAND_DMA_END &&
            ((offset - NAND_DMA_BASE) + len - 1) >= 7 &&
            d->nand->dma_cmd[7] == 0xECu) {
            cpu->cd.mips.coproc[0]->reg[COP0_COUNT] = 0; /* WORKAROUND */
            cpu->cd.mips.gpr[19] = 1;  /* WORKAROUND: $s3 = DMA complete */
        }
    } else {
        uint64_t val = nand_read(d->nand, offset, (unsigned)len, d->log_mmio, pc);
        memory_writemax64(cpu, data, len, val);
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

struct be300_vrc4173_latch {
    uint8_t  bytes[0x20000];
    bool     log_mmio;
};

static struct be300_vrc4173_latch *g_be300_vrc4173_latch = NULL;

struct be300_vrc4173_segment {
    struct be300_vrc4173_latch *latch;
    uint32_t offset_in_latch;    /* offset of this segment within the latch */
};

#define WINCE_AUX_BASE  0x0C000120ULL
#define WINCE_AUX_SIZE  0x00000500u

struct be300_wince_aux {
    uint8_t bytes[WINCE_AUX_SIZE];
    bool    log_mmio;
};

DEVICE_ACCESS(be300_vrc4173)
{
    struct be300_vrc4173_segment *seg = (struct be300_vrc4173_segment *)extra;
    struct be300_vrc4173_latch *d = seg->latch;
    uint32_t off = seg->offset_in_latch + (uint32_t)relative_addr;

    if (off + len > VRC4173_LATCH_SIZE)
        return 0;

    if (writeflag == MEM_WRITE) {
        uint64_t val = memory_readmax64(cpu, data, len);

        /*
         * VRC4173 interrupt status registers use write-1-to-clear:
         * writing a 1-bit clears that bit.  Without this, the WinCE
         * interrupt dispatch loop writes 1 to clear interrupt sources
         * but the latch keeps returning 1 on subsequent reads.
         *
         * W1C registers (offsets from VRC4173 base 0x0A000000):
         *   0x060  SYSINT1REG — aggregate interrupt status (R on real HW,
         *          but W1C here lets software explicitly clear bits)
         *   0x062-0x06A  Level-2 status registers (PIU, AIU, KIU, GIU)
         *   0x1120 GIU interrupt status / clear
         *   0x112C GIU interrupt status / clear
         *   0x1B00 INTSTAT1 (primary interrupt status)
         *   0x1B10 INTSTAT1 secondary read
         *   0x1B20 INTMASK1 / interrupt acknowledge
         */
        if ((off >= 0x060 && off < 0x078) ||
            (off >= 0x1100 && off < 0x1140) ||
            (off >= 0x1B00 && off < 0x1B30)) {
            /* W1C: clear bits that are written as 1 */
            for (size_t i = 0; i < len && (off + i) < VRC4173_LATCH_SIZE; i++)
                d->bytes[off + i] &= ~data[i];
            /*
             * WORKAROUND: Zero SYSINT1REG (0x060) after any W1C write
             * to peripheral interrupt registers.
             *
             * On real hardware, SYSINT1REG is a read-only aggregate
             * that reflects the OR of all peripheral interrupt lines.
             * In our latch-based emulation, SYSINT1REG is just memory
             * that retains whatever was last written or initialized.
             * Without this workaround, stale non-zero bits in
             * SYSINT1REG cause WinCE's ISR to keep setting software
             * interrupt flags in an infinite loop, because the ISR
             * reads SYSINT1REG, sees pending interrupts, and never
             * clears them (the real hardware would auto-clear when the
             * peripheral source is serviced).
             */
            if (off != 0x060) {
                d->bytes[0x060] = 0;  /* WORKAROUND */
                d->bytes[0x061] = 0;  /* WORKAROUND */
            }
        } else {
            memcpy(&d->bytes[off], data, len);
        }
        if (d->log_mmio)
            fprintf(stderr, "[VRC4173] W PA=0x%08X size=%zu val=0x%llX PC=0x%08X\n",
                    (uint32_t)(VRC4173_LATCH_BASE + off), len,
                    (unsigned long long)val, (uint32_t)cpu->pc);
        wince_boot_note_mmio_access(cpu->machine, cpu,
            VRC4173_LATCH_BASE + off, len, val, true);
    } else {
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
            memset(data, 0, len);  /* WORKAROUND: instant MCU completion */
        } else {
            memcpy(data, &d->bytes[off], len);
        }
        if (d->log_mmio)
            fprintf(stderr, "[VRC4173] R PA=0x%08X size=%zu val=0x%llX PC=0x%08X\n",
                    (uint32_t)(VRC4173_LATCH_BASE + off), len,
                    (unsigned long long)memory_readmax64(cpu, data, len),
                    (uint32_t)cpu->pc);
        wince_boot_note_mmio_access(cpu->machine, cpu,
            VRC4173_LATCH_BASE + off, len,
            memory_readmax64(cpu, data, len), false);
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

    if (writeflag == MEM_WRITE) {
        val = memory_readmax64(cpu, data, len);
        memcpy(&d->bytes[off], data, len);

        /*
         * WinCE uses 0x0C000520 as a command latch and then polls the
         * same halfword until bit 1 clears. Captured companion-chip
         * surveys show the backing word at 0x0A000520 idles at zero, so
         * keep reads of the exact 0x520 status halfword deasserted even
         * after command writes.
         */
        if (off == 0x400 && len >= 2) {
            uint16_t idle = 0;

            memcpy(&d->bytes[off], &idle, sizeof(idle));
        }

        if (d->log_mmio) {
            fprintf(stderr,
                "[WINCE_AUX] W PA=0x%08X size=%zu val=0x%llX PC=0x%08X\n",
                (uint32_t)(WINCE_AUX_BASE + off), len,
                (unsigned long long)val, (uint32_t)cpu->pc);
        }
    } else {
        memcpy(data, &d->bytes[off], len);
        val = memory_readmax64(cpu, data, len);

        if (off == 0x400 && len >= 2) {
            val = 0;
            memory_writemax64(cpu, data, len, val);
        }

        if (off == 0x400) {
            val = memory_readmax64(cpu, data, len);
        }

        if (d->log_mmio) {
            fprintf(stderr,
                "[WINCE_AUX] R PA=0x%08X size=%zu val=0x%llX PC=0x%08X\n",
                (uint32_t)(WINCE_AUX_BASE + off), len,
                (unsigned long long)memory_readmax64(cpu, data, len),
                (uint32_t)cpu->pc);
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
 *  be300_register_nand():
 *
 *  Register the NAND flash controller as a GXemul device.
 *  Called from machine_be300.c after NAND image is loaded.
 */
void be300_register_nand(struct machine *gxm, nand_state_t *nand, bool log_mmio)
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
    lo->nand = nand;  lo->log_mmio = log_mmio;  lo->reg_offset = 0x0000;
    memory_device_register(gxm->memory, "be300_nand_lo",
        0x0A00A000ULL, 0x40,
        dev_be300_nand_access, (void *)lo, DM_DEFAULT, NULL);

    CHECK_ALLOCATION(hi = malloc(sizeof(struct be300_nand_device)));
    hi->nand = nand;  hi->log_mmio = log_mmio;  hi->reg_offset = 0x0050;
    memory_device_register(gxm->memory, "be300_nand_hi",
        0x0A00A050ULL, 0x37B0,
        dev_be300_nand_access, (void *)hi, DM_DEFAULT, NULL);

    fprintf(stderr, "[BE300] Registered NAND controller"
            " (lo: 0x0A00A000+0x40, hi: 0x0A00A050+0x37B0)\n");
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
void be300_register_vrc4173_latch(struct machine *gxm, bool log_mmio)
{
    struct be300_vrc4173_latch *latch;
    struct be300_wince_aux *aux;
    CHECK_ALLOCATION(latch = calloc(1, sizeof(struct be300_vrc4173_latch)));
    latch->log_mmio = log_mmio;
    g_be300_vrc4173_latch = latch;
    CHECK_ALLOCATION(aux = calloc(1, sizeof(struct be300_wince_aux)));
    aux->log_mmio = log_mmio;

    /*
     * Pre-populate latch with real hardware register values from
     * hardware_survey/HardwareDump.txt and HardwareDump6.txt.
     * The OEMInit callbacks read these registers to initialize
     * display, touch, keyboard, audio, power, etc.
     */
    {
        /* VRC4173 Core (PA 0x0A000000, offset 0x0000, 64 words) */
        static const uint32_t core_regs[] = {
            0x00000000, 0x00000000, 0x00000001, 0x00000000,
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

    memory_device_register(gxm->memory, "be300_wince_aux",
        WINCE_AUX_BASE, WINCE_AUX_SIZE,
        dev_be300_wince_aux_access, (void *)aux, DM_DEFAULT, NULL);

    fprintf(stderr,
        "[BE300] Registered VRC4173 latch plus WinCE 0x0C000120 alias\n");
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
};

/* Touchpanel device — PA 0x0A000300, size 0x60 */
DEVICE_ACCESS(be300_touch)
{
    struct be300_input_device *d = (struct be300_input_device *)extra;
    machine_t *m = d->m;

    if (writeflag == MEM_WRITE)
        return 1;   /* touch panel registers are read-only from guest */

    uint64_t val = 0;
    uint32_t off = (uint32_t)relative_addr;

    if (m->touch_down) {
        /* Linear ADC mapping from calibration data:
         *   y+ : ADC range 0x81E1→0x8E30 covers the FULL touch panel
         *        (display 320px + virtual button strip below).
         *        TOUCH_PANEL_H = display height + button strip height.
         *        Adjust TOUCH_PANEL_H if y-axis calibration is off.
         *   x- : ADC range 0x8300→0x8D1B, x-axis is physically inverted
         *        relative to screen coordinates.                         */
#define TOUCH_PANEL_H  (319u + 40u)   /* 320 display + ~40px button strip */
        uint16_t yp = (uint16_t)(0x81E1u +
            ((uint32_t)m->touch_y * (0x8E30u - 0x81E1u)) / TOUCH_PANEL_H);
        uint16_t ym = (uint16_t)(0x8E30u -
            ((uint32_t)m->touch_y * (0x8E30u - 0x81E1u)) / TOUCH_PANEL_H);
        /* x is inverted: screen left (x=0) → high ADC, right (x=239) → low ADC */
        uint16_t xm = (uint16_t)(0x8D1Bu -
            ((uint32_t)m->touch_x * (0x8D1Bu - 0x8300u)) / 239u);
        uint16_t xp = (uint16_t)(0x8300u +
            ((uint32_t)m->touch_x * (0x8D1Bu - 0x8300u)) / 239u);

        switch (off) {
        case 0x00: val = 0x2000u; break;   /* pendown flag */
        case 0x04: val = 0;       break;   /* interrupt bits */
        case 0x20: case 0x50: val = yp; break;
        case 0x24: case 0x54: val = ym; break;
        case 0x28: case 0x58: val = xm; break;
        case 0x2C: case 0x5C: val = xp; break;
        default:   val = 0; break;
        }
    } else {
        /* Pen up: return idle ADC values */
        switch (off) {
        case 0x00: val = 0;       break;
        case 0x20: case 0x50: val = 0x81E1u; break;
        case 0x24: case 0x54: val = 0x8E30u; break;
        case 0x28: case 0x58: val = 0x8300u; break;
        case 0x2C: case 0x5C: val = 0x8D1Bu; break;
        default:   val = 0; break;
        }
    }

    memory_writemax64(cpu, data, len, val);
    wince_boot_note_mmio_access(m->gxe_machine, cpu, 0x0A00A040ULL
        + (uint32_t)relative_addr, len, val, false);
    return 1;
}

/* Button register device — PA 0x0A00A040, size 0x10 */
DEVICE_ACCESS(be300_buttons)
{
    struct be300_input_device *d = (struct be300_input_device *)extra;
    machine_t *m = d->m;

    if (writeflag == MEM_WRITE)
        return 1;

    uint64_t val = 0;
    switch ((uint32_t)relative_addr) {
    case 0x02: val = m->btn_set1; break;   /* 0x0A00A042 */
    case 0x03: val = m->btn_set2; break;   /* 0x0A00A043 */
    default:   val = 0;           break;
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

    CHECK_ALLOCATION(touch_d = malloc(sizeof(struct be300_input_device)));
    touch_d->m = m;
    touch_d->log_mmio = log_mmio;
    memory_device_register(gxm->memory, "be300_touch",
        0x0A000300ULL, 0x60,
        dev_be300_touch_access, (void *)touch_d, DM_DEFAULT, NULL);

    CHECK_ALLOCATION(btn_d = malloc(sizeof(struct be300_input_device)));
    btn_d->m = m;
    btn_d->log_mmio = log_mmio;
    memory_device_register(gxm->memory, "be300_buttons",
        0x0A00A040ULL, 0x10,
        dev_be300_buttons_access, (void *)btn_d, DM_DEFAULT, NULL);

    fprintf(stderr, "[BE300] Registered input devices:"
            " touch@0x0A000300 buttons@0x0A00A040\n");
}
