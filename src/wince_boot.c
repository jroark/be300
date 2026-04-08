/*
 * wince_boot.c — WinCE cold-boot support for the BE-300 emulator.
 *
 * Phase 3 cleanup: all warm-boot, seed, replay, snapshot, and fault-site
 * diagnostics have been removed.  Only the cold-boot essentials remain.
 */

#include "wince_boot.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cop0.h"
#include "cpu.h"
#include "cpu_mips.h"
#include "devices.h"
#include "machine.h"
#include "memory.h"

static machine_t *g_active_wince_machine = NULL;
static const char *wince_gpr_names[] = MIPS_REGISTER_NAMES;

#define WINCE_COLD_LATE_PROBE_LOGGED UINT32_C(0x00200000)
#define WINCE_PATH_PROBE_77820      UINT64_C(0x0000000000000001)
#define WINCE_PATH_PROBE_79488      UINT64_C(0x0000000000000002)
#define WINCE_PATH_PROBE_794C8      UINT64_C(0x0000000000000004)
#define WINCE_PATH_PROBE_795D8      UINT64_C(0x0000000000000008)
#define WINCE_PATH_PROBE_79634      UINT64_C(0x0000000000000010)
#define WINCE_PATH_PROBE_79730      UINT64_C(0x0000000000000020)
#define WINCE_PATH_PROBE_76FBC      UINT64_C(0x0000000000000040)
#define WINCE_PATH_PROBE_76FE0      UINT64_C(0x0000000000000080)
#define WINCE_PATH_PROBE_7767C      UINT64_C(0x0000000000000100)
#define WINCE_PATH_PROBE_776F0      UINT64_C(0x0000000000000200)
#define WINCE_PATH_PROBE_77770      UINT64_C(0x0000000000000400)
#define WINCE_PATH_PROBE_79430      UINT64_C(0x0000000000000800)
#define WINCE_PATH_PROBE_79460      UINT64_C(0x0000000000001000)
#define WINCE_PATH_PROBE_79510      UINT64_C(0x0000000000002000)
#define WINCE_PATH_PROBE_772F0      UINT64_C(0x0000000000004000)
#define WINCE_PATH_PROBE_7742C      UINT64_C(0x0000000000008000)
#define WINCE_PATH_PROBE_7757C      UINT64_C(0x0000000000010000)
#define WINCE_PATH_PROBE_77664      UINT64_C(0x0000000000020000)
#define WINCE_PATH_PROBE_77738      UINT64_C(0x0000000000040000)
#define WINCE_PATH_PROBE_77260      UINT64_C(0x0000000000080000)
#define WINCE_PATH_PROBE_76B50      UINT64_C(0x0000000000100000)
#define WINCE_PATH_PROBE_76BA0      UINT64_C(0x0000000000200000)
#define WINCE_PATH_PROBE_76C60      UINT64_C(0x0000000000400000)
#define WINCE_PATH_PROBE_76CBC      UINT64_C(0x0000000000800000)
#define WINCE_PATH_PROBE_76E68      UINT64_C(0x0000000001000000)
#define WINCE_PATH_PROBE_77344      UINT64_C(0x0000000002000000)
#define WINCE_PATH_PROBE_775B0      UINT64_C(0x0000000004000000)
#define WINCE_PATH_PROBE_775B8      UINT64_C(0x0000000008000000)
#define WINCE_PATH_PROBE_7794C      UINT64_C(0x0000000010000000)
#define WINCE_PATH_PROBE_7796C      UINT64_C(0x0000000020000000)
#define WINCE_PATH_PROBE_77A14      UINT64_C(0x0000000040000000)
#define WINCE_PATH_PROBE_7B398      UINT64_C(0x0000000080000000)
#define WINCE_PATH_PROBE_7B57C      UINT64_C(0x0000000100000000)
#define WINCE_PATH_PROBE_947C8      UINT64_C(0x0000000200000000)
#define WINCE_PATH_PROBE_8B21C      UINT64_C(0x0000000400000000)
#define WINCE_PATH_PROBE_8B528      UINT64_C(0x0000000800000000)
#define WINCE_PATH_PROBE_7A3FC      UINT64_C(0x0000001000000000)
#define WINCE_PATH_PROBE_79898      UINT64_C(0x0000002000000000)
#define WINCE_PATH_PROBE_79910      UINT64_C(0x0000004000000000)
#define WINCE_PATH_PROBE_79990      UINT64_C(0x0000008000000000)

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static machine_t *wince_boot_from_gx(struct machine *gxm)
{
    if (!g_active_wince_machine)
        return NULL;
    if (g_active_wince_machine->gxe_machine != gxm)
        return NULL;
    return g_active_wince_machine;
}

static uint64_t pa_to_kseg0(uint32_t pa)
{
    return 0xffffffff80000000ULL | (uint64_t)pa;
}

static uint64_t va32_to_mips64(uint32_t va)
{
    return (uint64_t)(int64_t)(int32_t)va;
}

static uint32_t load_pa_word(machine_t *m, uint32_t pa)
{
    unsigned char *host = memory_paddr_to_hostaddr(m->cpu->mem, pa, MEM_READ);
    if (!host)
        return 0;
    return (uint32_t)host[0]
         | ((uint32_t)host[1] << 8)
         | ((uint32_t)host[2] << 16)
         | ((uint32_t)host[3] << 24);
}

static bool load_va_word(machine_t *m, uint32_t va, uint32_t *out)
{
    unsigned char buf[4] = { 0, 0, 0, 0 };

    if (!m || !m->cpu || !out)
        return false;
    if (!m->cpu->memory_rw(m->cpu, m->cpu->mem, va32_to_mips64(va), buf, 4,
        MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
        return false;

    *out = (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
    return true;
}

static bool load_va_half(machine_t *m, uint32_t va, uint16_t *out)
{
    unsigned char buf[2] = { 0, 0 };

    if (!m || !m->cpu || !out)
        return false;
    if (!m->cpu->memory_rw(m->cpu, m->cpu->mem, va32_to_mips64(va), buf, 2,
        MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
        return false;

    *out = (uint16_t)buf[0]
         | ((uint16_t)buf[1] << 8);
    return true;
}

static bool load_vrc_latch_word(uint32_t pa, uint32_t *out)
{
    return be300_vrc4173_latch_read_u32(pa, out);
}

static void invalidate_all(machine_t *m)
{
    m->cpu->invalidate_translation_caches(m->cpu, 0, INVALIDATE_ALL);
}

static bool block_matches(const uint32_t *a, const uint32_t *b, size_t words)
{
    return memcmp(a, b, words * sizeof(uint32_t)) == 0;
}

static bool block_has_nonzero(const uint32_t *a, size_t words)
{
    size_t i;

    for (i = 0; i < words; i++) {
        if (a[i] != 0)
            return true;
    }
    return false;
}

static void read_block(machine_t *m, uint32_t pa, uint32_t *out, size_t words)
{
    size_t i;

    for (i = 0; i < words; i++)
        out[i] = load_pa_word(m, pa + (uint32_t)(i * 4u));
}

static bool range_overlaps(uint64_t base_a, uint64_t size_a, uint64_t base_b,
    uint64_t size_b)
{
    uint64_t end_a;
    uint64_t end_b;

    if (size_a == 0 || size_b == 0)
        return false;

    end_a = base_a + size_a;
    end_b = base_b + size_b;
    return base_a < end_b && base_b < end_a;
}

static void set_vector_write_observer(machine_t *m, bool enable)
{
    m->wince.suppress_vector_write_observer = !enable;
}

static void write_block(machine_t *m, uint32_t pa, const uint32_t *words,
    size_t word_count)
{
    size_t i;

    if (pa < 0x00000400u) {
        set_vector_write_observer(m, false);
        m->wince.low_vector_observed_valid = false;
    }
    for (i = 0; i < word_count; i++)
        store_32bit_word(m->cpu, pa_to_kseg0(pa + (uint32_t)(i * 4u)), words[i]);
    if (pa < 0x00000400u)
        set_vector_write_observer(m, true);
}

/* ------------------------------------------------------------------ */
/*  Diagnostic / dump helpers                                           */
/* ------------------------------------------------------------------ */

static const char *format_word_or_unknown(char *buf, size_t buf_size, bool ok,
    uint32_t value)
{
    if (!ok)
        return "????????";

    snprintf(buf, buf_size, "%08X", value);
    return buf;
}

static const char *format_half_or_unknown(char *buf, size_t buf_size, bool ok,
    uint16_t value)
{
    if (!ok)
        return "????";

    snprintf(buf, buf_size, "%04X", value);
    return buf;
}

static void set_watch_observer(machine_t *m, bool enable)
{
    m->wince.suppress_watch_observer = !enable;
}

static bool watched_ram_range_name(uint64_t paddr, uint64_t len,
    const char **name)
{
    if (range_overlaps(paddr, len, 0x00002200u, 0x000000D4u)) {
        *name = "resume_ctx";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0000250Cu, 4u)) {
        *name = "cold_probe_250c";
        return true;
    }
    if (range_overlaps(paddr, len, 0x00002518u, 4u)) {
        *name = "warm_flag_2518";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0000251Cu, 4u)) {
        *name = "boot_flags_251c";
        return true;
    }
    if (range_overlaps(paddr, len, 0x00002528u, 4u)) {
        *name = "resume_ptr_2528";
        return true;
    }
    if (range_overlaps(paddr, len, 0x00002554u, 4u)) {
        *name = "ram_size_2554";
        return true;
    }
    return false;
}

static bool watched_mmio_range_name(uint64_t paddr, uint64_t len,
    const char **name)
{
    if (range_overlaps(paddr, len, 0x0A008010u, 4u)) {
        *name = "vrc4173_8010";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001120u, 4u)) {
        *name = "vrc4173_1120";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001128u, 4u)) {
        *name = "vrc4173_1128";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A00112Cu, 4u)) {
        *name = "vrc4173_112c";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001138u, 4u)) {
        *name = "vrc4173_1138";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A00113Cu, 4u)) {
        *name = "vrc4173_113c";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001B10u, 4u)) {
        *name = "vrc4173_1b10";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001B14u, 4u)) {
        *name = "vrc4173_1b14";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001B20u, 4u)) {
        *name = "vrc4173_1b20";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001B2Cu, 4u)) {
        *name = "vrc4173_1b2c";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A000004u, 4u)) {
        *name = "vrc4173_0004";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A00104Cu, 4u)) {
        *name = "vrc4173_104c";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A00A042u, 2u)) {
        *name = "buttons_a042";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A00A044u, 2u)) {
        *name = "buttons_a044";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A00A07Cu, 2u)) {
        *name = "buttons_a07c";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A00A03Cu, 4u)) {
        *name = "buttons_a03c";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A00A0E0u, 4u)) {
        *name = "nand_a0e0";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A000300u, 4u)) {
        *name = "vrc4173_0300";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A000390u, 4u)) {
        *name = "vrc4173_0390";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001054u, 2u)) {
        *name = "vrc4173_1054";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001B50u, 4u)) {
        *name = "vrc4173_1b50";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001B58u, 4u)) {
        *name = "vrc4173_1b58";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0F000100u, 4u)) {
        *name = "vr41xx_0100";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0F000104u, 2u)) {
        *name = "vr41xx_0104";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0F000144u, 2u)) {
        *name = "vr41xx_0144";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0F0000C0u, 2u)) {
        *name = "pmu_c0";
        return true;
    }
    return false;
}

static void maybe_log_boot_path_probe(machine_t *m, uint32_t raw_pc32)
{
    uint32_t pc32 = raw_pc32;
    uint64_t bit = 0;
    const char *name = NULL;
    uint32_t pa2400;
    uint32_t pa2404;
    uint32_t pa2408;
    uint32_t pa250c;
    uint32_t pa2518;
    uint32_t pa251c;
    uint32_t pa2524;
    uint32_t pa254c;
    uint32_t pa2554;
    uint32_t pa2528;
    uint32_t pa2700;
    uint32_t latch0004 = 0;
    uint32_t a03c = 0;
    uint32_t nand_a0e0 = 0;
    uint32_t vrc8010 = 0;
    uint32_t vrc104c = 0;
    uint32_t vrc1b50 = 0;
    uint32_t vrc1b58 = 0;
    uint32_t vrc08a0 = 0;
    uint32_t vrc0a00 = 0;
    uint32_t vrc1120 = 0;
    uint32_t vrc112c = 0;
    uint32_t vrc1134 = 0;
    uint32_t vrc130c = 0;
    uint32_t vrc1b10 = 0;
    uint32_t vrc1b20 = 0;
    uint32_t lat8010 = 0;
    uint32_t lat1120 = 0;
    uint32_t lat112c = 0;
    uint32_t lat1b10 = 0;
    uint32_t lat1b20 = 0;
    uint32_t vrc0300 = 0;
    uint32_t vrc0390 = 0;
    uint16_t btn_a042 = 0;
    uint16_t btn_a044 = 0;
    uint16_t btn_a07c = 0;
    uint16_t vrc1054 = 0;
    uint16_t pmu_c0 = 0;
    uint16_t vr0046 = 0;
    uint16_t vr0104 = 0;
    uint16_t vr0144 = 0;
    uint32_t vr0100 = 0;
    bool vrc8010_ok;
    bool latch0004_ok;
    bool a03c_ok;
    bool nand_a0e0_ok;
    bool vrc104c_ok;
    bool vrc1b50_ok;
    bool vrc1b58_ok;
    bool vrc08a0_ok;
    bool vrc0a00_ok;
    bool vrc1120_ok;
    bool vrc112c_ok;
    bool vrc1134_ok;
    bool vrc130c_ok;
    bool vrc1b10_ok;
    bool vrc1b20_ok;
    bool lat8010_ok;
    bool lat1120_ok;
    bool lat112c_ok;
    bool lat1b10_ok;
    bool lat1b20_ok;
    bool btn_ok;
    bool btn044_ok;
    bool btn07c_ok;
    bool vrc1054_ok;
    bool vrc0300_ok;
    bool vrc0390_ok;
    bool pmu_c0_ok;
    bool vr0100_ok;
    bool vr0046_ok;
    bool vr0104_ok;
    bool vr0144_ok;
    char b8010[9];
    char b0004[9];
    char ba03c[9];
    char ba0e0[9];
    char b104c[9];
    char b1b50[9];
    char b1b58[9];
    char b08a0[9];
    char b0a00[9];
    char b1120[9];
    char b112c[9];
    char b1134[9];
    char b130c[9];
    char b1b10[9];
    char b1b20[9];
    char blat8010[9];
    char blat1120[9];
    char blat112c[9];
    char blat1b10[9];
    char blat1b20[9];
    char ba042[5];
    char ba044[5];
    char ba07c[5];
    char b1054[5];
    char b0300[9];
    char b0390[9];
    char bpmu[5];
    char b0046[5];
    char b0100[9];
    char b0104[5];
    char b0144[5];

    if ((pc32 & 0xE0000000u) == 0x80000000u
        || (pc32 & 0xE0000000u) == 0xA0000000u)
        pc32 = (pc32 & 0x1FFFFFFFu) | 0x80000000u;

    switch (pc32) {
    case 0x80076B50u:
        bit = WINCE_PATH_PROBE_76B50;
        name = "nk_entry_76b50";
        break;
    case 0x80076BA0u:
        bit = WINCE_PATH_PROBE_76BA0;
        name = "nk_uncached_entry";
        break;
    case 0x80076C60u:
        bit = WINCE_PATH_PROBE_76C60;
        name = "main_hw_init_76c60";
        break;
    case 0x80076CBCu:
        bit = WINCE_PATH_PROBE_76CBC;
        name = "version_gate_2400";
        break;
    case 0x80076E68u:
        bit = WINCE_PATH_PROBE_76E68;
        name = "hibernate_gate_76e68";
        break;
    case 0x80076FBCu:
        bit = WINCE_PATH_PROBE_76FBC;
        name = "preinit_wait_0144_0800";
        break;
    case 0x80076FE0u:
        bit = WINCE_PATH_PROBE_76FE0;
        name = "preinit_fallback_79460";
        break;
    case 0x80077260u:
        bit = WINCE_PATH_PROBE_77260;
        name = "wait_0144_timeout_79460";
        break;
    case 0x800772F0u:
        bit = WINCE_PATH_PROBE_772F0;
        name = "cont_772f0";
        break;
    case 0x80077344u:
        bit = WINCE_PATH_PROBE_77344;
        name = "keys_to_251c";
        break;
    case 0x8007742Cu:
        bit = WINCE_PATH_PROBE_7742C;
        name = "cont_after_76e68";
        break;
    case 0x8007757Cu:
        bit = WINCE_PATH_PROBE_7757C;
        name = "cont_version_gate";
        break;
    case 0x800775B0u:
        bit = WINCE_PATH_PROBE_775B0;
        name = "ram_size_select";
        break;
    case 0x800775B8u:
        bit = WINCE_PATH_PROBE_775B8;
        name = "cold_mem_clear";
        break;
    case 0x80077664u:
        bit = WINCE_PATH_PROBE_77664;
        name = "cont_seq2408_gate";
        break;
    case 0x8007767Cu:
        bit = WINCE_PATH_PROBE_7767C;
        name = "late_dispatch_entry";
        break;
    case 0x800776F0u:
        bit = WINCE_PATH_PROBE_776F0;
        name = "late_cold_setup";
        break;
    case 0x80077738u:
        bit = WINCE_PATH_PROBE_77738;
        name = "cont_join_77738";
        break;
    case 0x80077770u:
        bit = WINCE_PATH_PROBE_77770;
        name = "late_keysplit_a07c";
        break;
    case 0x80077820u:
        bit = WINCE_PATH_PROBE_77820;
        name = "boot_dispatch";
        break;
    case 0x8007794Cu:
        bit = WINCE_PATH_PROBE_7794C;
        name = "dispatch_fallback_7794c";
        break;
    case 0x8007796Cu:
        bit = WINCE_PATH_PROBE_7796C;
        name = "cold_kernel_prep";
        break;
    case 0x80077A14u:
        bit = WINCE_PATH_PROBE_77A14;
        name = "cold_kernel_trampoline";
        break;
    case 0x80079430u:
        bit = WINCE_PATH_PROBE_79430;
        name = "resume_snapshot";
        break;
    case 0x80079460u:
        bit = WINCE_PATH_PROBE_79460;
        name = "preinit_tail";
        break;
    case 0x80079488u:
        bit = WINCE_PATH_PROBE_79488;
        name = "resume_split";
        break;
    case 0x800794C8u:
        bit = WINCE_PATH_PROBE_794C8;
        name = "cold_path";
        break;
    case 0x80079510u:
        bit = WINCE_PATH_PROBE_79510;
        name = "common_tail";
        break;
    case 0x800795D8u:
        bit = WINCE_PATH_PROBE_795D8;
        name = "warm_resume_split";
        break;
    case 0x80079634u:
        bit = WINCE_PATH_PROBE_79634;
        name = "warm_path_entry";
        break;
    case 0x80079730u:
        bit = WINCE_PATH_PROBE_79730;
        name = "warm_gpr_restore";
        break;
    case 0x80079898u:
        bit = WINCE_PATH_PROBE_79898;
        name = "low_power_helper";
        break;
    case 0x80079910u:
        bit = WINCE_PATH_PROBE_79910;
        name = "low_power_branch";
        break;
    case 0x80079990u:
        bit = WINCE_PATH_PROBE_79990;
        name = "suspend_wait";
        break;
    case 0x8007A3FCu:
        bit = WINCE_PATH_PROBE_7A3FC;
        name = "idle_dispatch";
        break;
    case 0x8007B398u:
        bit = WINCE_PATH_PROBE_7B398;
        name = "cold_kernel_entry";
        break;
    case 0x8007B57Cu:
        bit = WINCE_PATH_PROBE_7B57C;
        name = "cold_kernel_main";
        break;
    case 0x8008B21Cu:
        bit = WINCE_PATH_PROBE_8B21C;
        name = "scheduler_entry";
        break;
    case 0x8008B528u:
        bit = WINCE_PATH_PROBE_8B528;
        name = "scheduler_dispatch";
        break;
    case 0x800947C8u:
        bit = WINCE_PATH_PROBE_947C8;
        name = "kernel_init_ptoc";
        break;
    default:
        return;
    }

    if ((m->wince.boot_path_probe_mask & bit) != 0)
        return;
    m->wince.boot_path_probe_mask |= bit;

    pa2400 = load_pa_word(m, 0x2400u);
    pa2404 = load_pa_word(m, 0x2404u);
    pa2408 = load_pa_word(m, 0x2408u);
    pa250c = load_pa_word(m, 0x250Cu);
    pa2518 = load_pa_word(m, 0x2518u);
    pa251c = load_pa_word(m, 0x251Cu);
    pa2524 = load_pa_word(m, 0x2524u);
    pa2528 = load_pa_word(m, 0x2528u);
    pa254c = load_pa_word(m, 0x254Cu);
    pa2554 = load_pa_word(m, 0x2554u);
    pa2700 = load_pa_word(m, 0x2700u);

    set_watch_observer(m, false);
    vrc8010_ok = load_va_word(m, 0xAA008010u, &vrc8010);
    latch0004_ok = load_va_word(m, 0xAA000004u, &latch0004);
    a03c_ok = load_va_word(m, 0xAA00A03Cu, &a03c);
    nand_a0e0_ok = load_va_word(m, 0xAA00A0E0u, &nand_a0e0);
    vrc104c_ok = load_va_word(m, 0xAA00104Cu, &vrc104c);
    btn_ok = load_va_half(m, 0xAA00A042u, &btn_a042);
    btn044_ok = load_va_half(m, 0xAA00A044u, &btn_a044);
    btn07c_ok = load_va_half(m, 0xAA00A07Cu, &btn_a07c);
    vrc1054_ok = load_va_half(m, 0xAA001054u, &vrc1054);
    vrc1b50_ok = load_va_word(m, 0xAA001B50u, &vrc1b50);
    vrc1b58_ok = load_va_word(m, 0xAA001B58u, &vrc1b58);
    vrc08a0_ok = load_va_word(m, 0xAA0008A0u, &vrc08a0);
    vrc0a00_ok = load_va_word(m, 0xAA000A00u, &vrc0a00);
    vrc1120_ok = load_va_word(m, 0xAA001120u, &vrc1120);
    vrc112c_ok = load_va_word(m, 0xAA00112Cu, &vrc112c);
    vrc1134_ok = load_va_word(m, 0xAA001134u, &vrc1134);
    vrc130c_ok = load_va_word(m, 0xAA00130Cu, &vrc130c);
    vrc1b10_ok = load_va_word(m, 0xAA001B10u, &vrc1b10);
    vrc1b20_ok = load_va_word(m, 0xAA001B20u, &vrc1b20);
    vrc0300_ok = load_va_word(m, 0xAA000300u, &vrc0300);
    vrc0390_ok = load_va_word(m, 0xAA000390u, &vrc0390);
    lat8010_ok = load_vrc_latch_word(0x0A008010u, &lat8010);
    lat1120_ok = load_vrc_latch_word(0x0A001120u, &lat1120);
    lat112c_ok = load_vrc_latch_word(0x0A00112Cu, &lat112c);
    lat1b10_ok = load_vrc_latch_word(0x0A001B10u, &lat1b10);
    lat1b20_ok = load_vrc_latch_word(0x0A001B20u, &lat1b20);
    vr0100_ok = load_va_word(m, 0xAF000100u, &vr0100);
    vr0046_ok = load_va_half(m, 0xAF000046u, &vr0046);
    vr0104_ok = load_va_half(m, 0xAF000104u, &vr0104);
    vr0144_ok = load_va_half(m, 0xAF000144u, &vr0144);
    pmu_c0_ok = load_va_half(m, 0xAF0000C0u, &pmu_c0);
    set_watch_observer(m, true);

    fprintf(stderr,
        "[WINCE_PATH] %s PC=0x%08X Canon=0x%08X RA=0x%08X SP=0x%08X"
        " V0=0x%08X V1=0x%08X T0=0x%08X"
        " Status=0x%08X Cause=0x%08X EPC=0x%08X"
        " PA2400=0x%08X PA2404=0x%08X PA2408=0x%08X PA250C=0x%08X"
        " PA2518=0x%08X PA251C=0x%08X PA2524=0x%08X PA2528=0x%08X"
        " PA254C=0x%08X PA2554=0x%08X"
        " PA2700=0x%08X"
        " VRC8010=0x%s LATCH0004=0x%s BTN_A03C=0x%s NAND_A0E0=0x%s VRC104C=0x%s"
        " BTN_A042=0x%s BTN_A044=0x%s BTN_A07C=0x%s VRC1054=0x%s"
        " VRC08A0=0x%s VRC0A00=0x%s VRC1120=0x%s VRC112C=0x%s"
        " VRC1134=0x%s VRC130C=0x%s VRC1B10=0x%s VRC1B20=0x%s"
        " LAT8010=0x%s LAT1120=0x%s LAT112C=0x%s LAT1B10=0x%s LAT1B20=0x%s"
        " VRC0300=0x%s VRC0390=0x%s VRC1B50=0x%s VRC1B58=0x%s"
        " VR0046=0x%s VR0100=0x%s VR0104=0x%s VR0144=0x%s PMU_C0=0x%s\n",
        name,
        raw_pc32,
        pc32,
        (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
        (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
        (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_V0],
        (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_V1],
        (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_T0],
        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS],
        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE],
        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC],
        pa2400,
        pa2404,
        pa2408,
        pa250c,
        pa2518,
        pa251c,
        pa2524,
        pa2528,
        pa254c,
        pa2554,
        pa2700,
        format_word_or_unknown(b8010, sizeof(b8010), vrc8010_ok, vrc8010),
        format_word_or_unknown(b0004, sizeof(b0004), latch0004_ok, latch0004),
        format_word_or_unknown(ba03c, sizeof(ba03c), a03c_ok, a03c),
        format_word_or_unknown(ba0e0, sizeof(ba0e0), nand_a0e0_ok, nand_a0e0),
        format_word_or_unknown(b104c, sizeof(b104c), vrc104c_ok, vrc104c),
        format_half_or_unknown(ba042, sizeof(ba042), btn_ok, btn_a042),
        format_half_or_unknown(ba044, sizeof(ba044), btn044_ok, btn_a044),
        format_half_or_unknown(ba07c, sizeof(ba07c), btn07c_ok, btn_a07c),
        format_half_or_unknown(b1054, sizeof(b1054), vrc1054_ok, vrc1054),
        format_word_or_unknown(b08a0, sizeof(b08a0), vrc08a0_ok, vrc08a0),
        format_word_or_unknown(b0a00, sizeof(b0a00), vrc0a00_ok, vrc0a00),
        format_word_or_unknown(b1120, sizeof(b1120), vrc1120_ok, vrc1120),
        format_word_or_unknown(b112c, sizeof(b112c), vrc112c_ok, vrc112c),
        format_word_or_unknown(b1134, sizeof(b1134), vrc1134_ok, vrc1134),
        format_word_or_unknown(b130c, sizeof(b130c), vrc130c_ok, vrc130c),
        format_word_or_unknown(b1b10, sizeof(b1b10), vrc1b10_ok, vrc1b10),
        format_word_or_unknown(b1b20, sizeof(b1b20), vrc1b20_ok, vrc1b20),
        format_word_or_unknown(blat8010, sizeof(blat8010), lat8010_ok, lat8010),
        format_word_or_unknown(blat1120, sizeof(blat1120), lat1120_ok, lat1120),
        format_word_or_unknown(blat112c, sizeof(blat112c), lat112c_ok, lat112c),
        format_word_or_unknown(blat1b10, sizeof(blat1b10), lat1b10_ok, lat1b10),
        format_word_or_unknown(blat1b20, sizeof(blat1b20), lat1b20_ok, lat1b20),
        format_word_or_unknown(b0300, sizeof(b0300), vrc0300_ok, vrc0300),
        format_word_or_unknown(b0390, sizeof(b0390), vrc0390_ok, vrc0390),
        format_word_or_unknown(b1b50, sizeof(b1b50), vrc1b50_ok, vrc1b50),
        format_word_or_unknown(b1b58, sizeof(b1b58), vrc1b58_ok, vrc1b58),
        format_half_or_unknown(b0046, sizeof(b0046), vr0046_ok, vr0046),
        format_word_or_unknown(b0100, sizeof(b0100), vr0100_ok, vr0100),
        format_half_or_unknown(b0104, sizeof(b0104), vr0104_ok, vr0104),
        format_half_or_unknown(b0144, sizeof(b0144), vr0144_ok, vr0144),
        format_half_or_unknown(bpmu, sizeof(bpmu), pmu_c0_ok, pmu_c0));
}

static void dump_pa_words(machine_t *m, const char *label, uint32_t pa,
    size_t word_count)
{
    size_t i;

    fprintf(stderr, "[WINCE_CKPT] %-14s PA=0x%08X", label, pa);
    for (i = 0; i < word_count; i++)
        fprintf(stderr, " %08X", load_pa_word(m, pa + (uint32_t)(i * 4u)));
    fprintf(stderr, "\n");
}

static void dump_ctx_window(machine_t *m, uint32_t pa, uint32_t size)
{
    uint32_t off;

    for (off = 0; off < size; off += 16u) {
        fprintf(stderr,
            "[WINCE_CKPT] ctx+0x%03X: %08X %08X %08X %08X\n",
            off,
            load_pa_word(m, pa + off + 0u),
            load_pa_word(m, pa + off + 4u),
            load_pa_word(m, pa + off + 8u),
            load_pa_word(m, pa + off + 12u));
    }
}

static void dump_va_window(machine_t *m, const char *label, uint32_t va,
    uint32_t size)
{
    uint32_t off;

    for (off = 0; off < size; off += 16u) {
        uint32_t w0 = 0;
        uint32_t w1 = 0;
        uint32_t w2 = 0;
        uint32_t w3 = 0;
        bool ok0 = load_va_word(m, va + off + 0u, &w0);
        bool ok1 = load_va_word(m, va + off + 4u, &w1);
        bool ok2 = load_va_word(m, va + off + 8u, &w2);
        bool ok3 = load_va_word(m, va + off + 12u, &w3);
        char b0[9];
        char b1[9];
        char b2[9];
        char b3[9];

        fprintf(stderr,
            "[WINCE_HANDLER] %s+0x%03X: %s %s %s %s\n",
            label,
            off,
            format_word_or_unknown(b0, sizeof(b0), ok0, w0),
            format_word_or_unknown(b1, sizeof(b1), ok1, w1),
            format_word_or_unknown(b2, sizeof(b2), ok2, w2),
            format_word_or_unknown(b3, sizeof(b3), ok3, w3));
    }
}

static void dump_code_window(machine_t *m, uint32_t pc, size_t before_words,
    size_t after_words)
{
    int rel;
    int start = -(int)before_words;
    int stop = (int)after_words;

    for (rel = start; rel <= stop; rel++) {
        uint32_t va = pc + (uint32_t)(rel * 4);
        uint32_t word = 0;
        bool ok = load_va_word(m, va, &word);
        char buf[9];

        fprintf(stderr,
            "[WINCE_CODE] %c rel=%+03d VA=0x%08X word=%s\n",
            rel == 0 ? '*' : ' ',
            rel * 4,
            va,
            format_word_or_unknown(buf, sizeof(buf), ok, word));
    }
}

static void dump_gpr_window(machine_t *m)
{
    size_t i;

    for (i = 0; i < 32; i += 4u) {
        fprintf(stderr,
            "[WINCE_REGS] %-3s=0x%08X %-3s=0x%08X %-3s=0x%08X %-3s=0x%08X\n",
            wince_gpr_names[i + 0u], (uint32_t)m->cpu->cd.mips.gpr[i + 0u],
            wince_gpr_names[i + 1u], (uint32_t)m->cpu->cd.mips.gpr[i + 1u],
            wince_gpr_names[i + 2u], (uint32_t)m->cpu->cd.mips.gpr[i + 2u],
            wince_gpr_names[i + 3u], (uint32_t)m->cpu->cd.mips.gpr[i + 3u]);
    }
}

/* ------------------------------------------------------------------ */
/*  Checkpoint logging                                                  */
/* ------------------------------------------------------------------ */

static bool log_checkpoint_header(machine_t *m, const char *tag,
    const char *detail)
{
    uint32_t status;
    uint32_t cause;
    uint32_t epc;
    uint32_t badvaddr;
    uint32_t wired;
    uint32_t entryhi;
    uint32_t pagemask;

    if (!m->wince.active)
        return false;

    status = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS];
    cause = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE];
    epc = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC];
    badvaddr = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR];
    wired = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_WIRED];
    entryhi = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_ENTRYHI];
    pagemask = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_PAGEMASK];

    fprintf(stderr,
        "[WINCE_CKPT] %s detail=%s PC=0x%08" PRIx64
        " Status=0x%08X Cause=0x%08X EPC=0x%08X BadVA=0x%08X"
        " Wired=0x%08X EntryHi=0x%08X PageMask=0x%08X"
        " owner=%d ready=%d\n",
        tag,
        detail ? detail : "-",
        (uint64_t)m->cpu->pc,
        status,
        cause,
        epc,
        badvaddr,
        wired,
        entryhi,
        pagemask,
        (int)m->wince.vector_owner,
        m->wince.vectors_ready ? 1 : 0);

    return true;
}

static void maybe_log_checkpoint(machine_t *m, const char *tag,
    const char *detail)
{
    if (!log_checkpoint_header(m, tag, detail))
        return;

    dump_pa_words(m, "vec_low_0000", 0x00000000u, WINCE_VECTOR_WORDS);
    dump_pa_words(m, "vec_low_0080", 0x00000080u, WINCE_VECTOR_WORDS);
    dump_pa_words(m, "vec_low_0100", 0x00000100u, WINCE_VECTOR_WORDS);
    dump_pa_words(m, "vec_low_0180", 0x00000180u, WINCE_VECTOR_WORDS);
    dump_pa_words(m, "vec_bev_0000", 0x1FC00000u, WINCE_VECTOR_WORDS);
    dump_pa_words(m, "vec_bev_0200", 0x1FC00200u, WINCE_VECTOR_WORDS);
    dump_pa_words(m, "vec_bev_0380", 0x1FC00380u, WINCE_VECTOR_WORDS);
    dump_ctx_window(m, 0x00002000u, 0x300u);
}

/* ------------------------------------------------------------------ */
/*  Low-vector state machine                                            */
/* ------------------------------------------------------------------ */

static void scan_low_vectors(machine_t *m)
{
    uint32_t tlb[WINCE_VECTOR_WORDS];
    uint32_t general[WINCE_VECTOR_WORDS];
    bool tlb_matches;
    bool general_matches;
    bool tlb_real;
    bool general_real;
    bool ready;

    if (!m->wince.active)
        return;

    read_block(m, 0x00000000u, tlb, WINCE_VECTOR_WORDS);
    read_block(m, 0x00000180u, general, WINCE_VECTOR_WORDS);

    tlb_matches = block_matches(tlb, m->wince.synthetic_low_tlb,
        WINCE_VECTOR_WORDS);
    general_matches = block_matches(general, m->wince.synthetic_low_general,
        WINCE_VECTOR_WORDS);
    tlb_real = block_has_nonzero(tlb, WINCE_VECTOR_WORDS) && !tlb_matches;
    general_real = block_has_nonzero(general, WINCE_VECTOR_WORDS)
        && !general_matches;
    ready = tlb_real || general_real;

    if (m->wince.vector_owner == WINCE_VECTOR_SYNTHETIC) {
        if (!tlb_matches || !general_matches) {
            m->wince.vector_owner = WINCE_VECTOR_GUEST;
            m->wince.vectors_ready = ready;
            m->wince.low_vector_observed_valid = false;
            invalidate_all(m);
            if (!m->wince.low_vector_guest_write_logged) {
                fprintf(stderr,
                    "[WINCE_CKPT] guest_low_vector_takeover"
                    " tlb_match=%d general_match=%d ready=%d\n",
                    tlb_matches ? 1 : 0,
                    general_matches ? 1 : 0,
                    ready ? 1 : 0);
                m->wince.low_vector_guest_write_logged = true;
            }
            if (ready)
                maybe_log_checkpoint(m, "vector_owner", "guest-low-vectors");
        }
        return;
    }

    if (m->wince.vector_owner == WINCE_VECTOR_GUEST
        && ready && !m->wince.vectors_ready) {
        m->wince.vectors_ready = true;
        maybe_log_checkpoint(m, "vector_owner", "guest-low-vectors");
    }
}

/* ------------------------------------------------------------------ */
/*  Exception detection                                                 */
/* ------------------------------------------------------------------ */

static void maybe_note_first_exception(machine_t *m)
{
    uint32_t cause;
    uint32_t badvaddr;
    uint32_t pc_norm;

    if (!m->wince.active || !m->wince.cold_boot_redirected
        || m->wince.first_exception_logged)
        return;

    cause = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE];
    badvaddr = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR];
    pc_norm = (uint32_t)m->cpu->pc & 0x1FFFFFFFu;

    if (badvaddr >= 0x00000400u && pc_norm >= 0x00000400u)
        return;

    m->wince.first_exception_logged = true;
    maybe_log_checkpoint(m, "first_exception", "post-redirect");
    (void)cause;
}

/* ------------------------------------------------------------------ */
/*  Runtime low-vector drift tracking                                   */
/* ------------------------------------------------------------------ */

static void maybe_track_low_vector_runtime_changes(machine_t *m)
{
    uint32_t current_tlb[WINCE_VECTOR_WORDS];
    uint32_t current_general[WINCE_VECTOR_WORDS];
    bool tlb_changed;
    bool general_changed;

    if (!m || !m->wince.active || !m->wince.cold_boot_redirected
        || !m->wince.vectors_ready) {
        return;
    }

    read_block(m, 0x00000000u, current_tlb, WINCE_VECTOR_WORDS);
    read_block(m, 0x00000180u, current_general, WINCE_VECTOR_WORDS);

    if (!m->wince.low_vector_observed_valid) {
        read_block(m, 0x00000000u, m->wince.observed_low_tlb,
            WINCE_VECTOR_WORDS);
        read_block(m, 0x00000180u, m->wince.observed_low_general,
            WINCE_VECTOR_WORDS);
        m->wince.low_vector_observed_valid = true;
        return;
    }

    tlb_changed = !block_matches(current_tlb, m->wince.observed_low_tlb,
        WINCE_VECTOR_WORDS);
    general_changed = !block_matches(current_general,
        m->wince.observed_low_general, WINCE_VECTOR_WORDS);
    if (!tlb_changed && !general_changed)
        return;

    invalidate_all(m);
    if (m->wince.vector_owner != WINCE_VECTOR_GUEST)
        m->wince.vector_owner = WINCE_VECTOR_GUEST;

    if (!m->wince.low_vector_runtime_drift_logged) {
        uint32_t sp = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP];
        uint32_t pc = (uint32_t)m->cpu->pc;

        m->wince.low_vector_runtime_drift_logged = true;
        fprintf(stderr,
            "[WINCE_VECTOR_DRIFT] pc=0x%08X ra=0x%08X sp=0x%08X"
            " epc=0x%08X badva=0x%08X status=0x%08X cause=0x%08X"
            " entryhi=0x%08X tlb_changed=%d general_changed=%d\n",
            pc,
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
            sp,
            (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC],
            (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR],
            (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS],
            (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE],
            (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_ENTRYHI],
            tlb_changed ? 1 : 0,
            general_changed ? 1 : 0);
        fprintf(stderr,
            "[WINCE_VECTOR_DRIFT] old_low_0000=%08X/%08X/%08X/%08X"
            " new_low_0000=%08X/%08X/%08X/%08X\n",
            m->wince.observed_low_tlb[0],
            m->wince.observed_low_tlb[1],
            m->wince.observed_low_tlb[2],
            m->wince.observed_low_tlb[3],
            current_tlb[0],
            current_tlb[1],
            current_tlb[2],
            current_tlb[3]);
        fprintf(stderr,
            "[WINCE_VECTOR_DRIFT] old_low_0180=%08X/%08X/%08X/%08X"
            " new_low_0180=%08X/%08X/%08X/%08X\n",
            m->wince.observed_low_general[0],
            m->wince.observed_low_general[1],
            m->wince.observed_low_general[2],
            m->wince.observed_low_general[3],
            current_general[0],
            current_general[1],
            current_general[2],
            current_general[3]);
        dump_gpr_window(m);
        dump_code_window(m, pc, 4u, 12u);
        if (sp != 0)
            dump_va_window(m, "low_vector_drift_stack", sp - 0x40u, 0x80u);
        maybe_log_checkpoint(m, "low_vector_drift", "runtime-change");
    }

    memcpy(m->wince.observed_low_tlb, current_tlb,
        sizeof(m->wince.observed_low_tlb));
    memcpy(m->wince.observed_low_general, current_general,
        sizeof(m->wince.observed_low_general));
}

/* ------------------------------------------------------------------ */
/*  Cold-boot scheduler probes                                          */
/* ------------------------------------------------------------------ */

static bool cold_boot_scheduler_probe_pc_match(uint32_t value)
{
    return (value >= UINT32_C(0x80079900) && value <= UINT32_C(0x800799C0))
        || (value >= UINT32_C(0x80089760) && value <= UINT32_C(0x800897C0))
        || (value >= UINT32_C(0x80089870) && value <= UINT32_C(0x800898F8))
        || (value >= UINT32_C(0x8008B4F0) && value <= UINT32_C(0x8008B6C0))
        || (value >= UINT32_C(0x8007A3F0) && value <= UINT32_C(0x8007A6B0));
}

static bool cold_boot_scheduler_probe_epc_match(uint32_t value)
{
    return cold_boot_scheduler_probe_pc_match(value)
        || value == UINT32_C(0x80089A50);
}

static void maybe_log_cold_boot_scheduler_probe(machine_t *m,
    uint32_t sampled_pc)
{
    uint32_t pc;
    uint32_t epc;
    uint32_t sp;
    uint32_t s0;
    uint32_t s1;
    uint32_t status;
    uint32_t cause;
    uint32_t badva;

    if (!m || !m->wince.active
        || !m->wince.cold_boot_redirected) {
        return;
    }
    if ((m->wince.cold_boot_pc_probes_logged
        & WINCE_COLD_LATE_PROBE_LOGGED) != 0) {
        return;
    }

    pc = sampled_pc;
    epc = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC];
    badva = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR];
    if (!cold_boot_scheduler_probe_pc_match(pc)
        && !cold_boot_scheduler_probe_epc_match(epc)
        && badva != UINT32_C(0x0201FE2C)) {
        return;
    }

    m->wince.cold_boot_pc_probes_logged |= WINCE_COLD_LATE_PROBE_LOGGED;
    sp = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP];
    s0 = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_S0];
    s1 = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_S1];
    status = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS];
    cause = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE];

    fprintf(stderr,
        "[WINCE_COLD_LATE] pc=0x%08X epc=0x%08X sp=0x%08X"
        " s0=0x%08X s1=0x%08X status=0x%08X cause=0x%08X"
        " badva=0x%08X\n",
        pc,
        epc,
        sp,
        s0,
        s1,
        status,
        cause,
        badva);

    dump_gpr_window(m);
    dump_va_window(m, "cold_late_pc", pc & ~UINT32_C(0x1F), 0x60u);
    if (epc != 0)
        dump_va_window(m, "cold_late_epc", epc & ~UINT32_C(0x1F), 0x60u);
    dump_va_window(m, "cold_late_sched_961c", UINT32_C(0x8067961C), 0x20u);
    dump_va_window(m, "cold_late_sched_9654", UINT32_C(0x80679654), 0x20u);
    dump_va_window(m, "cold_late_sched_96d8", UINT32_C(0x806796D8), 0x20u);
    dump_va_window(m, "cold_late_sched_96f8", UINT32_C(0x806796F8), 0x20u);
    dump_va_window(m, "cold_late_sched_9780", UINT32_C(0x80679780), 0x20u);
    dump_va_window(m, "cold_late_obj_table", UINT32_C(0x8066BFC0), 0x40u);
    if (s0 != 0)
        dump_va_window(m, "cold_late_s0", s0 & ~UINT32_C(0x1F), 0x120u);
    if (s1 != 0)
        dump_va_window(m, "cold_late_s1", s1 & ~UINT32_C(0x1F), 0x60u);

    /* Scheduler globals from FUN_8008B528 / FUN_8007A3FC decompilation */
    dump_va_window(m, "sched_runlist",  UINT32_C(0x80669800), 0x100u);
    dump_va_window(m, "sched_kerndata", UINT32_C(0x80660000), 0x80u);
    dump_va_window(m, "sched_timers",   UINT32_C(0x8066BF80), 0x80u);
    dump_va_window(m, "sched_misc",     UINT32_C(0x80669500), 0x80u);

    /* PC diversity analysis: scan pc_ring[] for distinct PCs */
    if (m->wince.pc_ring_active && m->wince.pc_ring_idx > 0) {
        uint32_t total = m->wince.pc_ring_idx;
        uint32_t count = total < WINCE_PC_RING_SIZE
            ? total : WINCE_PC_RING_SIZE;
        uint32_t distinct = 0;
        uint32_t usermode = 0;
        uint32_t last_non_idle = 0;
        uint32_t seen[64];
        uint32_t nseen = 0;

        for (uint32_t i = 0; i < count; i++) {
            uint32_t idx = (total - count + i) % WINCE_PC_RING_SIZE;
            uint32_t ring_pc = m->wince.pc_ring[idx];
            bool found = false;

            if (ring_pc < UINT32_C(0x80000000))
                usermode++;
            if (!((ring_pc >= UINT32_C(0x80079000)
                   && ring_pc < UINT32_C(0x800799FF))
                  || (ring_pc >= UINT32_C(0x8007A300)
                      && ring_pc < UINT32_C(0x8007A500))
                  || (ring_pc >= UINT32_C(0x8008B400)
                      && ring_pc < UINT32_C(0x8008B600))))
                last_non_idle = ring_pc;
            for (uint32_t j = 0; j < nseen; j++) {
                if (seen[j] == ring_pc) { found = true; break; }
            }
            if (!found && nseen < 64)
                seen[nseen++] = ring_pc;
        }
        distinct = nseen;
        fprintf(stderr,
            "[WINCE_COLD_LATE] pc_diversity: samples=%u distinct=%u"
            " usermode=%u last_non_idle=0x%08X\n",
            count, distinct, usermode, last_non_idle);
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

void wince_boot_attach_machine(machine_t *m)
{
    g_active_wince_machine = m;
}

void wince_boot_detach_machine(machine_t *m)
{
    if (g_active_wince_machine == m)
        g_active_wince_machine = NULL;
}

void wince_boot_init(machine_t *m)
{
    memset(&m->wince, 0, sizeof(m->wince));
    m->wince.active = (m->cfg.nand_path != NULL);
    m->wince.vector_owner = WINCE_VECTOR_NONE;
}

void wince_boot_note_spl_handoff(machine_t *m)
{
    if (!m->wince.active || m->wince.spl_handoff_logged)
        return;
    m->wince.spl_handoff_logged = true;
    maybe_log_checkpoint(m, "spl_handoff", "nk-entry");
}

void wince_boot_note_cold_boot_redirect(machine_t *m, const char *detail)
{
    if (!m->wince.active)
        return;
    m->wince.cold_boot_redirected = true;
    maybe_log_checkpoint(m, "cold_boot_redirect", detail);
}

void wince_boot_note_first_exception(machine_t *m, const char *detail)
{
    if (!m->wince.active || m->wince.first_exception_logged)
        return;
    m->wince.first_exception_logged = true;
    maybe_log_checkpoint(m, "first_exception", detail);
}

void wince_boot_note_fatal_stop(machine_t *m, const char *reason)
{
    if (!m->wince.active || m->wince.fatal_exit_logged)
        return;
    m->wince.fatal_exit_logged = true;
    maybe_log_checkpoint(m, "fatal_stop", reason);
}

void wince_boot_install_synthetic_low_vectors(machine_t *m,
    const uint32_t *handler, size_t word_count, const char *reason)
{
    size_t count = word_count;

    if (!m->wince.active)
        return;
    if (count > WINCE_VECTOR_WORDS)
        count = WINCE_VECTOR_WORDS;

    memset(m->wince.synthetic_low_tlb, 0, sizeof(m->wince.synthetic_low_tlb));
    memset(m->wince.synthetic_low_general, 0,
        sizeof(m->wince.synthetic_low_general));
    memcpy(m->wince.synthetic_low_tlb, handler, count * sizeof(uint32_t));
    memcpy(m->wince.synthetic_low_general, handler, count * sizeof(uint32_t));

    if (m->wince.vector_owner == WINCE_VECTOR_NONE
        || m->wince.vector_owner == WINCE_VECTOR_SYNTHETIC) {
        write_block(m, 0x00000000u, m->wince.synthetic_low_tlb, count);
        write_block(m, 0x00000180u, m->wince.synthetic_low_general, count);
        invalidate_all(m);
        m->wince.vector_owner = WINCE_VECTOR_SYNTHETIC;
        m->wince.vectors_ready = false;
    }

    if (reason)
        fprintf(stderr, "[WINCE_CKPT] synthetic_low_vectors reason=%s\n",
            reason);
}

/*
 *  ROM DMA polling function intercept.
 *
 *  The ROM's MIPS16 function at 0x9FC013F0 polls the DMA controller for
 *  completion.  On real hardware, the VRC4173 DMA autonomously transfers
 *  NAND page data to a RAM buffer.  Our emulator only supports CPU-mediated
 *  FIFO reads, so we simulate the autonomous transfer by copying the current
 *  NAND page directly to the buffer address passed in $a0.
 *
 *  This fires on every call to 0x13F0, including the first call (where the
 *  ROM also reads via FIFO).  The FIFO read will overwrite our copy with
 *  identical data, so there's no conflict.
 */
#define ROM_DMA_POLL_PC  0x9FC013F0u
#define DMA_AUTOCOPY_MAX 20
static int dma_autocopy_count = 0;

static void maybe_dma_autocopy(machine_t *m, struct cpu *cpu)
{
    uint32_t pc32 = (uint32_t)cpu->pc;
    if (pc32 != ROM_DMA_POLL_PC)
        return;

    /* The DMA autocopy approach was investigated and found to be wrong.
     * The delay slot of the JAL to 0x13F0 overwrites $a0 with $s0,
     * so all three calls use buffer 0x80010060 (not 0x80008084).
     * The callback table at 0x80008080 is populated by the processing
     * code at 0x1160-0x11A6, not by DMA transfer.
     * Keeping the function entry detection for future diagnostics. */
    return; /* autocopy disabled — wrong approach */

    if (!m->nand.dma_active)
        return;
    uint32_t buf_va = (uint32_t)cpu->cd.mips.gpr[4];
    uint32_t buf_pa = buf_va & 0x1FFFFFFFu;
    if (buf_pa < 0x8000u || buf_pa >= 0x9000u)
        return;
    if (dma_autocopy_count >= DMA_AUTOCOPY_MAX)
        return;
    dma_autocopy_count++;

    uint32_t nand_addr = m->nand.dma_nand_addr;
    uint32_t nbytes = m->nand.dma_total_bytes;
    if (nbytes > 2048) nbytes = 2048;  /* sanity cap */

    fprintf(stderr,
        "[DMA_AUTOCOPY] PC=0x%08X buf=0x%08X nand=0x%06X len=%u"
        " cursor=%u/%u #%d\n",
        pc32, buf_va, nand_addr, nbytes,
        m->nand.dma_cursor, m->nand.dma_total_bytes,
        dma_autocopy_count);

    /* Copy NAND data to the CPU-visible buffer.
     * This simulates autonomous DMA transfer — the hardware copies
     * NAND page data to RAM without CPU intervention.
     * We do NOT change dma_cursor so that FIFO-based reads (used by
     * the first DMA call) continue to work normally. */
    for (uint32_t i = 0; i < nbytes; i++) {
        uint8_t byte;
        if (m->nand.image && (nand_addr + i) < m->nand.image_size)
            byte = m->nand.image[nand_addr + i];
        else
            byte = 0xFFu;
        cpu->memory_rw(cpu, cpu->mem,
            (uint64_t)(buf_va + i), &byte, 1,
            MEM_WRITE, CACHE_DATA);
    }
}

/*
 *  Called from the MIPS16 slow interpreter on every ROM instruction.
 *  Checks if we're at the DMA polling function entry and performs
 *  the autonomous DMA copy if needed.
 */
void wince_boot_check_dma_autocopy(struct cpu *cpu)
{
    machine_t *m = g_active_wince_machine;
    if (!m || !m->wince.active)
        return;
    maybe_dma_autocopy(m, cpu);

    /* Callback table frame copy removed — it was a workaround that
     * prevented the immediate epilogue crash but provided wrong values
     * (saved registers instead of callback function pointers), causing
     * the boot dispatcher to jump to invalid addresses.  The real fix
     * needs the ROM processing code at 0x1160-0x11A6 to correctly
     * populate the callback table at PA 0x8080. */
}

void wince_boot_on_vr41xx_tick(struct machine *gxm, struct cpu *cpu)
{
    machine_t *m = wince_boot_from_gx(gxm);

    if (!m || !m->wince.active)
        return;

    /* Detect NK.exe code execution during machine_run (the per-batch
     * check in be300_run_batch misses NK.exe entry because it can enter
     * and crash within a single batch). */
    if (!m->wince.cold_boot_copy_done && m->nand_data) {
        uint32_t pc32 = (uint32_t)cpu->pc;
        uint32_t pa = pc32 & 0x1FFFFFFFu;
        if (pa >= 0x60000u && pa < 0x100000u) {
            m->wince.cold_boot_copy_done = true;
            m->wince.cold_boot_redirected = true;
            m->nand.wince_mode = true;
            fprintf(stderr,
                "[WINCE_CKPT] nk_entry_detected PC=0x%08X PA=0x%08X"
                " SP=0x%08X RA=0x%08X Status=0x%08X\n",
                pc32, pa,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
                (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_STATUS]);
            /* Dump PA 0x2700 boot signature (FUN_80079E48 checks this) */
            fprintf(stderr, "[WINCE_CKPT] boot_sig PA=0x2700:");
            {
                size_t i;
                for (i = 0; i < 16; i++)
                    fprintf(stderr, " %02X",
                        load_pa_word(m, 0x2700u + (uint32_t)(i * 4u))
                            & 0xFFu);
            }
            fprintf(stderr, "\n");
            dump_pa_words(m, "resume_ctx", 0x00002200u, 8);
            /* Activate PC ring for diversity analysis */
            if (!m->wince.pc_ring_active)
                wince_boot_pc_ring_activate(m);
        }
    }

    /* PC ring buffer — sample every tick once PA 0x24FC is detected */
    if (m->wince.pc_ring_active) {
        uint32_t idx = m->wince.pc_ring_idx % WINCE_PC_RING_SIZE;
        m->wince.pc_ring[idx] = (uint32_t)cpu->pc;
        m->wince.pc_ring_sp[idx] =
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
        m->wince.pc_ring_status[idx] =
            (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_STATUS];
        m->wince.pc_ring_idx++;
    }

    maybe_log_boot_path_probe(m, (uint32_t)cpu->pc);
    scan_low_vectors(m);
    maybe_track_low_vector_runtime_changes(m);
    maybe_note_first_exception(m);
    maybe_log_cold_boot_scheduler_probe(m, (uint32_t)cpu->pc);

    /* PIU pen-state change detection (generates VRIP PIU interrupt) */
    {
        extern void be300_touch_tick(machine_t *m);
        be300_touch_tick(m);
    }
}

void wince_boot_note_timer_config(struct machine *gxm, struct cpu *cpu,
    uint64_t relative_addr, uint64_t value)
{
    machine_t *m = wince_boot_from_gx(gxm);
    (void)cpu;

    if (!m || !m->wince.active || m->wince.timer_config_logged)
        return;

    m->wince.timer_config_logged = true;
    maybe_log_checkpoint(m, "timer_config", "vr41xx-rtcl1");
    {
        fprintf(stderr,
            "[WINCE_CKPT] timer_register off=0x%03" PRIx64
            " value=0x%04" PRIx64 "\n",
            relative_addr, value);
    }
}

bool wince_boot_timer_irq_allowed(struct machine *gxm, struct cpu *cpu)
{
    machine_t *m = wince_boot_from_gx(gxm);
    (void)cpu;

    if (!m || !m->wince.active)
        return true;
    if (!m->wince.cold_boot_redirected)
        return true;
    if (!m->wince.vectors_ready) {
        if (!m->wince.timer_gate_logged) {
            fprintf(stderr,
                "[WINCE_CKPT] timer_irq_gate active waiting_for_vectors\n");
            m->wince.timer_gate_logged = true;
        }
        return false;
    }

    if (!m->wince.timer_release_logged) {
        fprintf(stderr,
            "[WINCE_CKPT] timer_irq_gate released owner=%d\n",
            (int)m->wince.vector_owner);
        m->wince.timer_release_logged = true;
    }
    return true;
}

void wince_boot_note_low_vector_write(struct cpu *cpu, uint64_t paddr,
    size_t len)
{
    machine_t *m;

    if (!cpu || !cpu->machine)
        return;

    m = wince_boot_from_gx(cpu->machine);
    if (!m || !m->wince.active || m->wince.suppress_vector_write_observer)
        return;
    if (!range_overlaps(paddr, (uint64_t)len, 0x00000000u, 0x00000400u))
        return;

    invalidate_all(m);
    m->wince.low_vector_observed_valid = false;
    if (m->wince.vector_owner != WINCE_VECTOR_GUEST) {
        m->wince.vector_owner = WINCE_VECTOR_GUEST;
        m->wince.vectors_ready = false;
        if (!m->wince.low_vector_guest_write_logged) {
            fprintf(stderr,
                "[WINCE_CKPT] guest_low_vector_write"
                " paddr=0x%08" PRIx64 " len=%zu\n",
                paddr, len);
            m->wince.low_vector_guest_write_logged = true;
        }
    }
}

void wince_boot_note_fb_oob(struct cpu *cpu, uint64_t paddr, size_t len)
{
    machine_t *m;

    if (!cpu || !cpu->machine)
        return;

    m = wince_boot_from_gx(cpu->machine);
    if (!m || !m->wince.active || m->wince.first_fb_oob_logged)
        return;
    if (paddr < 0x0A228000ULL || paddr >= 0x0A240000ULL)
        return;

    m->wince.first_fb_oob_logged = true;
    fprintf(stderr,
        "[WINCE_FB] first out-of-range framebuffer write"
        " paddr=0x%08" PRIx64 " len=%zu pc=0x%08" PRIx64 "\n",
        paddr, len, (uint64_t)cpu->pc);
}

void wince_boot_pc_ring_activate(machine_t *m)
{
    if (!m || !m->wince.active || m->wince.pc_ring_active)
        return;
    m->wince.pc_ring_active = true;
    m->wince.pc_ring_idx = 0;
    fprintf(stderr, "[WINCE_CKPT] pc_ring activated\n");
}

void wince_boot_pc_ring_dump(machine_t *m)
{
    uint32_t total;
    uint32_t start;
    uint32_t count;
    uint32_t i;

    if (!m || !m->wince.active || !m->wince.pc_ring_active)
        return;

    total = m->wince.pc_ring_idx;
    if (total == 0) {
        fprintf(stderr, "[PC_RING] empty\n");
        return;
    }

    count = total < WINCE_PC_RING_SIZE ? total : WINCE_PC_RING_SIZE;
    start = total < WINCE_PC_RING_SIZE ? 0 : total % WINCE_PC_RING_SIZE;

    fprintf(stderr, "[PC_RING] %u samples (last %u shown):\n", total, count);
    for (i = 0; i < count; i++) {
        uint32_t idx = (start + i) % WINCE_PC_RING_SIZE;
        uint32_t pc = m->wince.pc_ring[idx];
        uint32_t pa = pc & 0x1FFFFFFFu;
        const char *region = "???";
        if (pa >= 0x1FC00000u)
            region = "ROM";
        else if (pa >= 0xF00000u && pa < 0x1000000u)
            region = "SPL";
        else if (pa >= 0x60000u && pa < 0x100000u)
            region = "NK";
        else if (pa < 0x10000u)
            region = "LOW";

        fprintf(stderr, "[PC_RING] [%3u] PC=0x%08X (%s)"
            " SP=0x%08X Status=0x%08X\n",
            i, pc, region,
            m->wince.pc_ring_sp[idx],
            m->wince.pc_ring_status[idx]);
    }
}

void wince_boot_crash_pc_dump(struct machine *gxm)
{
    machine_t *m = wince_boot_from_gx(gxm);
    if (m)
        wince_boot_pc_ring_dump(m);
}

void wince_boot_note_pc(struct cpu *cpu, uint32_t pc32)
{
    machine_t *m;

    if (!cpu)
        return;

    m = wince_boot_from_gx(cpu->machine);
    if (!m || !m->wince.active)
        return;

    maybe_log_boot_path_probe(m, pc32);
}

bool wince_boot_arm_step_trace(struct cpu *cpu, uint32_t pc32)
{
    machine_t *m;
    uint32_t canon_pc = pc32;

    if (!cpu)
        return false;

    m = wince_boot_from_gx(cpu->machine);
    if (!m || !m->wince.active || !m->wince.cold_boot_copy_done)
        return false;

    if ((canon_pc & 0xE0000000u) == 0x80000000u
        || (canon_pc & 0xE0000000u) == 0xA0000000u)
        canon_pc = (canon_pc & 0x1FFFFFFFu) | 0x80000000u;

    if (m->wince.nk_step_trace_done || m->wince.nk_step_trace_active)
        return false;
    if (canon_pc < 0x80079400u || canon_pc >= 0x80079800u)
        return false;

    m->wince.nk_step_trace_active = true;
    m->wince.nk_step_trace_remaining = 256;
    fprintf(stderr,
        "[WINCE_STEP] armed PC=0x%08X Canon=0x%08X"
        " RA=0x%08X SP=0x%08X Status=0x%08X\n",
        pc32,
        canon_pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
        (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_STATUS]);
    return true;
}

void wince_boot_note_ram_access(struct cpu *cpu, uint64_t paddr,
    const unsigned char *data, size_t len, bool is_write)
{
    machine_t *m;
    const char *name;
    uint64_t val = 0;
    uint16_t *count;
    size_t i;

    if (!cpu || !cpu->machine || !data || len == 0)
        return;

    m = wince_boot_from_gx(cpu->machine);
    if (!m || !m->wince.active || m->wince.suppress_watch_observer)
        return;
    if (!watched_ram_range_name(paddr, (uint64_t)len, &name))
        return;

    count = is_write ? &m->wince.ram_watch_write_count
        : &m->wince.ram_watch_read_count;
    if (*count >= 256)
        return;
    (*count)++;

    for (i = 0; i < len && i < 8; i++)
        val |= (uint64_t)data[i] << (8 * i);

    fprintf(stderr,
        "[WINCE_RAM] %c %s PA=0x%08" PRIx64 " len=%zu val=0x%llX"
        " PC=0x%08" PRIx64 " RA=0x%08X\n",
        is_write ? 'W' : 'R',
        name,
        paddr,
        len,
        (unsigned long long)val,
        (uint64_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
}

void wince_boot_note_mmio_access(struct machine *gxm, struct cpu *cpu,
    uint64_t paddr, size_t len, uint64_t value, bool is_write)
{
    machine_t *m;
    const char *name;
    uint16_t *count;

    if (!cpu)
        return;

    m = wince_boot_from_gx(gxm != NULL ? gxm : cpu->machine);
    if (!m || !m->wince.active || m->wince.suppress_watch_observer)
        return;
    if (!watched_mmio_range_name(paddr, (uint64_t)len, &name))
        return;

    count = is_write ? &m->wince.mmio_watch_write_count
        : &m->wince.mmio_watch_read_count;
    if (*count >= 256)
        return;
    (*count)++;

    fprintf(stderr,
        "[WINCE_MMIO] %c %s PA=0x%08" PRIx64 " len=%zu val=0x%llX"
        " PC=0x%08" PRIx64 " RA=0x%08X\n",
        is_write ? 'W' : 'R',
        name,
        paddr,
        len,
        (unsigned long long)value,
        (uint64_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
}

void wince_boot_note_idle_transition(struct cpu *cpu, const char *event,
    const char *mode, uint32_t status, uint32_t cause, uint32_t enabled,
    uint32_t mask, uint32_t raw_pending, uint32_t count, uint32_t compare,
    int compare_pending, int is_halted)
{
    machine_t *m;
    uint32_t pc;
    uint32_t epc;
    uint32_t sp;
    uint32_t ra;
    uint32_t badva;
    uint16_t vr0046 = 0;
    uint16_t vr008c = 0;
    uint16_t vr00a6 = 0;
    uint16_t vr0118 = 0;
    uint32_t lat1120 = 0;
    uint32_t lat112c = 0;
    uint32_t lat1b10 = 0;
    uint32_t lat1b20 = 0;

    if (!cpu)
        return;

    m = wince_boot_from_gx(cpu->machine);
    if (!m || !m->wince.active || !m->wince.cold_boot_redirected)
        return;
    if (m->wince.idle_diag_count >= 48)
        return;

    pc = (uint32_t)cpu->pc;
    epc = (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_EPC];
    badva = (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR];
    if (!cold_boot_scheduler_probe_pc_match(pc)
        && !cold_boot_scheduler_probe_epc_match(epc)
        && badva != UINT32_C(0x0201FE2C)) {
        return;
    }

    set_watch_observer(m, false);
    (void)load_va_half(m, UINT32_C(0xAF000046), &vr0046);
    (void)load_va_half(m, UINT32_C(0xAF00008C), &vr008c);
    (void)load_va_half(m, UINT32_C(0xAF0000A6), &vr00a6);
    (void)load_va_half(m, UINT32_C(0xAF000118), &vr0118);
    (void)load_vrc_latch_word(UINT32_C(0x0A001120), &lat1120);
    (void)load_vrc_latch_word(UINT32_C(0x0A00112C), &lat112c);
    (void)load_vrc_latch_word(UINT32_C(0x0A001B10), &lat1b10);
    (void)load_vrc_latch_word(UINT32_C(0x0A001B20), &lat1b20);
    set_watch_observer(m, true);

    sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
    ra = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA];

    fprintf(stderr,
        "[WINCE_IDLE] event=%s mode=%s pc=0x%08X epc=0x%08X"
        " ra=0x%08X sp=0x%08X status=0x%08X cause=0x%08X"
        " enabled=%u mask=0x%08X raw=0x%08X count=0x%08X"
        " compare=0x%08X cmp_pending=%d halted=%d"
        " vr0046=0x%04X vr008c=0x%04X vr00a6=0x%04X vr0118=0x%04X"
        " lat1120=0x%08X lat112c=0x%08X lat1b10=0x%08X lat1b20=0x%08X\n",
        event ? event : "?",
        mode ? mode : "?",
        pc,
        epc,
        ra,
        sp,
        status,
        cause,
        enabled,
        mask,
        raw_pending,
        count,
        compare,
        compare_pending,
        is_halted,
        vr0046,
        vr008c,
        vr00a6,
        vr0118,
        lat1120,
        lat112c,
        lat1b10,
        lat1b20);
    m->wince.idle_diag_count++;
}
