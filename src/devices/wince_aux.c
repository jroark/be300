/*
 *  src/devices/wince_aux.c — PPSH debug-shell parallel-port device.
 *
 *  Block: PA 0x0C000120 + 0x500.
 *  Real BE-300 hardware always has the VRC4173 parallel port present;
 *  --ppsh enables the debug-host protocol, otherwise we model the
 *  transport as wired-but-no-host-attached so the WinCE PPSH probe
 *  fails fast and falls through to the normal GUI boot.
 *
 *  Split out of src/be300_devices.c. Public API (declared in be300.h):
 *    be300_ppsh_transport_ready, be300_ppsh_queue_host_input.
 *  Internal helper (declared in devices_internal.h):
 *    be300_register_wince_aux.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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
#include "ppsh.h"

#include "devices_internal.h"

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

struct be300_wince_aux *g_be300_wince_aux = NULL;

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

void be300_register_wince_aux(struct machine *gxm, bool log_mmio,
    bool enable_ppsh)
{
    struct be300_wince_aux *aux;

    CHECK_ALLOCATION(aux = calloc(1, sizeof(struct be300_wince_aux)));
    aux->log_mmio = log_mmio;
    aux->ppsh_enabled = enable_ppsh;
    g_be300_wince_aux = aux;

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

    ppsh_refresh_status(aux);

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
