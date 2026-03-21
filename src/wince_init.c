#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "wince_init.h"
#include "machine.h"

typedef struct {
    uint16_t off;
    uint32_t val;
} wince_vec_seed_word_t;

typedef struct {
    uint32_t pa;
    uint32_t val;
} wince_pa_seed_word_t;

typedef struct {
    const char *name;
    uint32_t pa;
    uint32_t size;
    uint32_t crc32;
    const uint8_t *data;
} wince_pa_seed_region_t;

typedef struct {
    const char *name;
    uint32_t va;
    uint32_t size;
    uint32_t crc32;
    const uint8_t *data;
} wince_va_seed_region_t;

#include "wince_probe_seed_data.h"
#include "wince_hw_seed_data.h"
#include "wince_kdata_probe_words.h"
#include "wince_bootrom_words.h"

static inline bool __attribute__((unused)) is_wince_probe_deferred_pa(uint32_t pa)
{
    if (pa >= UINT32_C(0x00006000) && pa < UINT32_C(0x00007000))
        return true;
    if (pa >= UINT32_C(0x0001D000) && pa < UINT32_C(0x0001E000))
        return true;
    if (pa >= UINT32_C(0x0002D000) && pa < UINT32_C(0x0002E000))
        return true;
    if (pa >= UINT32_C(0x00051680) && pa < UINT32_C(0x00051B00))
        return true;
    if (pa >= UINT32_C(0x00660000) && pa < UINT32_C(0x00660040))
        return true;
    if (pa >= UINT32_C(0x0066BFC0) && pa < UINT32_C(0x00680000))
        return true;
    return false;
}

static void seed_wince_exception_vectors(machine_t *m)
{
    /*
     * Warm-state vector snapshot from hardware_survey/HardwareDump 2.txt.
     * This avoids zero-filled low vectors when WinCE exception paths run.
     */
    static const wince_vec_seed_word_t words[] = {
        { 0x0000, 0x401A4000u }, { 0x0004, 0xAC08D888u },
        { 0x0008, 0xAC1BD88Cu }, { 0x000C, 0x001A45C2u },
        { 0x0010, 0x07400018u }, { 0x0014, 0x310800FCu },
        { 0x0018, 0x8D08D8C0u }, { 0x001C, 0x001ADB82u },
        { 0x0020, 0x337B07FCu }, { 0x0024, 0x011B4021u },
        { 0x0028, 0x8D080000u }, { 0x002C, 0x001AD282u },
        { 0x0030, 0x335A0038u }, { 0x0034, 0x0501000Fu },
        { 0x0038, 0x0348D021u }, { 0x003C, 0x8D1B0000u },
        { 0x0040, 0x8C08D89Cu }, { 0x0044, 0x0368D824u },
        { 0x0048, 0x8F48000Cu }, { 0x004C, 0x101B0009u },
        { 0x0050, 0x8F5A0010u }, { 0x0054, 0x40881000u },
        { 0x0058, 0x409A1800u }, { 0x005C, 0x8C08D888u },
        { 0x0060, 0x8C1BD88Cu }, { 0x0064, 0x42000006u },
        { 0x0070, 0x42000018u }, { 0x0074, 0x8C1BD88Cu },
        { 0x0078, 0x08000060u }, { 0x007C, 0x8C08D888u },
        { 0x0108, 0x0001000Du }, { 0x010C, 0x42000018u },
        { 0x0188, 0x3C1A8008u }, { 0x018C, 0x375AB240u },
        { 0x0190, 0x03400008u },
    };

    for (unsigned i = 0; i < (sizeof(words) / sizeof(words[0])); i++) {
        write_pa_u32_all_aliases(m, (uint32_t)words[i].off, words[i].val);
    }

    fprintf(stderr, "[WINCE_VECTOR_SEED] seeded %u words into low vectors\n",
            (unsigned)(sizeof(words) / sizeof(words[0])));
}

static void seed_wince_kdata(machine_t *m)
{
    (void)m;
    fprintf(stderr, "[WINCE_KDATA_SEED] DISABLED — de-speculated (v4 survey: ctx_2200 3 families)\n");
}

static void seed_wince_probe_boot_safe(machine_t *m)
{
    (void)m;
    fprintf(stderr, "[WINCE_PROBE_SEED_BOOT] DISABLED — de-speculated (v4 survey: post-boot runtime page)\n");
}

static void seed_wince_obj_bootstrap(machine_t *m)
{
    static const wince_pa_seed_word_t words[WINCE_OBJ_BOOTSTRAP_WORD_COUNT] = {
        { UINT32_C(0x00660000), UINT32_C(0x8066BFC0) },
        { UINT32_C(0x0066BFC0), UINT32_C(0x8008AE98) },
        { UINT32_C(0x0066BFC4), UINT32_C(0x80078BA0) },
        { UINT32_C(0x0066BFC8), UINT32_C(0x800A4978) },
        { UINT32_C(0x0066BFCC), UINT32_C(0x8008AE98) },
        { UINT32_C(0x0066BFD0), UINT32_C(0x800A4978) },
        { UINT32_C(0x0066BFD4), UINT32_C(0x8008AE98) },
    };

    if (!m || !m->uc || !m->cfg.wince_obj_bootstrap || m->wince_obj_bootstrap_active)
        return;

    m->wince_obj_bootstrap_active = true;
    memset(m->wince_obj_bootstrap, 0, sizeof(m->wince_obj_bootstrap));

    for (uint32_t i = 0; i < WINCE_OBJ_BOOTSTRAP_WORD_COUNT; i++) {
        write_pa_u32_all_aliases(m, words[i].pa, words[i].val);
        m->wince_obj_bootstrap[i].pa = words[i].pa;
        m->wince_obj_bootstrap[i].seed_val = words[i].val;
    }

    fprintf(stderr,
            "[WINCE_OBJ_BOOTSTRAP] enabled"
            " pa[660000]=0x%08X"
            " objhdr=[0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X]\n",
            words[0].val,
            words[1].val, words[2].val, words[3].val,
            words[4].val, words[5].val, words[6].val);
}

static void write_pa_bytes_all_aliases(machine_t *m, uint32_t pa,
                                       const uint8_t *data, uint32_t size)
{
    if (!m || !m->uc || !data || size == 0u)
        return;

    uint64_t addrs[] = {
        (uint64_t)pa,
        UINT64_C(0x0000000080000000) + (uint64_t)pa,
        UINT64_C(0x00000000A0000000) + (uint64_t)pa,
        (uint64_t)mips_sext(UINT32_C(0x80000000) + pa),
        (uint64_t)mips_sext(UINT32_C(0xA0000000) + pa),
    };

    for (uint32_t i = 0; i < (uint32_t)(sizeof(addrs) / sizeof(addrs[0])); i++)
        uc_mem_write(m->uc, addrs[i], data, size);
}

static void write_va_bytes(machine_t *m, uint32_t va,
                           const uint8_t *data, uint32_t size)
{
    uc_err err;
    uint64_t addr;

    if (!m || !m->uc || !data || size == 0u)
        return;

    addr = (uint64_t)va;
    err = uc_mem_write(m->uc, addr, data, size);
    if (err == UC_ERR_OK)
        return;

    {
        uint64_t block = addr & ~UINT64_C(0xFFFFF);
        uc_mem_map(m->uc, block, UINT64_C(0x100000), UC_PROT_READ | UC_PROT_WRITE);
        uc_mem_write(m->uc, addr, data, size);
    }
}

static void seed_wince_hw_regions(machine_t *m)
{
    if (!m || !m->uc || !m->cfg.wince_hw_seed || m->wince_hw_seed_active)
        return;

    if (wince_hw_seed_region_count == 0u) {
        fprintf(stderr,
                "[WINCE_HW_SEED] requested but no generated regions are available\n");
        return;
    }

    m->wince_hw_seed_active = true;
    fprintf(stderr,
            "[WINCE_HW_SEED] applying %u captured PA regions and %u captured VA regions\n",
            wince_hw_seed_region_count,
            wince_hw_vseed_region_count);

    for (uint32_t i = 0; i < wince_hw_seed_region_count; i++) {
        const wince_pa_seed_region_t *r = &wince_hw_seed_regions[i];
        if (m->cfg.wince_hw_seed_skip_caller_frame &&
            r->name != NULL &&
            strcmp(r->name, "caller_frame") == 0) {
            fprintf(stderr,
                    "[WINCE_HW_SEED] skip region=%s pa=0x%08X size=0x%04X crc32=0x%08X reason=caller_frame_disabled\n",
                    r->name, r->pa, r->size, r->crc32);
            continue;
        }
        write_pa_bytes_all_aliases(m, r->pa, r->data, r->size);
        fprintf(stderr,
                "[WINCE_HW_SEED] region=%s pa=0x%08X size=0x%04X crc32=0x%08X\n",
                r->name ? r->name : "<unnamed>",
                r->pa, r->size, r->crc32);
    }

    for (uint32_t i = 0; i < wince_hw_vseed_region_count; i++) {
        const wince_va_seed_region_t *r = &wince_hw_vseed_regions[i];
        write_va_bytes(m, r->va, r->data, r->size);
        fprintf(stderr,
                "[WINCE_HW_SEED] vregion=%s va=0x%08X size=0x%04X crc32=0x%08X\n",
                r->name ? r->name : "<unnamed>",
                r->va, r->size, r->crc32);
    }

    if (m->cfg.wince_hw_seed_clear_callback_slot) {
        write_pa_u32_all_aliases(m, UINT32_C(0x006694F4), 0u);
        fprintf(stderr,
                "[WINCE_HW_SEED] clear_callback_slot pa=0x006694F4 old_use=jalr_a3 now=0x00000000\n");
    }

    if (m->cfg.wince_hw_seed_clear_callback_target) {
        write_pa_u32_all_aliases(m, UINT32_C(0x006694EC), 0u);
        fprintf(stderr,
                "[WINCE_HW_SEED] clear_callback_target pa=0x006694EC old_use=callback_target now=0x00000000\n");
    }

    if (m->cfg.wince_hw_seed_clear_caller_restore_s0) {
        write_pa_u32_all_aliases(m, UINT32_C(0x000017B0), 0u);
        fprintf(stderr,
                "[WINCE_HW_SEED] clear_caller_restore_s0 pa=0x000017B0 old_use=caller_restore_s0 now=0x00000000\n");
    }

    if (m->cfg.wince_hw_seed_force_alt_entry_prologue) {
        write_pa_u32_all_aliases(m, UINT32_C(0x00002274), UINT32_C(0x80096800));
        fprintf(stderr,
                "[WINCE_HW_SEED] force_alt_entry_prologue"
                " pa=0x00002274 old_use=ctx_saved_ra now=0x80096800\n");
    }

    if (m->cfg.wince_hw_seed_clear_future_frame) {
        write_pa_u32_all_aliases(m, UINT32_C(0x00001760), 0u);
        write_pa_u32_all_aliases(m, UINT32_C(0x00001764), 0u);
        fprintf(stderr,
                "[WINCE_HW_SEED] clear_future_frame pa=0x00001760,0x00001764 old_use=future_s0_ra now=0x00000000\n");
    }
}

void seed_wince_probe_deferred(machine_t *m, uint32_t pc32)
{
    if (!m || m->wince_deferred_seed_done)
        return;

    m->wince_deferred_seed_done = true;
    fprintf(stderr,
            "[WINCE_PROBE_SEED_DEFERRED] DISABLED — de-speculated (v4 survey: post-boot runtime page)"
            " pc=0x%08X\n", pc32);
}

static void seed_wince_bootrom_window(machine_t *m)
{
    if (!m || !m->uc)
        return;

    for (uint32_t i = 0; i < WINCE_BOOTROM_WORD_COUNT; i++) {
        uint32_t pa = PA_RESET_VECTOR + (i * 4u);
        uc_mem_write(m->uc, pa, &wince_bootrom_words[i], sizeof(uint32_t));
    }

    fprintf(stderr,
            "[WINCE_BOOTROM_SEED] seeded %u words at PA=0x%08X (CRC32=0xFA3B5582)\n",
            (unsigned)WINCE_BOOTROM_WORD_COUNT, PA_RESET_VECTOR);
}

static void apply_wince_bcu_warm_profile(machine_t *m)
{
    if (!m)
        return;

    /*
     * Post-boot core-page snapshot from BE300BootROM_v1.txt:
     *   PA 0x0F000000: 0000000C 100C4444 26721242 00000000
     *   PA 0x0F000010: 00005002 0883020C ...
     */
    m->bcu.bcucntreg1  = 0x000Cu;
    m->bcu.bcucntreg2  = 0x0000u;
    m->bcu.romsizereg  = 0x4444u;
    m->bcu.romspeedreg = 0x100Cu;
    m->bcu.io0sizereg  = 0x1242u;
    m->bcu.io0speedreg = 0x2672u;
    m->bcu.io1speedreg = 0x0000u;
    m->bcu.revidreg    = 0x5002u;
    m->bcu.clkspeedreg = 0x020Cu;

    fprintf(stderr,
            "[WINCE_BCU_WARM] BCUCNT1=0x%04X ROMSIZE=0x%04X ROMSPEED=0x%04X"
            " REVID=0x%04X CLKSPEED=0x%04X\n",
            m->bcu.bcucntreg1, m->bcu.romsizereg, m->bcu.romspeedreg,
            m->bcu.revidreg, m->bcu.clkspeedreg);
}

void apply_wince_warm_profile(machine_t *m, const char *marker)
{
    if (!m || !m->uc)
        return;

    uint64_t status = VR4131_STATUS_WARM;
    uc_reg_write(m->uc, UC_MIPS_REG_CP0_STATUS, &status);
    apply_wince_bcu_warm_profile(m);
    seed_wince_bootrom_window(m);
    seed_wince_exception_vectors(m);
    seed_wince_kdata(m);
    seed_wince_probe_boot_safe(m);
    seed_wince_obj_bootstrap(m);
    seed_wince_hw_regions(m);

    if (marker != NULL) {
        fprintf(stderr,
                "[%s] STATUS=0x%08X COUNT=0x%08X COMPARE=0x%08X\n",
                marker, VR4131_STATUS_WARM, m->cp0_count_base,
                m->cp0_compare_shadow);
    }
}


bool maybe_emulate_wince_objptr_init_call(machine_t *m, uc_engine *uc,
                                                 uint32_t pc32, uint32_t insn)
{
    if (!is_wince_boot_machine(m))
        return false;

    /* Diagnostics only: keep this site visible without forcing PC/register state. */
    if (pc32 != 0x80078BE0u || insn != 0x0C01DFF9u)
        return false;

    static uint32_t fix_logs = 0;
    if (fix_logs < 16u) {
        uint64_t a0 = 0, ra = 0;
        uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
        uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
        fprintf(stderr,
                "[WINCE_ISA_GAP_DIAG] pc=0x80078BE0 observed_call target=0x80077FE4"
                " ra=0x%08X a0=0x%08X\n",
                (uint32_t)ra, (uint32_t)a0);
        fix_logs++;
    }
    return false;
}

bool maybe_skip_wince_bootmode_delay_call(machine_t *m, uc_engine *uc,
                                                 uint32_t pc32, uint32_t insn)
{
    if (!is_wince_boot_machine(m))
        return false;
    if (pc32 != 0x80078038u || insn != 0x0C01DC84u)
        return false;

    static uint32_t skip_logs = 0;
    if (m->cfg.wince_delay_skip) {
        uint64_t a0 = 0, ra_before = 0;
        uint64_t next_pc = UINT64_C(0x80078040);
        uint64_t new_ra = UINT64_C(0x80078040);
        uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
        uc_reg_read(uc, UC_MIPS_REG_RA, &ra_before);
        m->wince_delay_call_trace.armed = false;
        uc_reg_write(uc, UC_MIPS_REG_RA, &new_ra);
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
        fprintf(stderr,
                "[WINCE_DELAY_SKIP] pc=0x80078038 target=0x80077210 next=0x80078040"
                " a0=0x%08X ra_before=0x%08X ra_after=0x%08X\n",
                (uint32_t)a0, (uint32_t)ra_before, (uint32_t)new_ra);
        return true;
    }

    if (!m->wince_delay_call_trace.entered && !m->wince_delay_call_trace.active) {
        m->wince_delay_call_trace.armed = true;
        m->wince_delay_call_trace.call_pc = pc32;
        m->wince_delay_call_trace.target_pc = UINT32_C(0x80077210);
        m->wince_delay_call_trace.expected_return_pc = UINT32_C(0x80078040);
    }

    if (skip_logs < 16u) {
        uint64_t a0 = 0, ra = 0;
        uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
        uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
        fprintf(stderr,
                "[WINCE_DELAY_DIAG] pc=0x80078038 target=0x80077210"
                " expect_return=0x80078040 armed=%u"
                " a0=0x%08X ra=0x%08X\n",
                m->wince_delay_call_trace.armed ? 1u : 0u,
                (uint32_t)a0, (uint32_t)ra);
        skip_logs++;
    }
    return false;
}
