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
#include "machine.h"
#include "memory.h"
#include "misc.h"

#include "be300.h"
#include "hw/nand.h"
#include "ppsh.h"
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

        wince_boot_note_mmio_access(cpu->machine, cpu,
            0x0A000000ULL + offset, len, val, true);
    } else {
        uint64_t val = nand_read(d->nand, offset, (unsigned)len, d->log_mmio, pc);
        memory_writemax64(cpu, data, len, val);
        wince_boot_note_mmio_access(cpu->machine, cpu,
            0x0A000000ULL + offset, len, val, false);
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

struct be300_vrc4173_segment {
    struct be300_vrc4173_latch *latch;
    uint32_t offset_in_latch;    /* offset of this segment within the latch */
};

/* Forward declarations for cross-device callbacks */
struct be300_input_device;
static void piu_update_state(struct be300_input_device *d);

#define WINCE_AUX_BASE  0x0C000120ULL
#define WINCE_AUX_SIZE  0x00000500u
#define PPSH_QUEUE_CAP  4096u
#define PPSH_TEXT_RUN_CAP 128u

struct be300_wince_aux {
    uint8_t bytes[WINCE_AUX_SIZE];
    bool    log_mmio;
    uint16_t ppsh_status_520;
    uint16_t ppsh_data;         /* response data at offset 0x000 */
    bool     ppsh_response_pending; /* true after cmd dispatch until data read */
    bool     ppsh_enabled;    /* --ppsh: enable PPSH debug shell probe */
    bool     ppsh_ack_pending;
    bool     ppsh_tx_valid;
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

    if (d->ppsh_guest_byte_count == 1) {
        fprintf(stderr,
            "[PPSH] guest transport traffic detected"
            " at PC=0x%08X\n", pc);
    }

    if (d->log_mmio && d->ppsh_raw_log_count < 64) {
        fprintf(stderr,
            "[PPSH_TX] byte=0x%02X PC=0x%08X count=%zu\n",
            byte, pc, d->ppsh_guest_byte_count);
        d->ppsh_raw_log_count++;
    }

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

    if (queued > 0) {
        d->ppsh_host_byte_count += queued;
        if (d->log_mmio) {
            fprintf(stderr,
                "[PPSH_RX] queued=%zu total=%zu pending=%zu\n",
                queued, d->ppsh_host_byte_count, d->ppsh_host_q_count);
        }
    }

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

    if (writeflag == MEM_WRITE) {
        uint64_t val = memory_readmax64(cpu, data, len);
        bool suspend_latch = false;

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
            memcpy(&d->bytes[off], data, len);
        } else if ((off >= 0x060 && off < 0x078) ||
            (off >= 0x1100 && off < 0x1140) ||
            (off >= 0x1B00 && off < 0x1B30)) {
            /* W1C: clear bits that are written as 1 */
            for (size_t i = 0; i < len && (off + i) < VRC4173_LATCH_SIZE; i++) {
                uint32_t byte_off = off + (uint32_t)i;
                if (byte_off == 0x060u || byte_off == 0x061u)
                    continue;
                d->bytes[byte_off] &= ~data[i];
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

        /* PIU control registers at offsets 0x000-0x05F: re-evaluate
         * scan sequencer state when NK.exe configures the PIU. */
        if (off < 0x060 && g_be300_machine && g_be300_machine->touch_device)
            piu_update_state(
                (struct be300_input_device *)g_be300_machine->touch_device);
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

            wince_boot_note_ppsh_command(cpu, cmd);
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

        if (d->log_mmio) {
            fprintf(stderr,
                "[WINCE_AUX] W PA=0x%08X size=%zu val=0x%llX PC=0x%08X\n",
                (uint32_t)(WINCE_AUX_BASE + off), len,
                (unsigned long long)val, (uint32_t)cpu->pc);
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
            wince_boot_note_ppsh_data_read(cpu, word);
        }

        /*
         * PPSH status register at offset 0x400 (PA 0x0C000520).
         * Reads return the current emulated companion-MCU status word.
         */
        if (off == 0x400 && len >= 2) {
            ppsh_refresh_status(d);
            val = d->ppsh_status_520;
            memory_writemax64(cpu, data, len, val);
            wince_boot_note_ppsh_status_read(cpu, (uint16_t)val);
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
void be300_register_vrc4173_latch(struct machine *gxm, bool log_mmio,
                                  bool enable_ppsh)
{
    struct be300_vrc4173_latch *latch;
    struct be300_wince_aux *aux;
    CHECK_ALLOCATION(latch = calloc(1, sizeof(struct be300_vrc4173_latch)));
    latch->log_mmio = log_mmio;
    g_be300_vrc4173_latch = latch;
    CHECK_ALLOCATION(aux = calloc(1, sizeof(struct be300_wince_aux)));
    aux->log_mmio = log_mmio;
    aux->ppsh_enabled = enable_ppsh;
    g_be300_wince_aux = aux;
    ppsh_refresh_status(aux);

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

        fprintf(stderr,
            "[BE300] VRC seed"
            " 8010=%08X 1120=%08X 1128=%08X 112C=%08X"
            " 1138=%08X 113C=%08X 1B10=%08X 1B14=%08X"
            " 1B20=%08X 1B2C=%08X"
            " 03C0=%08X 03C4=%08X 03C8=%08X 03F4=%08X"
            " 0880=%08X 0888=%08X 1118=%08X\n",
            be300_latch_peek_u32(latch, 0x8010u),
            be300_latch_peek_u32(latch, 0x1120u),
            be300_latch_peek_u32(latch, 0x1128u),
            be300_latch_peek_u32(latch, 0x112Cu),
            be300_latch_peek_u32(latch, 0x1138u),
            be300_latch_peek_u32(latch, 0x113Cu),
            be300_latch_peek_u32(latch, 0x1B10u),
            be300_latch_peek_u32(latch, 0x1B14u),
            be300_latch_peek_u32(latch, 0x1B20u),
            be300_latch_peek_u32(latch, 0x1B2Cu),
            be300_latch_peek_u32(latch, 0x03C0u),
            be300_latch_peek_u32(latch, 0x03C4u),
            be300_latch_peek_u32(latch, 0x03C8u),
            be300_latch_peek_u32(latch, 0x03F4u),
            be300_latch_peek_u32(latch, 0x0880u),
            be300_latch_peek_u32(latch, 0x0888u),
            be300_latch_peek_u32(latch, 0x1118u));
        fprintf(stderr,
            "[BE300] VRC audio seed"
            " 03C0=%08X 03C4=%08X 03C8=%08X 03F4=%08X"
            " 0880=%08X 0888=%08X 1118=%08X\n",
            be300_latch_peek_u32(latch, 0x03C0u),
            be300_latch_peek_u32(latch, 0x03C4u),
            be300_latch_peek_u32(latch, 0x03C8u),
            be300_latch_peek_u32(latch, 0x03F4u),
            be300_latch_peek_u32(latch, 0x0880u),
            be300_latch_peek_u32(latch, 0x0888u),
            be300_latch_peek_u32(latch, 0x1118u));
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
    /* PIU state (touch device only) */
    uint16_t   piu_regs[16];       /* register file: index = offset / 4 */
    uint16_t   piu_padstate;       /* PADSTATE(2:0) scan sequencer state */
    bool       piu_prev_touch;     /* previous touch_down for edge detect */
    struct interrupt piu_irq;      /* VRIP line 5 interrupt handle */
    bool       piu_irq_connected;
    bool       piu_irq_asserted;
};

/*
 *  PIU interrupt helper — idempotent assert/deassert through VRIP line 5.
 *  INTERRUPT_ASSERT calls vr41xx_vrip_interrupt_assert() (dev_vr41xx.c:249)
 *  which sets VR4131 SYSINT1 bit 5, potentially asserting CPU IRQ 2.
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

/*
 *  PIU scan sequencer state update after PIUCNTREG write.
 *  Follows the state transition diagram in VRC4173 manual Figure 9-4.
 *
 *  PIUCNTREG is at PA 0x0A000000 (VRC4173 PIU Base, offset 0x000),
 *  handled by the VRC4173 latch — NOT by the touch device at PA 0x0A000300.
 *  We read it from the latch so the state machine sees NK.exe's control writes.
 */
static void piu_update_state(struct be300_input_device *d)
{
    uint32_t ctl32 = 0;
    be300_vrc4173_latch_read_u32(0x0A000000, &ctl32);
    uint16_t ctl = (uint16_t)ctl32;

    if (ctl & 0x0001) {
        /* PADRST: reset everything */
        memset(d->piu_regs, 0, sizeof(d->piu_regs));
        d->piu_padstate = 0;  /* Disable */
        piu_irq_update(d, false);
        return;
    }

    if (!(ctl & 0x0002)) {
        /* PIUPWR=0 → Disable */
        d->piu_padstate = 0;
        piu_irq_update(d, false);
        return;
    }

    /* PIUPWR=1: at least Standby */
    if (d->piu_padstate == 0)
        d->piu_padstate = 1;  /* Disable → Standby */

    if ((ctl & 0x0004) && (ctl & 0x0100) && ((ctl >> 3) & 3) == 0) {
        /* PIUSEQEN=1, PADATSTART=1, PIUMODE=00 → WaitPenTouch */
        if (d->piu_padstate < 4)
            d->piu_padstate = 4;  /* → WaitPenTouch */

        /* If pen is already down, transition immediately */
        if (d->m->touch_down && d->piu_padstate == 4) {
            d->piu_padstate = 5;  /* → PenDataScan */
            /* Set pending bits in byte 0; assert IRQ if mask matches */
            d->piu_regs[1] |= 0x01;  /* PENCHGINTR (pen contact change) */
            d->piu_regs[1] |= 0x5C;  /* touch data ready bits */
            if (d->piu_regs[1] & (d->piu_regs[1] >> 8))
                piu_irq_update(d, true);
        }
    } else if (!(ctl & 0x0004)) {
        /* PIUSEQEN=0 → back to Standby */
        if (d->piu_padstate > 1) {
            d->piu_padstate = 1;
            piu_irq_update(d, false);
        }
    }
}

/*
 *  Touchpanel device — PA 0x0A000300, size 0x60
 *
 *  Register layout from docs/hardware.txt ISR analysis (GIRQ0-9):
 *
 *  +0x00 PIUCNTREG: bit 13 (0x2000) = pendown status
 *  +0x04 PIUINTREG: byte 0 (bits 7:0) = pending status (set by HW)
 *                   byte 1 (bits 15:8) = interrupt mask (R/W by driver)
 *                   ISR does: intr = W[304]; intr &= (intr >> 8);
 *                   Active bits: 0x5C → SYSINTR_TOUCH, 0x01 → SYSINTR_TOUCH_CHANGED
 *                   ISR clears by zeroing mask bits (NOT write-1-to-clear)
 *  +0x20-0x2C, +0x50-0x5C: ADC coordinate buffers (y+, y-, x-, x+)
 */
DEVICE_ACCESS(be300_touch)
{
    struct be300_input_device *d = (struct be300_input_device *)extra;
    machine_t *m = d->m;
    uint32_t off = (uint32_t)relative_addr;
    uint64_t val;

    if (writeflag == MEM_WRITE) {
        val = memory_readmax64(cpu, data, len);

        if (d->log_mmio)
            fprintf(stderr, "[PIU] W off=0x%02X val=0x%04X PC=0x%08X\n",
                off, (uint16_t)val, (uint32_t)cpu->pc);

        if (off <= 0x18 && (off & 3) == 0) {
            uint32_t idx = off / 4;
            if (off == 0x00) {
                /* PIUCNTREG: store writable bits 9:0 only */
                d->piu_regs[0] = (uint16_t)(val & 0x03FF);
                piu_update_state(d);
            } else if (off == 0x04) {
                /* PIUINTREG: mask/status byte pair.
                 * Byte 0 (bits 7:0) = pending status (read-only, set by HW)
                 * Byte 1 (bits 15:8) = interrupt mask (writable)
                 * ISR clears by zeroing mask bits, e.g. W[304] &= 0xFE00.
                 * Only update the mask byte; leave pending bits alone. */
                d->piu_regs[1] = (d->piu_regs[1] & 0x00FF)
                    | ((uint16_t)val & 0xFF00);
                /* Deassert if no pending bits match the new mask */
                if ((d->piu_regs[1] & (d->piu_regs[1] >> 8)) == 0)
                    piu_irq_update(d, false);
            } else {
                d->piu_regs[idx] = (uint16_t)val;
            }
        }
        wince_boot_note_mmio_access(m->gxe_machine, cpu, 0x0A000300ULL
            + off, len, val, true);
        return 1;
    }

    /* Read path */
    val = 0;

    if (off == 0x00) {
        /* PIUCNTREG: compose from stored bits + dynamic state */
        val = (d->piu_regs[0] & 0x03FFu)
            | ((uint16_t)(d->piu_padstate & 7) << 10)
            | (m->touch_down ? 0x2000u : 0);
    } else if (off == 0x04) {
        /* Return (mask << 8) | pending; ISR does pending & mask */
        val = d->piu_regs[1];
    } else if (off <= 0x18 && (off & 3) == 0) {
        val = d->piu_regs[off / 4];
    } else if ((off >= 0x20 && off <= 0x2C) || (off >= 0x50 && off <= 0x5C)) {
        /* ADC buffer registers — return live touch coordinates */
#define TOUCH_PANEL_H  (319u + 40u)
        uint16_t yp = (uint16_t)(0x81E1u +
            ((uint32_t)m->touch_y * (0x8E30u - 0x81E1u)) / TOUCH_PANEL_H);
        uint16_t ym = (uint16_t)(0x8E30u -
            ((uint32_t)m->touch_y * (0x8E30u - 0x81E1u)) / TOUCH_PANEL_H);
        uint16_t xm = (uint16_t)(0x8D1Bu -
            ((uint32_t)m->touch_x * (0x8D1Bu - 0x8300u)) / 239u);
        uint16_t xp = (uint16_t)(0x8300u +
            ((uint32_t)m->touch_x * (0x8D1Bu - 0x8300u)) / 239u);

        if (d->piu_padstate == 5 && m->touch_down) {
            switch (off & 0x0F) {
            case 0x00: val = yp; break;
            case 0x04: val = ym; break;
            case 0x08: val = xm; break;
            case 0x0C: val = xp; break;
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

    if (d->log_mmio)
        fprintf(stderr, "[PIU] R off=0x%02X val=0x%04X PC=0x%08X\n",
            off, (uint16_t)val, (uint32_t)cpu->pc);

    wince_boot_note_mmio_access(m->gxe_machine, cpu, 0x0A000300ULL
        + off, len, val, false);
    return 1;
}

/*
 *  be300_touch_tick():
 *
 *  Called from the VR41xx device tick to detect pen state changes
 *  and generate PIU interrupts when the scan sequencer is active.
 */
void be300_touch_tick(machine_t *m)
{
    struct be300_input_device *d;
    bool now, prev;

    if (!m || !m->touch_device)
        return;
    d = (struct be300_input_device *)m->touch_device;
    if (!d->piu_irq_connected)
        return;

    now = m->touch_down;
    prev = d->piu_prev_touch;
    d->piu_prev_touch = now;

    if (d->piu_padstate == 4 && now && !prev) {
        /* WaitPenTouch + pen down → PenDataScan */
        d->piu_regs[1] |= 0x01;   /* pending: pen contact change */
        if (d->piu_regs[0] & 0x100)  /* PADATSTART */
            d->piu_padstate = 5;
        d->piu_regs[1] |= 0x5C;   /* pending: touch data ready */
        /* Assert IRQ only if pending & mask is non-zero */
        if (d->piu_regs[1] & (d->piu_regs[1] >> 8))
            piu_irq_update(d, true);
    } else if (d->piu_padstate >= 4 && !now && prev) {
        /* Pen release */
        d->piu_regs[1] |= 0x01;   /* pending: pen contact change */
        d->piu_padstate = 4;       /* back to WaitPenTouch */
        if (d->piu_regs[1] & (d->piu_regs[1] >> 8))
            piu_irq_update(d, true);
    }
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

    CHECK_ALLOCATION(touch_d = calloc(1, sizeof(struct be300_input_device)));
    touch_d->m = m;
    touch_d->log_mmio = log_mmio;

    /* Connect PIU to VRIP interrupt line 5 (VRIP_INTR_PIU).
     * The VRIP tree is registered by dev_vr41xx_init() — this must
     * be called AFTER that init.  Same pattern as KIU/GIU at
     * gxemul/src/devices/dev_vr41xx.c:1237-1242. */
    {
        char tmps[300];
        snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i",
            gxm->path, gxm->bootstrap_cpu, 5);
        INTERRUPT_CONNECT(tmps, touch_d->piu_irq);
        touch_d->piu_irq_connected = true;
        touch_d->piu_irq_asserted = false;
    }

    m->touch_device = touch_d;
    g_be300_machine = m;

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
