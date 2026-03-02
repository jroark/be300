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
static uint64_t vrc4173_read_cb(uc_engine *uc, uint64_t offset,
                                 unsigned size, void *user_data)
{
    (void)uc;
    machine_t *m = user_data;

    if (m->cfg.log_mmio)
        fprintf(stderr, "[VRC4173] R%u offset=0x%05" PRIX64 "\n",
                size * 8, offset);

    if (offset == VRC4173_UART_LSR)
        return 0x60u;   /* TEMT | THRE — transmitter always ready */

    return 0;
}

static void vrc4173_write_cb(uc_engine *uc, uint64_t offset,
                              unsigned size, uint64_t value, void *user_data)
{
    (void)uc;
    machine_t *m = user_data;

    if (m->cfg.log_mmio)
        fprintf(stderr, "[VRC4173] W%u offset=0x%05" PRIX64 " <- 0x%" PRIX64 "\n",
                size * 8, offset, value);

    if (offset == VRC4173_UART_THR) {
        putchar((int)(value & 0xFF));
        fflush(stdout);
    }
}

/* ------------------------------------------------------------------ */
/* Unicorn MMIO callbacks                                               */
/* ------------------------------------------------------------------ */

static uint64_t mmio_read_cb(uc_engine *uc, uint64_t offset,
                              unsigned size, void *user_data)
{
    (void)uc;
    machine_t *m = user_data;
    uint64_t val = 0;

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
    else
        fprintf(stderr, "[BUS] Unhandled MMIO read  @ PA 0x%08" PRIX64
                " (offset 0x%03" PRIX64 ") size %u\n",
                PA_IO_BASE + offset, offset, size);

    if (m->cfg.log_mmio)
        fprintf(stderr, "[MMIO] R%u PA=0x%08" PRIX64 " -> 0x%" PRIX64 "\n",
                size * 8, PA_IO_BASE + offset, val);

    return val;
}

static void mmio_write_cb(uc_engine *uc, uint64_t offset,
                           unsigned size, uint64_t value, void *user_data)
{
    (void)uc;
    machine_t *m = user_data;

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
    else
        fprintf(stderr, "[BUS] Unhandled MMIO write @ PA 0x%08" PRIX64
                " (offset 0x%03" PRIX64 ") size %u value 0x%" PRIX64 "\n",
                PA_IO_BASE + offset, offset, size, value);
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

void bus_init(machine_t *m)
{
    uc_err err;

    /* SDRAM — read/write/exec */
    err = uc_mem_map(m->uc, PA_SDRAM_BASE, m->cfg.sdram_size, UC_PROT_ALL);
    if (err != UC_ERR_OK)
        fprintf(stderr, "[BUS] SDRAM map failed: %s\n", uc_strerror(err));

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

    /* Region 1: Base to Framebuffer start */
    err = uc_mmio_map(m->uc, PA_VRC4173_BASE, PA_FRAMEBUFFER_BASE - PA_VRC4173_BASE,
                      vrc4173_read_cb,  m,
                      vrc4173_write_cb, m);
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
        err = uc_mmio_map(m->uc, region3_base, region3_size,
                          vrc4173_read_cb,  m,
                          vrc4173_write_cb, m);
        if (err != UC_ERR_OK)
            fprintf(stderr, "[BUS] VRC4173 MMIO Region 2 map failed: %s\n", uc_strerror(err));
    }
        fprintf(stderr, "[BUS] MMIO map failed: %s\n", uc_strerror(err));


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
