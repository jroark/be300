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

#include "wince_probe_seed_data.h"
#include "wince_kdata_probe_words.h"
#include "wince_bootrom_words.h"

static inline bool is_wince_probe_deferred_pa(uint32_t pa)
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
    /*
     * Warm-state KData snapshot from hardware_survey/BE300Probe_v1.txt
     * baseline (PA 0x00002000..0x000023FF).
     */
    for (uint32_t i = 0;
         i < (uint32_t)(sizeof(wince_kdata_words) / sizeof(wince_kdata_words[0]));
         i++) {
        uint32_t pa = 0x00002000u + (i * 4u);
        write_pa_u32_all_aliases(m, pa, wince_kdata_words[i]);
    }

    /*
     * The post-boot KData snapshot includes a live context record at
     * PA 0x00002200 whose saved sp/ra restore into the alternate entry
     * path at 0x80096894. That resumed path later expects the function's
     * saved s0/ra slots to exist at PA 0x1760/0x1764, but the post-boot
     * probe did not capture that stack window. Reconstruct the two slots
     * the skipped prologue would have written:
     *   sw s0, 0x20(sp)   → frame_s0_pa = ctx_s0
     *   sw ra, 0x24(sp)   → frame_ra_pa = real caller return address
     *
     * The ctx_ra from the KData context (0x80096894) is the resume PC
     * within the function — NOT the function's saved return address.
     * The actual caller is at VA 0x8008B6B8 (JAL 0x800967FC), so the
     * correct frame ra is the return address 0x8008B6C0.
     *
     * Previously, seeding ctx_ra into the frame caused the function to
     * "return" into its own mid-body, creating an infinite loop that
     * consumed stack until ra=0 → crash.
     */
    {
        uint32_t ctx_s0 = 0;
        uint32_t ctx_sp = 0;
        uint32_t ctx_ra = 0;
        bool ok_s0 = read_pa_u32_all_aliases(m, UINT32_C(0x00002238), &ctx_s0);
        bool ok_sp = read_pa_u32_all_aliases(m, UINT32_C(0x0000226C), &ctx_sp);
        bool ok_ra = read_pa_u32_all_aliases(m, UINT32_C(0x00002274), &ctx_ra);

        if (ok_s0 && ok_sp && ok_ra &&
            ctx_sp == UINT32_C(0xFFFFD7B4) &&
            ctx_ra == UINT32_C(0x80096894)) {
            uint32_t frame_s0_pa = UINT32_C(0x00001760);
            uint32_t frame_ra_pa = UINT32_C(0x00001764);
            /* Use the real caller's return address, not the context resume PC.
             * VA 0x8008B6B8: JAL 0x800967FC  → return addr = 0x8008B6C0 */
            uint32_t real_caller_ra = UINT32_C(0x8008B6C0);
            write_pa_u32_all_aliases(m, frame_s0_pa, ctx_s0);
            write_pa_u32_all_aliases(m, frame_ra_pa, real_caller_ra);
            fprintf(stderr,
                    "[WINCE_KDATA_CTX_FRAME_SEED] ctx_s0=0x%08X"
                    " ctx_sp=0x%08X ctx_ra=0x%08X"
                    " frame_s0_pa=0x%08X frame_ra_pa=0x%08X"
                    " real_caller_ra=0x%08X\n",
                    ctx_s0, ctx_sp, ctx_ra,
                    frame_s0_pa, frame_ra_pa, real_caller_ra);
        } else {
            fprintf(stderr,
                    "[WINCE_KDATA_CTX_FRAME_SEED] skipped"
                    " ok=[%u %u %u] ctx_s0=0x%08X ctx_sp=0x%08X ctx_ra=0x%08X\n",
                    ok_s0 ? 1u : 0u, ok_sp ? 1u : 0u, ok_ra ? 1u : 0u,
                    ctx_s0, ctx_sp, ctx_ra);
        }
    }

    fprintf(stderr,
            "[WINCE_KDATA_SEED] seeded %u words at PA=0x00002000\n",
            (unsigned)(sizeof(wince_kdata_words) / sizeof(wince_kdata_words[0])));
}

static void seed_wince_probe_boot_safe(machine_t *m)
{
    uint32_t applied = 0;
    uint32_t deferred = 0;

    for (uint32_t i = 0;
         i < (uint32_t)(sizeof(wince_probe_seed_words) / sizeof(wince_probe_seed_words[0]));
         i++) {
        if (is_wince_probe_deferred_pa(wince_probe_seed_words[i].pa)) {
            deferred++;
            continue;
        }
        write_pa_u32_all_aliases(m, wince_probe_seed_words[i].pa,
                                 wince_probe_seed_words[i].val);
        applied++;
    }

    fprintf(stderr,
            "[WINCE_PROBE_SEED_BOOT] applied=%u deferred=%u total=%u\n",
            applied, deferred,
            (unsigned)(sizeof(wince_probe_seed_words) / sizeof(wince_probe_seed_words[0])));
}

void seed_wince_probe_deferred(machine_t *m, uint32_t pc32)
{
    if (!m || m->wince_deferred_seed_done)
        return;

    uint32_t considered = 0;
    uint32_t applied = 0;
    uint32_t skipped_nonzero = 0;
    uint32_t skipped_read_fail = 0;

    for (uint32_t i = 0;
         i < (uint32_t)(sizeof(wince_probe_seed_words) / sizeof(wince_probe_seed_words[0]));
         i++) {
        uint32_t pa = wince_probe_seed_words[i].pa;
        uint32_t seed_val = wince_probe_seed_words[i].val;
        uint32_t cur = 0;

        if (!is_wince_probe_deferred_pa(pa))
            continue;
        considered++;

        if (!read_pa_u32_all_aliases(m, pa, &cur)) {
            skipped_read_fail++;
            continue;
        }
        if (cur != 0u) {
            skipped_nonzero++;
            continue;
        }

        write_pa_u32_all_aliases(m, pa, seed_val);
        applied++;
    }

    m->wince_deferred_seed_done = true;
    fprintf(stderr,
            "[WINCE_PROBE_SEED_DEFERRED] pc=0x%08X applied=%u"
            " skip_nonzero=%u skip_unreadable=%u considered=%u\n",
            pc32, applied, skipped_nonzero, skipped_read_fail, considered);
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
    if (skip_logs < 16u) {
        uint64_t a0 = 0, ra = 0;
        uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
        uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
        fprintf(stderr,
                "[WINCE_DELAY_DIAG] pc=0x80078038 target=0x80077210"
                " a0=0x%08X ra=0x%08X\n",
                (uint32_t)a0, (uint32_t)ra);
        skip_logs++;
    }
    return false;
}
