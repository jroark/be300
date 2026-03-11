#include <stdio.h>
#include <inttypes.h>
#include "bus.h"
#include "machine.h"
#include "hw/bcu.h"
#include "hw/cmu.h"
#include "hw/pmu.h"
#include "hw/icu.h"
#include "hw/siu.h"
#include "hw/rtc.h"
#include "hw/gpio.h"
#include "hw/nand.h"

/* ------------------------------------------------------------------ */
/* VRC4173 companion chip MMIO                                          */
/* ------------------------------------------------------------------ */

/*
 * VRC4173 UART stub — NS16550-compatible, 4-byte register spacing.
 * Only the minimum registers needed to unblock early boot are modelled:
 *   LSR (PA 0x0A008694, offset VRC4173_UART_LSR): always returns 0x60
 *     (TEMT|THRE — transmitter empty and ready) so the kernel's THRE
 *     polling loop exits immediately.
 *   THR (PA 0x0A008680, offset VRC4173_UART_THR): writes forwarded to
 *     stdout so kernel boot messages are visible.
 */
static uint8_t vrc4173_uart_scr;  /* scratch register (read/write) */
static uint32_t vrc4173_ne2k_cwin_latch[20]; /* 0x0C00..0x0C4C, 32-bit words */
/*
 * Generic VRC4173 register latch for not-yet-modeled blocks.
 * Keeps write->readback deterministic and allows us to seed warm-state values
 * observed on hardware without fully modeling each sub-device.
 */
#define VRC4173_LATCH_SIZE UINT32_C(0x20000)
static uint8_t vrc4173_latch_bytes[VRC4173_LATCH_SIZE];
static uint8_t vrc4173_latch_valid[VRC4173_LATCH_SIZE];
static bool vrc4173_latch_seeded;

typedef struct {
    uint16_t off;
    uint32_t val;
} vrc4173_seed_word_t;

/*
 * WinCE NK board-detect probe reads PA 0xAA00A0C0 (cs3_off=0x0A0C0),
 * masks with 0xFFF0, and expects 0x7100 on BE-300 hardware.
 */
#define VRC4173_BOOT_ID_OFF    UINT32_C(0x0A0C0)
#define VRC4173_BOOT_ID_VALUE  UINT32_C(0x00007100)
#define VRC4173_NAND_C38_OFF   UINT32_C(0x00C38)
#define VRC4173_NAND_C38_INIT  UINT32_C(0x00000006)
#define VRC4173_NAND_C38_BUSY  UINT32_C(0x00000091)
#define VRC4173_NAND_C38_DONE  UINT32_C(0x0000007C)

static uint32_t vrc4173_nand_c38 = VRC4173_NAND_C38_INIT;

static inline uint32_t read_pc32(uc_engine *uc)
{
    uint64_t pc64 = 0;
    uc_reg_read(uc, UC_MIPS_REG_PC, &pc64);
    return (uint32_t)pc64;
}

static inline uint32_t vrc4173_cs3_off(const vrc4173_cb_ctx_t *ctx,
                                       uint64_t callback_offset)
{
    uint64_t pa = (uint64_t)ctx->region_base_pa + callback_offset;
    if (pa < PA_VRC4173_BASE)
        return UINT32_MAX;
    pa -= PA_VRC4173_BASE;
    if (pa >= PA_VRC4173_SIZE)
        return UINT32_MAX;
    return (uint32_t)pa;
}

/* Check if VRC4173 offset belongs to a NAND controller register range */
static inline bool is_nand_offset(uint32_t off)
{
    return (off >= NAND_CTRL_BASE   && off < NAND_CTRL_END)   ||
           (off >= NAND_XFER_BASE   && off < NAND_XFER_END)   ||
           (off >= NAND_CMD_BASE    && off < NAND_CMD_END)    ||
           (off >= NAND_ENABLE_BASE && off < NAND_ENABLE_END) ||
           (off >= NAND_STREAM_BASE && off < NAND_STREAM_END) ||
           (off >= NAND_DATA_BASE   && off < NAND_DATA_END);
}

static inline bool is_ne2k_cwin_offset(uint32_t off)
{
    return off >= 0x0C00u && off <= 0x0C4Cu;
}

static inline uint32_t ne2k_cwin_idx(uint32_t off)
{
    return (off - 0x0C00u) >> 2;
}

static inline void vrc4173_latch_write(uint32_t off, unsigned size, uint64_t value);

static inline void vrc4173_set_nand_c38(machine_t *m, uint32_t val,
                                        uint32_t pc32, const char *reason)
{
    uint32_t old = vrc4173_nand_c38;
    vrc4173_nand_c38 = val;
    vrc4173_latch_write(VRC4173_NAND_C38_OFF, 4u, vrc4173_nand_c38);

    if (m != NULL &&
        (m->cfg.log_mmio || m->cfg.log_nand_legacy || m->cfg.log_wince_stall) &&
        old != vrc4173_nand_c38) {
        static uint32_t c38_logs = 0;
        if (c38_logs < 512u) {
            fprintf(stderr,
                    "[VRC4173_NAND_C38] pc=0x%08X reason=%s old=0x%04X new=0x%04X\n",
                    pc32, reason ? reason : "unknown",
                    old & 0xFFFFu, vrc4173_nand_c38 & 0xFFFFu);
            c38_logs++;
        }
    }
}

static inline void vrc4173_nand_sideband_write(machine_t *m, uint32_t cs3_off,
                                               uint64_t value, uint32_t pc32)
{
    if (cs3_off == NAND_REG_XFER_CMD) {
        uint8_t cmd = (uint8_t)(value & 0xFFu);
        if (cmd == 0x03u || cmd == 0x06u)
            vrc4173_set_nand_c38(m, VRC4173_NAND_C38_INIT, pc32, "xfer_cmd");
        return;
    }

    if (cs3_off == NAND_REG_XFER_MODE) {
        uint8_t mode = (uint8_t)(value & 0xFFu);
        if (mode == 0x05u)
            vrc4173_set_nand_c38(m, VRC4173_NAND_C38_BUSY, pc32, "xfer_mode_busy");
        else if (mode == 0x04u)
            vrc4173_set_nand_c38(m, VRC4173_NAND_C38_DONE, pc32, "xfer_mode_done");
    }
}

static inline void vrc4173_latch_write(uint32_t off, unsigned size, uint64_t value)
{
    if (size == 0u || size > 4u)
        return;
    if (off > VRC4173_LATCH_SIZE || (uint64_t)off + size > VRC4173_LATCH_SIZE)
        return;

    for (unsigned i = 0; i < size; i++) {
        vrc4173_latch_bytes[off + i] = (uint8_t)((value >> (i * 8u)) & 0xFFu);
        vrc4173_latch_valid[off + i] = 1u;
    }
}

static inline bool vrc4173_latch_read(uint32_t off, unsigned size, uint64_t *value_out)
{
    if (!value_out || size == 0u || size > 4u)
        return false;
    if (off > VRC4173_LATCH_SIZE || (uint64_t)off + size > VRC4173_LATCH_SIZE)
        return false;

    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        if (!vrc4173_latch_valid[off + i])
            return false;
        v |= (uint64_t)vrc4173_latch_bytes[off + i] << (i * 8u);
    }
    *value_out = v;
    return true;
}

static void vrc4173_seed_core_dump_once(void)
{
    if (vrc4173_latch_seeded)
        return;
    vrc4173_latch_seeded = true;

    /*
     * Warm-state VRC4173 snapshot seeds:
     *  - hardware_survey/HardwareDump 2.txt (legacy low-offset probes)
     *  - hardware_survey/HardwareDump6.txt (active pages 0x0A008000, 0x0A00A000)
     * Seed non-zero words only.
     */
    static const vrc4173_seed_word_t seed_words[] = {
        /* HardwareDump 2: low-offset companion core words. */
        { 0x0008u, 0x00000001u },
        { 0x0010u, 0x0000000Cu }, { 0x0014u, 0x0000000Cu },
        { 0x0018u, 0x0000000Cu }, { 0x0020u, 0x0000000Cu },
        { 0x0028u, 0x0000000Cu }, { 0x002Cu, 0x0000000Cu },
        { 0x0030u, 0x0000000Cu }, { 0x0034u, 0x0000003Cu },
        { 0x0040u, 0x0000000Cu },
        { 0x0088u, 0x00000001u },
        { 0x0090u, 0x0000000Cu }, { 0x0094u, 0x0000000Cu },
        { 0x0098u, 0x0000000Cu }, { 0x00A0u, 0x0000000Cu },
        { 0x00A8u, 0x0000000Cu }, { 0x00ACu, 0x0000000Cu },
        { 0x00B0u, 0x0000000Cu }, { 0x00B4u, 0x0000003Cu },
        { 0x00C0u, 0x0000000Cu },

        /* HardwareDump6: PA 0x0A008000 page (bridge/config identity). */
        { 0x08000u, 0x0000200Cu },
        { 0x08004u, 0x00001100u },
        { 0x08010u, 0x0000000Cu },
        { 0x08014u, 0x00000002u },
        { 0x08040u, 0x0000000Au },

        /* HardwareDump6: PA 0x0A00A000 page (functional core). */
        { 0x0A000u, 0x0000000Bu },
        { 0x0A004u, 0x00000004u },
        { 0x0A008u, 0x000002F9u },
        { 0x0A010u, 0x00001073u },
        { 0x0A014u, 0x00001061u },
        { 0x0A018u, 0x000010B1u },
        { 0x0A01Cu, 0x00000004u },
        { 0x0A020u, 0x00000078u },
        { 0x0A024u, 0x00000040u },
        { 0x0A030u, 0x00000040u },
        { 0x0A034u, 0x0000000Cu },
        { 0x0A040u, 0x00009EFFu },
        { 0x0A060u, 0x000C000Cu }, { 0x0A064u, 0x000C000Cu },
        { 0x0A068u, 0x000C000Cu }, { 0x0A06Cu, 0x000C000Cu },
        { 0x0A070u, 0x000C000Cu }, { 0x0A074u, 0x000C000Cu },
        { 0x0A078u, 0x000C000Cu }, { 0x0A07Cu, 0x000C000Cu },
        { 0x0A084u, 0x00000001u },
        { 0x0A08Cu, 0x00007100u },
        { 0x0A090u, 0x00007100u }, { 0x0A094u, 0x00007100u },
        { 0x0A098u, 0x00007100u }, { 0x0A09Cu, 0x00007100u },
        { 0x0A0A0u, 0x00007100u }, { 0x0A0A4u, 0x00007100u },
        { 0x0A0A8u, 0x00007100u }, { 0x0A0ACu, 0x00007100u },
        { 0x0A0B0u, 0x00007100u }, { 0x0A0B4u, 0x00007100u },
        { 0x0A0B8u, 0x00007100u }, { 0x0A0BCu, 0x00007100u },
        { 0x0A0C0u, 0x00007100u },
        { 0x0A0C4u, 0x00000003u },
        { 0x0A0C8u, 0x0000FFFFu },
        { 0x0A0D0u, 0x00000002u },
        { 0x0A0DCu, 0x00000005u },
        { 0x0A0E0u, 0x00000041u },
        { 0x0A0E4u, 0x0000000Fu },
        { 0x0A0F4u, 0x00007100u },
        { 0x0A0F8u, 0x00007100u },
        { 0x0A0FCu, 0x00007100u },
        { 0x0A140u, 0x00000001u },
        { 0x0A19Cu, 0x00000001u },
        { 0x0A1A0u, 0x00000001u },
        { 0x0A1A4u, 0x00000001u },
        { 0x0A1C8u, 0x00000001u },
        { 0x0A1CCu, 0x00000040u },
    };

    for (unsigned i = 0; i < (sizeof(seed_words) / sizeof(seed_words[0])); i++)
        vrc4173_latch_write(seed_words[i].off, 4u, seed_words[i].val);

    /* NAND sideband latch observed in nand_sniff_v3.txt. */
    vrc4173_set_nand_c38(NULL, VRC4173_NAND_C38_INIT, 0u, "seed");
}

static uint64_t vrc4173_read_cb(uc_engine *uc, uint64_t offset,
                                 unsigned size, void *user_data)
{
    vrc4173_cb_ctx_t *ctx = user_data;
    machine_t *m = ctx->m;
    uint32_t cs3_off = vrc4173_cs3_off(ctx, offset);
    uint32_t pc32 = 0;
    uint64_t val = 0;

    if (m->cfg.log_wince_stall || m->cfg.log_nand_legacy || m->cfg.log_mmio)
        pc32 = read_pc32(uc);

    if (m->cfg.log_mmio)
        fprintf(stderr, "[VRC4173] R%u cb_off=0x%05" PRIX64 " cs3_off=0x%05X\n",
                size * 8, offset, cs3_off);

    if (cs3_off == UINT32_MAX)
        goto out;
    vrc4173_seed_core_dump_once();

    if (cs3_off == VRC4173_BOOT_ID_OFF && m->cfg.log_wince_stall) {
        static uint32_t boot_id_probe_logs = 0;
        if (boot_id_probe_logs < 16u) {
            fprintf(stderr,
                    "[VRC4173_BOOT_ID] R%u cs3_off=0x%05X cb_off=0x%05" PRIX64
                    " pc=0x%08X\n",
                    size * 8, cs3_off, offset, pc32);
            boot_id_probe_logs++;
        }
    }

    if (cs3_off == VRC4173_NAND_C38_OFF) {
        val = vrc4173_nand_c38;
        goto out;
    }

    /* VRC4173 UART (NS16550-compatible, 4-byte register spacing) */
    switch (cs3_off) {
    case VRC4173_BOOT_ID_OFF: val = VRC4173_BOOT_ID_VALUE; break;
    case VRC4173_UART_THR:   val = 0; break;       /* RBR: no data */
    case VRC4173_UART_IER:   val = 0; break;       /* IER: no interrupts enabled */
    case VRC4173_UART_IIR:   val = 0x01u; break;   /* IIR: no pending interrupt */
    case VRC4173_UART_LCR:   val = 0x03u; break;   /* LCR: 8N1 */
    case VRC4173_UART_MCR:   val = 0; break;       /* MCR: no modem control */
    case VRC4173_UART_LSR:   val = 0x60u; break;   /* TEMT | THRE: tx ready */
    case VRC4173_UART_MSR:   val = 0; break;       /* MSR: no modem status */
    case VRC4173_UART_SCR:   val = vrc4173_uart_scr; break;
    default: break;
    }
    if (cs3_off >= VRC4173_UART_THR && cs3_off <= VRC4173_UART_SCR &&
        ((cs3_off - VRC4173_UART_THR) % 4u) == 0u)
        goto out;

    /* NAND controller registers */
    if (is_nand_offset(cs3_off)) {
        val = nand_read(&m->nand, cs3_off, size,
                        m->cfg.log_mmio || m->cfg.log_nand_legacy,
                        pc32);
        goto out;
    }

    /* No onboard NE2000: deterministic no-card behavior for C-window probes. */
    if (is_ne2k_cwin_offset(cs3_off)) {
        if (cs3_off == 0x0C48u)
            val = UINT32_C(0x00000001);
        else if (cs3_off == 0x0C4Cu)
            val = UINT32_C(0x00000000);
        else if (size == 4 && (cs3_off & 3u) == 0u)
            val = vrc4173_ne2k_cwin_latch[ne2k_cwin_idx(cs3_off)];

        if (m->cfg.log_mmio)
            fprintf(stderr, "[VRC4173_CWIN] R%u cs3_off=0x%05X -> 0x%" PRIX64 "\n",
                    size * 8, cs3_off, val);
        goto out;
    }

    /* Companion chip stub registers */
    if (cs3_off >= 0x0300u && cs3_off < 0x0400u)
        goto out;           /* Touch panel: no pen down */
    if (cs3_off >= 0x1000u && cs3_off < 0x1100u)
        val = 0x0Cu;        /* CF status: card removed */
    if (cs3_off >= 0x8000u && cs3_off < 0x8100u)
        goto out;           /* Vic/CommMode */
    if (vrc4173_latch_read(cs3_off, size, &val))
        goto out;

out:
    {
        uint64_t pa = (uint64_t)ctx->region_base_pa + offset;
        if (pa <= UINT32_MAX)
            machine_mmio_history_record(m, false, (uint32_t)pa, size, val, pc32);
    }
    return val;
}

static void vrc4173_write_cb(uc_engine *uc, uint64_t offset,
                              unsigned size, uint64_t value, void *user_data)
{
    vrc4173_cb_ctx_t *ctx = user_data;
    machine_t *m = ctx->m;
    uint32_t cs3_off = vrc4173_cs3_off(ctx, offset);
    uint32_t pc32 = 0;

    if (m->cfg.log_wince_stall || m->cfg.log_nand_legacy || m->cfg.log_mmio)
        pc32 = read_pc32(uc);

    if (m->cfg.log_mmio)
        fprintf(stderr, "[VRC4173] W%u cb_off=0x%05" PRIX64 " cs3_off=0x%05X <- 0x%" PRIX64 "\n",
                size * 8, offset, cs3_off, value);

    if (cs3_off == UINT32_MAX)
        goto out;
    vrc4173_seed_core_dump_once();

    switch (cs3_off) {
    case VRC4173_UART_THR:
#ifndef __ANDROID__
        {
            int ch = (int)(value & 0xFF);
            putchar(ch);
            fflush(stdout);
        }
#endif
        break;
    case VRC4173_UART_SCR:
        vrc4173_uart_scr = (uint8_t)(value & 0xFF);
        break;
    case VRC4173_BOOT_ID_OFF:
        /* Read-only identity probe on hardware; ignore writes. */
        break;
    case VRC4173_NAND_C38_OFF:
        vrc4173_set_nand_c38(m, (uint32_t)value, pc32, "guest_write");
        break;
    default:
        /* No onboard NE2000: latch C-window writes, but never signal presence. */
        if (is_ne2k_cwin_offset(cs3_off)) {
            if (size == 4 && (cs3_off & 3u) == 0u)
                vrc4173_ne2k_cwin_latch[ne2k_cwin_idx(cs3_off)] = (uint32_t)value;
            goto out;
        }

        /* NAND controller registers */
        if (is_nand_offset(cs3_off)) {
            nand_write(&m->nand, cs3_off, size, value,
                       m->cfg.log_mmio || m->cfg.log_nand_legacy,
                       pc32);
            vrc4173_nand_sideband_write(m, cs3_off, value, pc32);
        } else {
            vrc4173_latch_write(cs3_off, size, value);
        }
        break;
    }
out:
    {
        uint64_t pa = (uint64_t)ctx->region_base_pa + offset;
        if (pa <= UINT32_MAX)
            machine_mmio_history_record(m, true, (uint32_t)pa, size, value, pc32);
    }
}

/* ------------------------------------------------------------------ */
/* Unicorn MMIO callbacks                                               */
/* ------------------------------------------------------------------ */

static uint64_t mmio_read_cb(uc_engine *uc, uint64_t offset,
                              unsigned size, void *user_data)
{
    machine_t *m = user_data;
    uint64_t val = 0;
    uint32_t pc32 = 0;

    if (m->cfg.log_wince_stall)
        pc32 = read_pc32(uc);

    if (offset >= IO_BCU_BASE && offset < IO_BCU_BASE + IO_BCU_SIZE)
        val = bcu_read(&m->bcu, (uint32_t)(offset - IO_BCU_BASE), size);
    else if (offset >= IO_PMU_BASE && offset < IO_PMU_BASE + IO_PMU_SIZE)
        val = pmu_read(&m->pmu, (uint32_t)(offset - IO_PMU_BASE), size);
    else if (offset >= IO_CMU_BASE && offset < IO_CMU_BASE + IO_CMU_SIZE)
        val = cmu_read(&m->cmu, (uint32_t)(offset - IO_CMU_BASE), size);
    else if (offset >= IO_ICU_BASE && offset < IO_ICU_BASE + IO_ICU_SIZE) {
        m->saw_icu_mmio = true;
        val = icu_read(&m->icu, (uint32_t)(offset - IO_ICU_BASE), size);
    }
    else if (offset >= IO_RTC_BASE && offset < IO_RTC_BASE + IO_RTC_SIZE)
        val = rtc_read(&m->rtc, (uint32_t)(offset - IO_RTC_BASE), size);
    else if (offset >= IO_GPIO_BASE && offset < IO_GPIO_BASE + IO_GPIO_SIZE)
        val = gpio_read(&m->gpio, (uint32_t)(offset - IO_GPIO_BASE), size);
    else if (offset >= IO_SIU_BASE && offset < IO_SIU_BASE + IO_SIU_SIZE)
        val = siu_read(&m->siu, (uint32_t)(offset - IO_SIU_BASE), size);
    else {
        /* Unrecognized internal I/O: read from write-latched shadow bytes.
         * This preserves write->readback behavior for unknown blocks
         * (for example, WinCE probing ranges around 0x0F0000C0). */
        bool have_latch = true;
        if (offset + size > PA_IO_SIZE)
            have_latch = false;
        else {
            val = 0;
            for (unsigned i = 0; i < size; i++) {
                uint32_t idx = (uint32_t)offset + i;
                if (!m->io_fallback_valid[idx]) {
                    have_latch = false;
                    break;
                }
                val |= (uint64_t)m->io_fallback_bytes[idx] << (i * 8u);
            }
        }
        if (!have_latch)
            val = 0;
        if (m->cfg.log_mmio)
            fprintf(stderr, "[BUS] Unhandled MMIO read  @ PA 0x%08" PRIX64
                    " (offset 0x%03" PRIX64 ") size %u%s\n",
                    PA_IO_BASE + offset, offset, size,
                    have_latch ? " [latched]" : "");
    }

    if (m->cfg.log_mmio)
        fprintf(stderr, "[MMIO] R%u PA=0x%08" PRIX64 " -> 0x%" PRIX64 "\n",
                size * 8, PA_IO_BASE + offset, val);

    machine_mmio_history_record(m, false, (uint32_t)(PA_IO_BASE + offset),
                                size, val, pc32);
    return val;
}

static void mmio_write_cb(uc_engine *uc, uint64_t offset,
                           unsigned size, uint64_t value, void *user_data)
{
    machine_t *m = user_data;
    uint32_t pc32 = 0;

    if (m->cfg.log_wince_stall)
        pc32 = read_pc32(uc);

    if (m->cfg.log_mmio)
        fprintf(stderr, "[MMIO] W%u PA=0x%08" PRIX64 " <- 0x%" PRIX64 "\n",
                size * 8, PA_IO_BASE + offset, value);

    if (offset >= IO_BCU_BASE && offset < IO_BCU_BASE + IO_BCU_SIZE)
        bcu_write(&m->bcu, (uint32_t)(offset - IO_BCU_BASE), size, (uint32_t)value);
    else if (offset >= IO_PMU_BASE && offset < IO_PMU_BASE + IO_PMU_SIZE)
        pmu_write(&m->pmu, (uint32_t)(offset - IO_PMU_BASE), size, (uint32_t)value);
    else if (offset >= IO_CMU_BASE && offset < IO_CMU_BASE + IO_CMU_SIZE)
        cmu_write(&m->cmu, (uint32_t)(offset - IO_CMU_BASE), size, (uint32_t)value);
    else if (offset >= IO_ICU_BASE && offset < IO_ICU_BASE + IO_ICU_SIZE) {
        m->saw_icu_mmio = true;
        icu_write(&m->icu, (uint32_t)(offset - IO_ICU_BASE), size, (uint32_t)value);
    }
    else if (offset >= IO_RTC_BASE && offset < IO_RTC_BASE + IO_RTC_SIZE)
        rtc_write(&m->rtc, (uint32_t)(offset - IO_RTC_BASE), size, (uint32_t)value);
    else if (offset >= IO_GPIO_BASE && offset < IO_GPIO_BASE + IO_GPIO_SIZE)
        gpio_write(&m->gpio, (uint32_t)(offset - IO_GPIO_BASE), size, (uint32_t)value);
    else if (offset >= IO_SIU_BASE && offset < IO_SIU_BASE + IO_SIU_SIZE)
        siu_write(&m->siu, (uint32_t)(offset - IO_SIU_BASE), size, (uint32_t)value);
    else {
        /* Latch unknown internal-I/O writes for deterministic readback. */
        if (offset + size <= PA_IO_SIZE) {
            for (unsigned i = 0; i < size; i++) {
                uint32_t idx = (uint32_t)offset + i;
                m->io_fallback_bytes[idx] = (uint8_t)(value >> (i * 8u));
                m->io_fallback_valid[idx] = 1u;
            }
        }
        if (m->cfg.log_mmio)
            fprintf(stderr, "[BUS] Unhandled MMIO write @ PA 0x%08" PRIX64
                    " (offset 0x%03" PRIX64 ") size %u value 0x%" PRIX64 "\n",
                    PA_IO_BASE + offset, offset, size, value);
    }

    machine_mmio_history_record(m, true, (uint32_t)(PA_IO_BASE + offset),
                                size, value, pc32);
}

/* ------------------------------------------------------------------ */
/* Memory-fault hook (unmapped access diagnostic)                       */
/* ------------------------------------------------------------------ */

/*
 * Memory-fault handler.
 *
 * For READ/WRITE to unmapped physical regions: map a 1 MB zero page and
 * allow execution to continue.  This lets the kernel probe unknown external
 * peripherals (VRC4173, external bus CS2-CS5, etc.) without crashing; the
 * kernel will see zeros and typically skip or stub-out the missing hardware.
 *
 * For FETCH from unmapped code: stop — executing unknown code is fatal.
 */
static bool mem_fault_cb(uc_engine *uc, uc_mem_type type,
                          uint64_t address, int size,
                          int64_t value, void *user_data  __attribute__((unused)))
{
    (void)value;
    uint64_t pc = 0;
    uc_reg_read(uc, UC_MIPS_REG_PC, &pc);

    if (type == UC_MEM_FETCH_UNMAPPED) {
        fprintf(stderr, "[BUS] Unmapped FETCH @ 0x%016" PRIX64
                " PC=0x%016" PRIX64 " — STOP\n", address, pc);
        return false;
    }

    /* Write-protection fault: region is mapped but not writable (e.g., ROM). */
    if (type == UC_MEM_WRITE_PROT) {
        fprintf(stderr, "[BUS] Write-prot WRITE @ 0x%016" PRIX64
                " size=%d PC=0x%016" PRIX64 " — ignoring\n", address, size, pc);
        return true;   /* skip the write, continue */
    }

    const char *kind = (type == UC_MEM_READ_UNMAPPED)  ? "READ"
                     : (type == UC_MEM_WRITE_UNMAPPED) ? "WRITE"
                     : "UNKNOWN";
    fprintf(stderr, "[BUS] Unmapped %s @ 0x%016" PRIX64
            " size=%d PC=0x%016" PRIX64 "\n", kind, address, size, pc);
    fflush(stderr);

    /* Map a 1 MB zeroed page at the 1 MB-aligned base of the faulting address */
    uint64_t block = address & ~((uint64_t)0xFFFFF);
    uc_err err = uc_mem_map(uc, block, 0x100000, UC_PROT_ALL);
    if (err != UC_ERR_OK && err != UC_ERR_MAP) {
        /* UC_ERR_MAP = already mapped (overlap), which is fine to ignore */
        fprintf(stderr, "[BUS]   uc_mem_map @ 0x%016" PRIX64 " failed: %s\n",
                block, uc_strerror(err));
        return false;
    }
    return true;    /* retry the faulting access — memory now zeroed */
}

/* ------------------------------------------------------------------ */

static bool map_sdram_alias_ptr(machine_t *m, uint64_t base, const char *tag)
{
    uc_err err = uc_mem_map_ptr(m->uc, base, m->cfg.sdram_size,
                                UC_PROT_ALL, m->sdram_backing);
    if (err == UC_ERR_OK || err == UC_ERR_MAP)
        return true;
    fprintf(stderr, "[ALIAS_MODE] map_ptr %s @ 0x%016" PRIX64 " failed: %s\n",
            tag, base, uc_strerror(err));
    return false;
}

typedef struct {
    uint16_t off;
    uint32_t val;
} io_seed_word_t;

static void seed_internal_io_fallback(machine_t *m)
{
    /*
     * Warm-state core-page snapshot from hardware_survey/HardwareDump6.txt
     * and docs/HARDWARE_GROUND_TRUTH.md (PA 0x0F000000 page).
     * These values are only used for unmodeled internal-I/O offsets.
     */
    static const io_seed_word_t seed_words[] = {
        { 0x000u, 0x0000000Cu }, { 0x004u, 0x100C4444u },
        { 0x008u, 0x26721242u },
        { 0x010u, 0x00005002u }, { 0x014u, 0x0883020Cu },
        { 0x020u, 0x01FFF800u }, { 0x024u, 0x01FFF800u },
        { 0x028u, 0x01FFF800u }, { 0x02Cu, 0x01FFF800u },
        { 0x030u, 0x00003800u }, { 0x034u, 0x00003800u },
        { 0x040u, 0x00010000u },
        { 0x060u, 0x09020902u }, { 0x064u, 0x09020902u },
        { 0x068u, 0x09020902u }, { 0x06Cu, 0x09020902u },
        { 0x070u, 0x09020902u }, { 0x074u, 0x09020902u },
        { 0x078u, 0x09020902u }, { 0x07Cu, 0x09020902u },
        { 0x080u, 0x00000004u }, { 0x08Cu, 0x00000307u },
        { 0x094u, 0x00000E83u }, { 0x098u, 0x00000001u },
        { 0x0A0u, 0x00000001u }, { 0x0ACu, 0x00010000u },
        { 0x0C0u, 0x10061400u }, { 0x0C8u, 0x00000148u },
        { 0x0CCu, 0x00000002u }, { 0x0D0u, 0x020003FFu },
        { 0x0D4u, 0x000007FFu },
        { 0x100u, 0x7C124A74u }, { 0x104u, 0x0000A5B3u },
        { 0x108u, 0x00011C2Fu },
        { 0x110u, 0x00000021u }, { 0x114u, 0x00000004u },
        { 0x118u, 0x0000FFFFu }, { 0x11Cu, 0x0000F7A1u },
        { 0x130u, 0x00060000u },
        { 0x140u, 0x00000070u }, { 0x144u, 0x0000AAE2u },
        { 0x148u, 0xFFFF111Cu }, { 0x14Cu, 0x00000E83u },
        { 0x154u, 0x00000401u }, { 0x158u, 0x0000BFFFu },
        { 0x180u, 0x00200010u }, { 0x184u, 0x000014C8u },
        { 0x188u, 0x04B00002u },
        { 0x1E0u, 0x01FFF800u }, { 0x1E4u, 0x01FFF800u },
        { 0x1E8u, 0x0A000000u }, { 0x1ECu, 0x0A000000u },
    };

    for (unsigned n = 0; n < (sizeof(seed_words) / sizeof(seed_words[0])); n++) {
        uint32_t off = seed_words[n].off;
        uint32_t val = seed_words[n].val;
        if ((uint64_t)off + 4u > PA_IO_SIZE)
            continue;
        for (unsigned i = 0; i < 4u; i++) {
            m->io_fallback_bytes[off + i] = (uint8_t)((val >> (i * 8u)) & 0xFFu);
            m->io_fallback_valid[off + i] = 1u;
        }
    }
}

/* ------------------------------------------------------------------ */

void bus_init(machine_t *m)
{
    uc_err err;
    bool pa_sdram_mapped = false;

    m->shared_alias_active = false;
    m->alias_fallback_sync_active = false;
    seed_internal_io_fallback(m);

    /* SDRAM — read/write/exec */
    if (m->sdram_backing != NULL &&
        m->sdram_backing_size >= (size_t)m->cfg.sdram_size) {
        bool pa_ok = map_sdram_alias_ptr(m, PA_SDRAM_BASE, "PA");
        bool kseg0_ok = map_sdram_alias_ptr(m, UINT64_C(0x0000000080000000), "kseg0");
        bool kseg1_ok = map_sdram_alias_ptr(m, UINT64_C(0x00000000A0000000), "kseg1");
        bool kseg0_sx_ok = map_sdram_alias_ptr(m, UINT64_C(0xFFFFFFFF80000000), "kseg0_sx");
        bool kseg1_sx_ok = map_sdram_alias_ptr(m, UINT64_C(0xFFFFFFFFA0000000), "kseg1_sx");

        pa_sdram_mapped = pa_ok;
        if (pa_ok && kseg0_ok && kseg1_ok) {
            m->shared_alias_active = true;
            fprintf(stderr,
                    "[ALIAS_MODE] shared SDRAM backing active size=0x%08X"
                    " kseg0_sx=%u kseg1_sx=%u\n",
                    m->cfg.sdram_size,
                    kseg0_sx_ok ? 1u : 0u,
                    kseg1_sx_ok ? 1u : 0u);
        } else {
            m->alias_fallback_sync_active = true;
            fprintf(stderr,
                    "[ALIAS_MODE] shared SDRAM alias incomplete"
                    " pa=%u kseg0=%u kseg1=%u kseg0_sx=%u kseg1_sx=%u"
                    " -> enabling write-sync fallback\n",
                    pa_ok ? 1u : 0u,
                    kseg0_ok ? 1u : 0u,
                    kseg1_ok ? 1u : 0u,
                    kseg0_sx_ok ? 1u : 0u,
                    kseg1_sx_ok ? 1u : 0u);
        }
    } else {
        m->alias_fallback_sync_active = true;
        fprintf(stderr,
                "[ALIAS_MODE] no shared SDRAM backing"
                " -> using map+write-sync fallback\n");
    }

    if (!pa_sdram_mapped) {
        err = uc_mem_map(m->uc, PA_SDRAM_BASE, m->cfg.sdram_size, UC_PROT_ALL);
        if (err != UC_ERR_OK && err != UC_ERR_MAP)
            fprintf(stderr, "[BUS] SDRAM map failed: %s\n", uc_strerror(err));
    }

    /* ROM/Flash — read/exec only */
    err = uc_mem_map(m->uc, PA_ROM_BASE, PA_ROM_SIZE,
                     UC_PROT_READ | UC_PROT_EXEC);
    if (err != UC_ERR_OK)
        fprintf(stderr, "[BUS] ROM map failed: %s\n", uc_strerror(err));

    /* Internal I/O — MMIO with callbacks */
    err = uc_mmio_map(m->uc, PA_IO_BASE, PA_IO_SIZE,
                      mmio_read_cb,  m,
                      mmio_write_cb, m);
    if (err != UC_ERR_OK)
        fprintf(stderr, "[BUS] Internal I/O MMIO map failed: %s\n", uc_strerror(err));

    /* Region 1: VRC4173 companion chip (Base to Framebuffer start) */
    m->vrc4173_region1_ctx.m = m;
    m->vrc4173_region1_ctx.region_base_pa = PA_VRC4173_BASE;
    err = uc_mmio_map(m->uc, PA_VRC4173_BASE, PA_FRAMEBUFFER_BASE - PA_VRC4173_BASE,
                      vrc4173_read_cb,  &m->vrc4173_region1_ctx,
                      vrc4173_write_cb, &m->vrc4173_region1_ctx);
    if (err != UC_ERR_OK)
        fprintf(stderr, "[BUS] VRC4173 MMIO Region 1 map failed: %s\n", uc_strerror(err));

    /* Region 2: Framebuffer VRAM (mapped as RAM) */
    err = uc_mem_map(m->uc, PA_FRAMEBUFFER_BASE, PA_FRAMEBUFFER_SIZE, UC_PROT_ALL);
    if (err != UC_ERR_OK)
        fprintf(stderr, "[BUS] Framebuffer VRAM map failed: %s\n", uc_strerror(err));

    /* Region 3: After Framebuffer to end of CS3 */
    uint32_t region3_base = PA_FRAMEBUFFER_BASE + PA_FRAMEBUFFER_SIZE;
    uint32_t region3_size = (PA_VRC4173_BASE + PA_VRC4173_SIZE) - region3_base;
    if (region3_size > 0) {
        m->vrc4173_region3_ctx.m = m;
        m->vrc4173_region3_ctx.region_base_pa = region3_base;
        err = uc_mmio_map(m->uc, region3_base, region3_size,
                          vrc4173_read_cb,  &m->vrc4173_region3_ctx,
                          vrc4173_write_cb, &m->vrc4173_region3_ctx);
        if (err != UC_ERR_OK)
            fprintf(stderr, "[BUS] VRC4173 MMIO Region 2 map failed: %s\n", uc_strerror(err));
    }


    /* Fault hook for unmapped accesses */
    uc_hook hk;
    uc_hook_add(m->uc, &hk,
                UC_HOOK_MEM_READ_UNMAPPED  |
                UC_HOOK_MEM_WRITE_UNMAPPED |
                UC_HOOK_MEM_FETCH_UNMAPPED |
                UC_HOOK_MEM_WRITE_PROT     |
                UC_HOOK_MEM_INVALID,
                mem_fault_cb, m, 1, 0);
}
