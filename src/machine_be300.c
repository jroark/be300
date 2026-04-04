/*
 *  machine_be300.c — BE-300 machine setup and run loop for GXemul.
 *
 *  Replaces the old 7800-line Unicorn machine.c. GXemul handles CP0, TLB,
 *  exceptions, and address translation natively.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <inttypes.h>
#include <time.h>

#include "be300.h"
#include "host_io.h"
#include "loader.h"
#include "ui.h"
#include "wince_boot.h"
#include "wince_resume_replay_data.h"
#include "wince_hw_seed_data.h"

/* GXemul headers */
#include "interrupt.h"
#include "cpu.h"
#include "cpu_mips.h"
#include "cop0.h"
#include "emul.h"
#include "device.h"
#include "devices.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"
#include "console.h"
#include "timer.h"
#include "settings.h"

/* GXemul externs */
extern volatile bool emul_shutdown;
extern volatile bool emul_executing;

enum {
    COLD_BOOT_PROBE_STACK_TLB_LOGGED      = 0x00020000u,
    COLD_BOOT_PROBE_STACK_TLB_VALID       = 0x00040000u,
    COLD_BOOT_PROBE_D8XX_FAULT_LOGGED     = 0x00080000u,
    COLD_BOOT_PROBE_D8XX_HELPER_INSTALLED = 0x00100000u,
    COLD_BOOT_PROBE_LATE_LOOP_LOGGED      = 0x00200000u,
    COLD_BOOT_PROBE_RESTORED_WAIT_LOGGED  = 0x00400000u,
    COLD_BOOT_PROBE_RESTORED_RA_LOGGED    = 0x00800000u,
    COLD_BOOT_PROBE_RESTORED_HANDOFF_DONE = 0x01000000u,
};

#define COLD_BOOT_OAL_BLOCK_BASE  UINT32_C(0x800794C0)
#define COLD_BOOT_OAL_BLOCK_WORDS 56u

static uint32_t g_cold_boot_oal_block_saved[COLD_BOOT_OAL_BLOCK_WORDS];
static bool g_cold_boot_oal_block_saved_valid = false;
static bool g_cold_boot_oal_block_restored = false;

static uint64_t be300_va32_to_mips64(uint32_t va)
{
    return (uint64_t)(int64_t)(int32_t)va;
}

static uint64_t be300_tlb_region_bits(machine_t *m)
{
    if (!m || !m->cpu || !m->cpu->cd.mips.coproc[0])
        return 0;

    return (uint64_t)m->cpu->cd.mips.coproc[0]->reg[COP0_ENTRYHI]
        & ENTRYHI_R_MASK;
}

static uint64_t be300_tlb_match_va_zeroext(machine_t *m, uint32_t va)
{
    uint64_t payload;

    payload = (uint64_t)va & (ENTRYHI_VPN2_MASK | UINT64_C(0x1FFF));
    return be300_tlb_region_bits(m) | payload;
}

static uint64_t be300_tlb_match_va_signext(machine_t *m, uint32_t va)
{
    uint64_t payload;

    payload = be300_va32_to_mips64(va)
        & (ENTRYHI_VPN2_MASK | UINT64_C(0x1FFF));
    return be300_tlb_region_bits(m) | payload;
}

static bool be300_read_va_word(machine_t *m, uint32_t va, uint32_t *value_out);

static bool be300_translate_va(machine_t *m, uint32_t va, uint64_t *paddr_out)
{
    uint64_t paddr;

    if (!m || !m->cpu || !m->cpu->translate_v2p)
        return false;
    if (!m->cpu->translate_v2p(m->cpu, be300_va32_to_mips64(va), &paddr,
            FLAG_NOEXCEPTIONS))
        return false;

    if (paddr_out)
        *paddr_out = paddr;
    return true;
}

static bool be300_is_plausible_resume_pc(machine_t *m, uint32_t pc)
{
    uint32_t instr = 0;

    if ((pc & 0x3u) != 0)
        return false;
    if (pc < UINT32_C(0x80060000) || pc >= UINT32_C(0x80100000))
        return false;
    if (pc >= UINT32_C(0x80079480) && pc <= UINT32_C(0x800797E0))
        return false;
    if (!be300_read_va_word(m, pc, &instr))
        return false;
    if (instr == 0 || instr == UINT32_C(0xFFFFFFFF))
        return false;
    return true;
}

static bool be300_find_stack_resume_pc(machine_t *m, uint32_t sp,
    uint32_t *resume_pc_out, uint32_t *slot_va_out)
{
    uint32_t off;

    if (!m || !m->cpu || !resume_pc_out)
        return false;

    for (off = 0; off < 0x200u; off += 4u) {
        uint32_t slot_va = sp + off;
        uint32_t candidate = 0;

        if (!be300_read_va_word(m, slot_va, &candidate))
            continue;
        if (!be300_is_plausible_resume_pc(m, candidate))
            continue;
        if (candidate >= UINT32_C(0x800947FC)
            && candidate <= UINT32_C(0x80094850)) {
            *resume_pc_out = candidate;
            if (slot_va_out)
                *slot_va_out = slot_va;
            return true;
        }
    }

    return false;
}

static void be300_invalidate_all(machine_t *m)
{
    if (!m || !m->cpu)
        return;
    if (m->cpu->invalidate_translation_caches)
        m->cpu->invalidate_translation_caches(m->cpu, 0, INVALIDATE_ALL);
    if (m->cpu->invalidate_code_translation)
        m->cpu->invalidate_code_translation(m->cpu, 0, INVALIDATE_ALL);
}

static bool be300_decode_tlb_match(machine_t *m, const struct mips_tlb *tlb,
    uint32_t va, uint64_t *pa_out, bool *valid_out, bool *dirty_out,
    bool *global_out, bool *odd_out, uint32_t *page_mask_out,
    const char **mode_out)
{
    const uint64_t vpn2_mask = ENTRYHI_R_MASK | ENTRYHI_VPN2_MASK;
    const uint32_t pagemask_mask = PAGEMASK_MASK_R4100;
    const int pagemask_shift = PAGEMASK_SHIFT_R4100;
    const int pfn_shift = 10;
    uint32_t pmask;
    uint64_t cached_hi;
    uint64_t cached_lo0;
    uint64_t cached_lo1;
    uint64_t entry_vpn2;
    uint64_t vaddr_vpn2;
    uint64_t entry_asid;
    uint64_t current_asid;
    uint64_t lo;
    uint64_t pfn;
    uint64_t paddr;
    uint64_t match_va_zero;
    uint64_t match_va_sign;
    uint64_t chosen_match_va = 0;
    uint32_t page_mask;
    bool odd;
    bool global;
    bool valid;
    bool dirty;
    size_t candidate_index;
    bool matched = false;
    const char *match_mode = NULL;

    if (!m || !m->cpu || !m->cpu->cd.mips.coproc[0] || !tlb)
        return false;

    pmask = (uint32_t)tlb->mask & pagemask_mask;
    cached_hi = tlb->hi;
    cached_lo0 = tlb->lo0;
    cached_lo1 = tlb->lo1;
    match_va_zero = be300_tlb_match_va_zeroext(m, va);
    match_va_sign = be300_tlb_match_va_signext(m, va);

    entry_asid = cached_hi & ENTRYHI_ASID;
    current_asid = m->cpu->cd.mips.coproc[0]->reg[COP0_ENTRYHI]
        & ENTRYHI_ASID;
    global = (cached_lo0 & ENTRYLO_G) != 0 && (cached_lo1 & ENTRYLO_G) != 0;

    for (candidate_index = 0; candidate_index < 2; candidate_index++) {
        uint64_t candidate_match_va =
            candidate_index == 0 ? match_va_zero : match_va_sign;

        if (pmask == 0) {
            entry_vpn2 = (cached_hi & vpn2_mask) >> pagemask_shift;
            vaddr_vpn2 = (candidate_match_va & vpn2_mask)
                >> pagemask_shift;
            page_mask = (UINT32_C(1) << (pagemask_shift - 1)) - 1u;
            odd = (candidate_match_va >> (pagemask_shift - 1)) & 1u;
        } else {
            int pageshift;

            switch (pmask | ((UINT32_C(1) << pagemask_shift) - 1u)) {
            case 0x00007ff: pageshift = 10; break;
            case 0x0001fff: pageshift = 12; break;
            case 0x0007fff: pageshift = 14; break;
            case 0x001ffff: pageshift = 16; break;
            case 0x007ffff: pageshift = 18; break;
            case 0x01fffff: pageshift = 20; break;
            case 0x07fffff: pageshift = 22; break;
            case 0x1ffffff: pageshift = 24; break;
            case 0x7ffffff: pageshift = 26; break;
            default:
                return false;
            }

            entry_vpn2 = (cached_hi & vpn2_mask) >> (pageshift + 1);
            vaddr_vpn2 = (candidate_match_va & vpn2_mask)
                >> (pageshift + 1);
            page_mask = (UINT32_C(1) << pageshift) - 1u;
            odd = (candidate_match_va >> pageshift) & 1u;
        }

        if (entry_vpn2 != vaddr_vpn2)
            continue;
        if (entry_asid != current_asid && !global)
            continue;

        chosen_match_va = candidate_match_va;
        matched = true;
        match_mode = candidate_index == 0 ? "zeroext" : "signext";
        break;
    }

    if (!matched)
        return false;

    lo = odd ? cached_lo1 : cached_lo0;
    valid = (lo & ENTRYLO_V) != 0;
    dirty = (lo & ENTRYLO_D) != 0;
    pfn = (lo & ENTRYLO_PFN_MASK) >> ENTRYLO_PFN_SHIFT;
    paddr = ((pfn << pfn_shift) & ~((uint64_t)page_mask))
        | (chosen_match_va & page_mask);

    if (pa_out)
        *pa_out = paddr;
    if (valid_out)
        *valid_out = valid;
    if (dirty_out)
        *dirty_out = dirty;
    if (global_out)
        *global_out = global;
    if (odd_out)
        *odd_out = odd;
    if (page_mask_out)
        *page_mask_out = page_mask;
    if (mode_out)
        *mode_out = match_mode;
    return true;
}

static bool be300_find_tlb_match(machine_t *m, uint32_t va, int *index_out,
    uint32_t *entryhi_out, uint32_t *entrylo0_out, uint32_t *entrylo1_out,
    uint32_t *mask_out, uint64_t *pa_out, bool *valid_out, bool *dirty_out,
    bool *global_out, bool *odd_out, const char **mode_out)
{
    struct mips_coproc *cp0;
    int i;

    if (!m || !m->cpu || !m->cpu->cd.mips.coproc[0])
        return false;

    cp0 = m->cpu->cd.mips.coproc[0];
    for (i = 0; i < cp0->nr_of_tlbs; i++) {
        uint64_t match_pa = 0;
        bool valid = false;
        bool dirty = false;
        bool global = false;
        bool odd = false;
        uint32_t page_mask = 0;
        const char *mode = NULL;

        if (!be300_decode_tlb_match(m, &cp0->tlbs[i], va, &match_pa,
                &valid, &dirty, &global, &odd, &page_mask, &mode)) {
            continue;
        }

        if (index_out)
            *index_out = i;
        if (entryhi_out)
            *entryhi_out = (uint32_t)cp0->tlbs[i].hi;
        if (entrylo0_out)
            *entrylo0_out = (uint32_t)cp0->tlbs[i].lo0;
        if (entrylo1_out)
            *entrylo1_out = (uint32_t)cp0->tlbs[i].lo1;
        if (mask_out)
            *mask_out = (uint32_t)cp0->tlbs[i].mask;
        if (pa_out)
            *pa_out = match_pa;
        if (valid_out)
            *valid_out = valid;
        if (dirty_out)
            *dirty_out = dirty;
        if (global_out)
            *global_out = global;
        if (odd_out)
            *odd_out = odd;
        if (mode_out)
            *mode_out = mode;
        return true;
    }

    return false;
}

static void be300_install_cold_boot_d8xx_helper(machine_t *m, uint32_t fault_va)
{
    struct mips_coproc *cp0;
    uint64_t resolved_pa = 0;
    int entry_index;

    if (!m || !m->cpu || !m->cpu->cd.mips.coproc[0])
        return;
    if (m->wince.cold_boot_pc_probes_logged
        & COLD_BOOT_PROBE_D8XX_HELPER_INSTALLED)
        return;

    cp0 = m->cpu->cd.mips.coproc[0];
    entry_index = cp0->nr_of_tlbs - 1;
    if (entry_index < 0)
        return;

    mips_coproc_tlb_set_entry(m->cpu, entry_index, 4096,
        UINT32_C(0xFFFFD800), UINT32_C(0x00001000), UINT32_C(0x00002000),
        1, 1, 1, 1, 1, 0, 3, 3);
    cp0->tlbs[entry_index].hi = UINT32_C(0xFFFFD800);
    cp0->tlbs[entry_index].lo0 = UINT32_C(0x0000019F);
    cp0->tlbs[entry_index].lo1 = UINT32_C(0x000001DF);
    m->wince.cold_boot_pc_probes_logged |=
        COLD_BOOT_PROBE_D8XX_HELPER_INSTALLED;
    be300_invalidate_all(m);

    fprintf(stderr,
        "[COLD_BOOT] Installed helper TLB[%d] for VA 0xFFFFD800"
        " after BadVA=0x%08X (hi=0x%08X lo0=0x%08X lo1=0x%08X)\n",
        entry_index,
        fault_va,
        (uint32_t)cp0->tlbs[entry_index].hi,
        (uint32_t)cp0->tlbs[entry_index].lo0,
        (uint32_t)cp0->tlbs[entry_index].lo1);
    if (be300_translate_va(m, UINT32_C(0xFFFFD888), &resolved_pa)) {
        fprintf(stderr,
            "[COLD_BOOT] Helper probe: VA 0xFFFFD888 -> PA 0x%08" PRIx64 "\n",
            resolved_pa);
    } else {
        fprintf(stderr,
            "[COLD_BOOT] Helper probe: VA 0xFFFFD888 still unmapped\n");
    }
}

static bool be300_read_va_word(machine_t *m, uint32_t va, uint32_t *value_out)
{
    unsigned char bytes[4];

    if (!m || !m->cpu || !value_out)
        return false;
    if (!m->cpu->memory_rw(m->cpu, m->cpu->mem, be300_va32_to_mips64(va),
            bytes, sizeof(bytes), MEM_READ, CACHE_DATA)) {
        return false;
    }

    *value_out = (uint32_t)bytes[0]
        | ((uint32_t)bytes[1] << 8)
        | ((uint32_t)bytes[2] << 16)
        | ((uint32_t)bytes[3] << 24);
    return true;
}

static void be300_log_va_words(machine_t *m, const char *label, uint32_t va,
    uint32_t bytes)
{
    uint32_t off;

    fprintf(stderr, "[COLD_BOOT_LATE] %s va=0x%08X", label, va);
    for (off = 0; off < bytes; off += 4) {
        uint32_t word;

        if (be300_read_va_word(m, va + off, &word))
            fprintf(stderr, " %08X", word);
        else
            fprintf(stderr, " ????????");
    }
    fprintf(stderr, "\n");
}

static void be300_capture_cold_boot_oal_block(machine_t *m)
{
    unsigned i;

    if (!m || !m->cpu)
        return;

    g_cold_boot_oal_block_saved_valid = false;
    g_cold_boot_oal_block_restored = false;
    for (i = 0; i < COLD_BOOT_OAL_BLOCK_WORDS; i++) {
        if (!be300_read_va_word(m,
                COLD_BOOT_OAL_BLOCK_BASE + i * 4u,
                &g_cold_boot_oal_block_saved[i])) {
            return;
        }
    }
    g_cold_boot_oal_block_saved_valid = true;
}

static void be300_restore_cold_boot_oal_block(machine_t *m, const char *reason)
{
    unsigned i;

    if (!m || !m->cpu || !g_cold_boot_oal_block_saved_valid
        || g_cold_boot_oal_block_restored) {
        return;
    }

    for (i = 0; i < COLD_BOOT_OAL_BLOCK_WORDS; i++) {
        store_32bit_word(m->cpu,
            be300_va32_to_mips64(COLD_BOOT_OAL_BLOCK_BASE + i * 4u),
            g_cold_boot_oal_block_saved[i]);
    }
    be300_invalidate_all(m);
    g_cold_boot_oal_block_restored = true;
    fprintf(stderr,
        "[COLD_BOOT] Restored full OAL block at 0x%08X"
        " (%u words)%s%s\n",
        COLD_BOOT_OAL_BLOCK_BASE,
        COLD_BOOT_OAL_BLOCK_WORDS,
        reason ? " reason=" : "",
        reason ? reason : "");
}

static void be300_maybe_restore_cold_boot_oal_block(machine_t *m,
    const char *context)
{
    uint32_t ready_ptr = 0;
    char reason[80];

    if (!m || !m->cpu || !m->cfg.wince_cold_boot
        || !m->wince.cold_boot_wait_logged
        || !g_cold_boot_oal_block_saved_valid
        || g_cold_boot_oal_block_restored) {
        return;
    }

    if (!be300_read_va_word(m, UINT32_C(0x80669554), &ready_ptr)
        || ready_ptr == 0) {
        return;
    }

    if (context && context[0] != '\0') {
        snprintf(reason, sizeof(reason),
            "%s:80669554=0x%08X", context, ready_ptr);
    } else {
        snprintf(reason, sizeof(reason),
            "80669554=0x%08X", ready_ptr);
    }
    be300_restore_cold_boot_oal_block(m, reason);
}

static bool be300_cold_late_probe_match(uint32_t value)
{
    return (value >= UINT32_C(0x80079900) && value <= UINT32_C(0x800799C0))
        || (value >= UINT32_C(0x80078320) && value <= UINT32_C(0x8007835C))
        || (value >= UINT32_C(0x80079560) && value <= UINT32_C(0x80079584))
        || (value >= UINT32_C(0x800A7170) && value <= UINT32_C(0x800A71A8))
        || (value >= UINT32_C(0x80089760) && value <= UINT32_C(0x800897C0))
        || (value >= UINT32_C(0x80089870) && value <= UINT32_C(0x800898F8))
        || (value >= UINT32_C(0x80089A40) && value <= UINT32_C(0x80089A60))
        || (value >= UINT32_C(0x8008B4F0) && value <= UINT32_C(0x8008B6C0))
        || (value >= UINT32_C(0x8007A3F0) && value <= UINT32_C(0x8007A6B0));
}

static bool be300_cold_copy_loop_match(uint32_t value)
{
    return (value >= UINT32_C(0x80078320) && value <= UINT32_C(0x8007835C))
        || (value >= UINT32_C(0x80079560) && value <= UINT32_C(0x80079584))
        || (value >= UINT32_C(0x800A7170) && value <= UINT32_C(0x800A71A8));
}

static void be300_log_cold_boot_late_loop(machine_t *m)
{
    uint64_t translated_badva = 0;
    uint64_t tlb_pa = 0;
    uint32_t pc;
    uint32_t epc;
    uint32_t badva;
    uint32_t sp;
    uint32_t s0;
    uint32_t s1;
    uint32_t status;
    uint32_t cause;
    uint32_t entryhi = 0;
    uint32_t entrylo0 = 0;
    uint32_t entrylo1 = 0;
    uint32_t mask = 0;
    int tlb_index = -1;
    bool tlb_valid = false;
    bool tlb_dirty = false;
    bool tlb_global = false;
    bool tlb_odd = false;
    const char *tlb_mode = NULL;

    if (!m || !m->cpu || !m->cfg.wince_cold_boot || !m->wince.log_stall)
        return;
    if ((m->wince.cold_boot_pc_probes_logged
        & COLD_BOOT_PROBE_LATE_LOOP_LOGGED) != 0) {
        return;
    }

    pc = (uint32_t)m->cpu->pc;
    epc = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC];
    badva = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR];
    if (badva != UINT32_C(0x0201FE2C)
        && !be300_cold_late_probe_match(pc)
        && !be300_cold_late_probe_match(epc)) {
        return;
    }

    m->wince.cold_boot_pc_probes_logged |= COLD_BOOT_PROBE_LATE_LOOP_LOGGED;
    sp = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP];
    s0 = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_S0];
    s1 = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_S1];
    status = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS];
    cause = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE];

    fprintf(stderr,
        "[COLD_BOOT_LATE] pc=0x%08X epc=0x%08X badva=0x%08X"
        " sp=0x%08X s0=0x%08X s1=0x%08X"
        " status=0x%08X cause=0x%08X ra=0x%08X\n",
        pc,
        epc,
        badva,
        sp,
        s0,
        s1,
        status,
        cause,
        (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA]);

    if (be300_find_tlb_match(m, badva, &tlb_index, &entryhi, &entrylo0,
            &entrylo1, &mask, &tlb_pa, &tlb_valid, &tlb_dirty, &tlb_global,
            &tlb_odd, &tlb_mode)) {
        fprintf(stderr,
            "[COLD_BOOT_LATE] badva_tlb idx=%d pa=0x%08" PRIx64
            " valid=%d dirty=%d global=%d odd=%d mode=%s"
            " hi=0x%08X lo0=0x%08X lo1=0x%08X mask=0x%08X\n",
            tlb_index,
            tlb_pa,
            tlb_valid ? 1 : 0,
            tlb_dirty ? 1 : 0,
            tlb_global ? 1 : 0,
            tlb_odd ? 1 : 0,
            tlb_mode ? tlb_mode : "?",
            entryhi,
            entrylo0,
            entrylo1,
            mask);
    } else {
        fprintf(stderr, "[COLD_BOOT_LATE] badva_tlb unmapped\n");
    }
    if (be300_translate_va(m, badva, &translated_badva)) {
        fprintf(stderr,
            "[COLD_BOOT_LATE] badva_translate pa=0x%08" PRIx64 "\n",
            translated_badva);
    } else {
        fprintf(stderr, "[COLD_BOOT_LATE] badva_translate failed\n");
    }

    be300_log_va_words(m, "pc_code", pc & ~UINT32_C(0x1F), 0x40u);
    if (epc != 0)
        be300_log_va_words(m, "epc_code", epc & ~UINT32_C(0x1F), 0x40u);
    be300_log_va_words(m, "init_9554", UINT32_C(0x80669554), 0x20u);
    be300_log_va_words(m, "init_956c", UINT32_C(0x8066956C), 0x20u);
    be300_log_va_words(m, "init_96d4", UINT32_C(0x806696D4), 0x20u);
    be300_log_va_words(m, "init_8c20", UINT32_C(0x80668C20), 0x20u);
    be300_log_va_words(m, "sched_9614", UINT32_C(0x80679614), 0x20u);
    be300_log_va_words(m, "sched_961c", UINT32_C(0x8067961C), 0x20u);
    be300_log_va_words(m, "sched_9654", UINT32_C(0x80679654), 0x20u);
    be300_log_va_words(m, "sched_96d8", UINT32_C(0x806796D8), 0x20u);
    be300_log_va_words(m, "sched_96f8", UINT32_C(0x806796F8), 0x20u);
    be300_log_va_words(m, "sched_9780", UINT32_C(0x80679780), 0x20u);
    be300_log_va_words(m, "obj_table", UINT32_C(0x8066BFC0), 0x40u);
    if (badva >= UINT32_C(0x80000000))
        be300_log_va_words(m, "fault_page", badva & ~UINT32_C(0x1F), 0x20u);
    else
        fprintf(stderr,
            "[COLD_BOOT_LATE] fault_page skipped for low badva=0x%08X\n",
            badva);
    be300_log_va_words(m, "stack", sp & ~UINT32_C(0x1F), 0x40u);
    if (s0 != 0)
        be300_log_va_words(m, "s0_window", s0 & ~UINT32_C(0x1F), 0x60u);
    if (s1 >= UINT32_C(0x80000000))
        be300_log_va_words(m, "s1_window", s1 & ~UINT32_C(0x1F), 0x40u);
    else if (s1 != 0)
        fprintf(stderr,
            "[COLD_BOOT_LATE] s1_window skipped for low s1=0x%08X\n",
            s1);

    if (badva == 0
        && (be300_cold_copy_loop_match(pc)
            || be300_cold_copy_loop_match(epc))) {
        be300_restore_cold_boot_oal_block(m, "late-kernel-copy-loop");
    }
}

static void be300_handle_stop_signal(int signum)
{
    (void)signum;
    emul_shutdown = true;
    __sync_synchronize();
}

static void be300_serial_ring_push(machine_t *m, int ch)
{
    if (!m)
        return;

    if (m->serial_count == BE300_SERIAL_RING_CAP) {
        m->serial_tail = (m->serial_tail + 1u) % BE300_SERIAL_RING_CAP;
        m->serial_count--;
    }

    m->serial_ring[m->serial_head] = (char)ch;
    m->serial_head = (m->serial_head + 1u) % BE300_SERIAL_RING_CAP;
    m->serial_count++;
}

static void be300_serial_sink(int ch, void *user_data)
{
    be300_serial_ring_push((machine_t *)user_data, ch);
}

static void be300_register_linux_input_if_needed(machine_t *m)
{
    if (m->input_registered)
        return;

    extern void be300_register_input(struct machine *, machine_t *, bool);
    be300_register_input(m->gxe_machine, m, m->cfg.log_mmio);
    m->input_registered = true;
}

static void be300_set_linux_boot_strings(machine_t *m,
                                         const char *kernel_name,
                                         const char *cmdline)
{
    struct machine *gxm = m->gxe_machine;
    const char *safe_kernel = kernel_name ? kernel_name : "vmlinux";
    const char *safe_cmdline = cmdline ? cmdline : "";

    free(gxm->boot_kernel_filename);
    gxm->boot_kernel_filename = strdup(safe_kernel);

    free(gxm->boot_string_argument);
    gxm->boot_string_argument = strdup(safe_cmdline);

    gxm->bootstr = gxm->boot_kernel_filename;
    gxm->bootarg = gxm->boot_string_argument[0] ? gxm->boot_string_argument : NULL;
}

static void be300_refresh_linux_bootinfo(machine_t *m)
{
    struct machine *gxm = m->gxe_machine;
    uint64_t ram_top;
    uint64_t argv_base;

    if (!gxm->prom_emulation)
        return;

    ram_top = 0x80000000ULL + ((uint64_t)gxm->physical_ram_in_mb << 20);
    argv_base = ram_top - 512;

    m->cpu->cd.mips.gpr[MIPS_GPR_A0] = 1;
    m->cpu->cd.mips.gpr[MIPS_GPR_A1] = argv_base;
    m->cpu->cd.mips.gpr[MIPS_GPR_A2] = ram_top - 256;

    store_32bit_word(m->cpu, argv_base, argv_base + 16);
    store_32bit_word(m->cpu, argv_base + 4, 0);
    store_32bit_word(m->cpu, argv_base + 8, 0);
    store_string(m->cpu, argv_base + 16, gxm->boot_kernel_filename);

    if (gxm->boot_string_argument && gxm->boot_string_argument[0]) {
        m->cpu->cd.mips.gpr[MIPS_GPR_A0]++;
        store_32bit_word(m->cpu, argv_base + 4, argv_base + 64);
        store_32bit_word(m->cpu, argv_base + 8, 0);
        store_string(m->cpu, argv_base + 64, gxm->boot_string_argument);
    }
}

static int be300_boot_linux_path(machine_t *m, const char *kernel_path,
                                 const char *cmdline, const char *ram_path)
{
    uint32_t entry_va = 0;
    uint32_t jiffies_pa = 0;

    if (!kernel_path)
        return -1;

    be300_set_linux_boot_strings(m, kernel_path, cmdline);
    be300_refresh_linux_bootinfo(m);

    if (loader_load_elf(m, kernel_path, &entry_va, &jiffies_pa) != 0) {
        fprintf(stderr, "[BE300] Failed to load kernel ELF\n");
        return -1;
    }

    m->cpu->pc = (uint64_t)(int32_t)entry_va;
    m->boot_mode = BE300_BOOT_LINUX_PATH;
    fprintf(stderr, "[BE300] Kernel entry: PC=0x%08X\n", entry_va);

    if (cmdline && cmdline[0])
        fprintf(stderr, "[BE300] Kernel cmdline: %s\n", cmdline);

    if (ram_path)
        loader_load_ram(m, ram_path);

    be300_register_linux_input_if_needed(m);
    return 0;
}

static int be300_boot_linux_memory_internal(machine_t *m,
                                            const void *kernel_data,
                                            size_t kernel_size,
                                            const char *cmdline)
{
    uint32_t entry_va = 0;
    uint32_t jiffies_pa = 0;

    if (!m || !kernel_data || kernel_size == 0 || !cmdline) {
        fprintf(stderr, "[BE300] Web boot requires a kernel image\n");
        return -1;
    }
    if (m->boot_mode != BE300_BOOT_NONE) {
        fprintf(stderr, "[BE300] Machine already has boot media loaded\n");
        return -1;
    }

    be300_set_linux_boot_strings(m, "vmlinux", cmdline);
    be300_refresh_linux_bootinfo(m);

    if (loader_load_elf_from_memory(m, kernel_data, kernel_size,
                                    &entry_va, &jiffies_pa) != 0) {
        fprintf(stderr, "[BE300] Failed to load in-memory kernel ELF\n");
        return -1;
    }

    m->cpu->pc = (uint64_t)(int32_t)entry_va;
    m->boot_mode = BE300_BOOT_LINUX_MEMORY;
    fprintf(stderr, "[BE300] In-memory kernel entry: PC=0x%08X\n", entry_va);
    fprintf(stderr, "[BE300] Kernel cmdline: %s\n", cmdline);

    be300_register_linux_input_if_needed(m);
    return 0;
}


machine_t *be300_create(const machine_config_t *cfg)
{
    static bool subsystems_initialized = false;
    machine_t *m = calloc(1, sizeof(machine_t));
    if (!m) return NULL;
    m->cfg = *cfg;
    m->boot_mode = BE300_BOOT_NONE;
    m->use_builtin_ui = true;
    m->save_exit_screenshot = true;
    m->mirror_serial_to_stdout = true;
    m->fb_width = 240;
    m->fb_height = 320;
    m->fb_stride = 256;
    wince_boot_init(m);

    /*
     * Initialize GXemul subsystems once per process.
     */
    if (!subsystems_initialized) {
        debugmsg_init();

        /* Initialize global settings before console_init needs them */
        extern void be300_init_global_settings(void);
        be300_init_global_settings();

        cpu_init();
        device_init();
        timer_init();
        console_init();

        /* Initialize machine type registry (registers hpcmips) */
        machine_init();
        
        subsystems_initialized = true;
    }

    /* Reset global execution flags for a new run */
    emul_executing = false;
    emul_shutdown = false;

    /*
     * Create the GXemul emul and machine objects.
     */
    m->emul = emul_new("be300");
    if (!m->emul) {
        fprintf(stderr, "[BE300] emul_new failed\n");
        free(m);
        return NULL;
    }

    struct machine *gxm = emul_add_machine(m->emul, "be300");
    if (!gxm) {
        fprintf(stderr, "[BE300] emul_add_machine failed\n");
        free(m);
        return NULL;
    }
    m->gxe_machine = gxm;

    /*
     * Configure as BE-300.
     */
    gxm->machine_type = MACHINE_HPCMIPS;
    gxm->machine_subtype = MACHINE_HPCMIPS_CASIO_BE300;
    gxm->cpu_name = strdup("VR4131");
    gxm->physical_ram_in_mb = cfg->sdram_size / (1024 * 1024);
    /* Enable prom emulation for Linux (sets up hpc_bootinfo, argc/argv),
     * but disable for NAND boot (SPL doesn't use NetBSD boot convention) */
    gxm->prom_emulation = cfg->nand_path ? 0 : 1;
    gxm->boot_kernel_filename = cfg->kernel_path ? strdup(cfg->kernel_path) : strdup("");
    gxm->boot_string_argument = cfg->cmdline ? strdup(cfg->cmdline) : strdup("");

    if (cfg->trace)
        gxm->instruction_trace = 1;

    /*
     * Manually set up the machine (adapted from emul_machine_setup).
     * We skip file_load since we use our own loader.
     */

    /* Create memory */
    uint64_t memory_amount = (uint64_t)gxm->physical_ram_in_mb * 1048576;
    gxm->memory = memory_new(memory_amount);

    /* Create CPU */
    gxm->ncpus = 1;
    gxm->cpus = malloc(sizeof(struct cpu *));
    gxm->cpus[0] = cpu_new(gxm->memory, gxm, 0, gxm->cpu_name);
    if (!gxm->cpus[0]) {
        fprintf(stderr, "[BE300] cpu_new failed for VR4131\n");
        free(m);
        return NULL;
    }
    gxm->bootstrap_cpu = 0;
    m->cpu = gxm->cpus[0];

    /*
     * Run the hpcmips machine setup function.
     * This calls dev_vr41xx_init(), adds the VRC4173 SIU (ns16550),
     * framebuffer, and configures hpc_bootinfo.
     *
     * Disable exit-on-error so missing devices (pcic, etc.) just
     * print a warning instead of crashing.
     */
    device_set_exit_on_error(0);
    machine_setup(gxm);
    device_set_exit_on_error(1);

    /*
     * Initialize our peripheral state structs.
     */
    bcu_init(&m->bcu);
    cmu_init(&m->cmu);
    pmu_init(&m->pmu);
    icu_init(&m->icu);
    siu_init(&m->siu);
    rtc_init(&m->rtc);
    gpio_init(&m->gpio);
    nand_init(&m->nand, NULL, 0);

    /*
     * Load kernel or NAND image.
     */
    if (cfg->kernel_path) {
        if (be300_boot_linux_path(m, cfg->kernel_path, cfg->cmdline,
                                  cfg->ram_path) != 0) {
            free(m);
            return NULL;
        }

    } else if (cfg->nand_path) {
        uint32_t entry_va = 0;

        if (loader_load_nand(m, cfg->nand_path, &entry_va) != 0) {
            fprintf(stderr, "[BE300] Failed to load NAND image\n");
            free(m);
            return NULL;
        }

        m->cpu->pc = (uint64_t)(int32_t)entry_va;
        m->boot_mode = BE300_BOOT_NAND;
        fprintf(stderr, "[BE300] NAND SPL entry: PC=0x%08X\n", entry_va);

        /* Re-initialize NAND controller with image data */
        if (m->nand_data) {
            nand_init(&m->nand, m->nand_data, m->nand_size);
        }

        /*
         * Map RAM at PA 0x1FC00000 for BEV=1 exception vectors.
         *
         * With prom_emulation=0 (NAND boot), CP0 Status has BEV=1.
         * The SPL doesn't clear BEV, so NK.exe starts with BEV=1.
         * Exception vectors (TLB refill, general, interrupt) go to
         * PA 0x1FC00000 + offset.  Real BE-300 hardware has 16K ROM
         * there (per docs/hardware.txt).  Without this RAM, exception
         * handlers can't be installed and WinCE can't set up virtual
         * memory or run the scheduler.
         */
        dev_ram_init(gxm, 0x1FC00000, 0x4000, DEV_RAM_RAM, 0, NULL);

        /*
         * Load the real BE-300 boot ROM into PA 0x1FC00000.
         * This 16KB masked ROM contains the reset vector, BEV
         * exception handlers, and utility routines that NK.exe
         * may call during initialization.  Captured from real
         * hardware via BEDiag (CRC32=0xFA3B5582).
         *
         * Falls back to ERET stubs if the ROM file is not found.
         */
        {
            const char *rom_paths[] = {
                "be300_boot_rom.bin",
                "../hardware_survey/be300_boot_rom.bin",
                NULL
            };
            FILE *rom_fp = NULL;
            for (int pi = 0; rom_paths[pi] && !rom_fp; pi++)
                rom_fp = fopen(rom_paths[pi], "rb");

            if (rom_fp) {
                uint8_t rom_buf[0x4000];
                size_t rom_read = fread(rom_buf, 1, sizeof(rom_buf), rom_fp);
                fclose(rom_fp);
                if (rom_read == sizeof(rom_buf)) {
                    uint64_t base_va = 0xffffffffBFC00000ULL;
                    for (size_t i = 0; i < sizeof(rom_buf); i += 4) {
                        uint32_t w = rom_buf[i] | (rom_buf[i+1] << 8) |
                                     (rom_buf[i+2] << 16) | (rom_buf[i+3] << 24);
                        store_32bit_word(m->cpu, base_va + i, w);
                    }
                    fprintf(stderr, "[BE300] Loaded real boot ROM"
                        " (%zu bytes) at PA 0x1FC00000\n", rom_read);

                    /*
                     * Patch the ROM's BEV exception vectors with MIPS32
                     * handlers.  The real ROM has:
                     *   +0x200 (TLB Refill): all 0xFF (no handler)
                     *   +0x380 (General Exception): boot code, not a handler
                     * The ROM uses MIPS16 code that GXemul can't execute.
                     *
                     * We write proper MIPS32 handlers into the 0xFF-filled
                     * area at +0x200-0x37F (384 bytes available).
                     */
                    {
                        uint64_t rom_va = 0xffffffffBFC00000ULL;

                        /* +0x200: BEV TLB Refill handler.
                         * Maps kseg3 (0xC0000000+) to low SDRAM by masking
                         * the upper address bits.  The ROM's MIPS16 dispatcher
                         * uses stack at 0xFF10xxxx which needs to map to
                         * PA 0x00F0xxxx.  Uses VPN2 & 0x1FFFFF as PFN. */
                        static const uint32_t tlb_refill[] = {
                            0x401B5000, /* mfc0 $k1, EntryHi    */
                            0x001BD1C2, /* srl  $k0, $k1, 7     */
                            0x3C1B0003, /* lui  $k1, 0x0003     */
                            0x377BFFFF, /* ori  $k1, 0xFFFF     */
                            0x035BD024, /* and  $k0, $k0, $k1   */
                            0x375A001F, /* ori  $k0, $k0, 0x1F  */
                            0x409A1000, /* mtc0 $k0, EntryLo0   */
                            0x275B0040, /* addiu $k1, $k0, 0x40 */
                            0x409B1800, /* mtc0 $k1, EntryLo1   */
                            0x42000006, /* tlbwr                 */
                            0x42000018, /* eret                  */
                        };
                        for (unsigned j = 0; j < sizeof(tlb_refill)/sizeof(tlb_refill[0]); j++)
                            store_32bit_word(m->cpu,
                                rom_va + 0x200 + j * 4,
                                tlb_refill[j]);

                        /*
                         * +0x280: General exception dispatcher.
                         * Checks ExcCode: TLB miss → identity map,
                         * interrupt → clear timer + ERET, other → ERET.
                         *
                         * The BEV general exception vector is at +0x380,
                         * which is the boot code continuation. We can't
                         * easily patch +0x380 (it's a delay slot of the
                         * JAL at +0x37C). Instead, we put the handler at
                         * +0x280 and patch +0x380 to jump here when EXL=1.
                         */
                        static const uint32_t gen_exc[] = {
                            /* +0x280: Check ExcCode */
                            0x401A6800, /* mfc0 $k0, Cause       */
                            0x001AD082, /* srl  $k0, $k0, 2      */
                            0x335A001F, /* andi $k0, $k0, 0x1F   */
                            0x1340000A, /* beqz $k0, +10 → irq   */
                            0x00000000, /* nop                    */
                            /* ExcCode 2 (TLBL) or 3 (TLBS) → TLB */
                            0x375A0002, /* ori  $k0, $k0, 2 (NOP-like, k0 already has exccode) */
                            /* actually just check: */
                            0x401A6800, /* mfc0 $k0, Cause       */
                            0x001AD082, /* srl  $k0, $k0, 2      */
                            0x335A001F, /* andi $k0, $k0, 0x1F   */
                            0x235BFFFE, /* addi $k1, $k0, -2     */
                            0x2F7B0002, /* sltiu $k1, $k1, 2     */
                            /* k1=1 if ExcCode is 2 or 3 (TLB) */
                            0x1760FFEE, /* bne $k1,$zero, -18 → +0x200 (TLB handler) */
                            0x00000000, /* nop                    */
                            /* Other exceptions: just ERET */
                            0x42000018, /* eret                  */
                        };
                        /* Actually, let me write a cleaner version */
                        static const uint32_t gen_handler[] = {
                            /* +0x280: general exception handler */
                            0x401A6800, /* mfc0 $k0, Cause        */
                            0x335A007C, /* andi $k0, $k0, 0x7C    */
                            /* k0 now has ExcCode << 2              */
                            /* 0x00 = Interrupt                     */
                            0x13400006, /* beqz $k0, handle_irq (+6)*/
                            0x00000000, /* nop                      */
                            /* 0x08 = TLBL, 0x0C = TLBS            */
                            0x235BFFF8, /* addi $k1, $k0, -8       */
                            0x2F7B0008, /* sltiu $k1, $k1, 8      */
                            /* k1=1 if exccode<<2 is 8..15         */
                            /* i.e. ExcCode 2 or 3 (TLBL/TLBS)     */
                            0x17600008, /* bne $k1, $zero, tlb (+8)*/
                            0x00000000, /* nop                      */
                            /* Other: just ERET                     */
                            0x42000018, /* eret                     */
                            0x00000000, /* nop                      */
                            /* handle_irq: ERET (timer stays pending */
                            /* but at least we return cleanly)       */
                            0x42000018, /* eret                     */
                            0x00000000, /* nop                      */
                            /* handle_tlb: map with addr mask        */
                            /* (same as +0x200 handler: mask PFN    */
                            /* with 0x1FFFFF to keep within SDRAM)  */
                            0x401B5000, /* mfc0 $k1, EntryHi       */
                            0x001BD1C2, /* srl  $k0, $k1, 7        */
                            0x3C1B1FFF, /* lui  $k1, 0x1FFF        */
                            0x377BFFFF, /* ori  $k1, 0xFFFF        */
                            0x035BD024, /* and  $k0, $k0, $k1      */
                            0x375A001F, /* ori  $k0, $k0, 0x1F     */
                            0x409A1000, /* mtc0 $k0, EntryLo0      */
                            0x275B0040, /* addiu $k1, $k0, 0x40    */
                            0x409B1800, /* mtc0 $k1, EntryLo1      */
                            0x42000006, /* tlbwr                    */
                            0x42000018, /* eret                     */
                        };
                        /*
                         * The handler body goes at +0x2300 (unused ROM
                         * padding area, 0x2250-0x3FFF) to avoid
                         * overwriting the ROM's dispatch table at +0x2A0
                         * which the MIPS16 boot dispatcher reads.
                         * A jump stub at +0x280 redirects to +0x2300.
                         */
                        for (unsigned j = 0;
                             j < sizeof(gen_handler)/sizeof(gen_handler[0]);
                             j++)
                            store_32bit_word(m->cpu,
                                rom_va + 0x2300 + j * 4,
                                gen_handler[j]);
                        /* +0x280: J 0xBFC02300 (jump to relocated handler) */
                        store_32bit_word(m->cpu, rom_va + 0x280,
                            0x0BF008C0);  /* j 0xBFC02300 */
                        store_32bit_word(m->cpu, rom_va + 0x284,
                            0x00000000);  /* nop (delay slot) */

                        /*
                         * +0x380: Patch the BEV general exception entry.
                         * Original is boot code (`li $a0, 0` = delay slot
                         * of JAL at +0x37C).  We can't change +0x380
                         * (delay slot) but we CAN change +0x37C to call
                         * our wrapper, and put a check at +0x384.
                         *
                         * Simpler: just redirect +0x380 area. The boot
                         * code flow via the reset vector reaches +0x37C
                         * which has a JAL with delay slot at +0x380.
                         * We preserve the delay slot, but patch +0x384
                         * to check EXL and branch to our handler.
                         */
                        /* At +0x384 (after delay slot): check EXL */
                        /* Original: lui $a1, 0x8001 (0x3c058001)  */
                        static const uint32_t exc_check[] = {
                            /* +0x384: check if exception or boot */
                            0x401A6000, /* mfc0 $k0, Status        */
                            0x335A0002, /* andi $k0, $k0, 0x2 EXL  */
                            0x1740FFBF, /* bne $k0,$zero, -65 → +0x284... */
                        };
                        /* Calculate branch offset: from +0x38C to +0x280
                           offset = (0x280 - 0x390) / 4 = -0x110/4 = -68
                           BNE encoding: offset is (target - (PC+4))/4
                           PC = 0x38C, target = 0x280
                           offset = (0x280 - 0x390) / 4 = -0x44 = -68
                           16-bit signed: 0xFFBC */
                        store_32bit_word(m->cpu, rom_va + 0x384,
                            0x401A6000); /* mfc0 $k0, Status */
                        store_32bit_word(m->cpu, rom_va + 0x388,
                            0x335A0002); /* andi $k0, $k0, 2 */
                        store_32bit_word(m->cpu, rom_va + 0x38C,
                            0x1740FFBC); /* bne $k0,$zero, -68 → +0x280 */
                        store_32bit_word(m->cpu, rom_va + 0x390,
                            0x00000000); /* nop (delay slot) */
                        /* Original boot code relocated from +0x384: */
                        store_32bit_word(m->cpu, rom_va + 0x394,
                            0x3C058001); /* lui $a1, 0x8001 (was +0x384) */
                        store_32bit_word(m->cpu, rom_va + 0x398,
                            0x24A50034); /* addiu $a1, 52 (was +0x388) */
                        store_32bit_word(m->cpu, rom_va + 0x39C,
                            0x80A00000); /* lb $zero, 0($a1) (was +0x38C) */
                        store_32bit_word(m->cpu, rom_va + 0x3A0,
                            0x3C089FC0); /* lui $t0, 0x9FC0 (was +0x390) */
                        store_32bit_word(m->cpu, rom_va + 0x3A4,
                            0x25080C85); /* addiu $t0, 0xC85 (was +0x394) */
                        store_32bit_word(m->cpu, rom_va + 0x3A8,
                            0x0100F809); /* jalr $t0 (was +0x398) */
                        store_32bit_word(m->cpu, rom_va + 0x3AC,
                            0x00000000); /* nop (was +0x39C) */
                        store_32bit_word(m->cpu, rom_va + 0x3B0,
                            0x3C059FC0); /* lui $a1, 0x9FC0 (was +0x3A0) */
                        store_32bit_word(m->cpu, rom_va + 0x3B4,
                            0x24A50C21); /* addiu $a1, 0xC21 (was +0x3A4) */
                        store_32bit_word(m->cpu, rom_va + 0x3B8,
                            0x00A0F809); /* jalr $a1 (was +0x3A8) */
                        store_32bit_word(m->cpu, rom_va + 0x3BC,
                            0x00000000); /* nop (was +0x3AC) */
                        store_32bit_word(m->cpu, rom_va + 0x3C0,
                            0x0FF0013A); /* jal 0xFC004E8 (was +0x3B0) */
                        store_32bit_word(m->cpu, rom_va + 0x3C4,
                            0x00000000); /* nop (was +0x3B4) */
                        store_32bit_word(m->cpu, rom_va + 0x3C8,
                            0x0FF00122); /* jal 0xFC00488 (was +0x3B8) */
                        store_32bit_word(m->cpu, rom_va + 0x3CC,
                            0x00000000); /* nop (was +0x3BC) */
                        /* beqz loop: was `beqz v0, 0x3A0` from +0x3C0.
                           Now relocated to +0x3D0, target is +0x3B0.
                           offset = (0x3B0 - 0x3D4) / 4 = -0x24/4 = -9
                           = 0xFFF7 */
                        /* NOP out BEQ loop, then J to NK.exe entry
                         * in unused ROM space.  MUST NOT overwrite
                         * 0x3DC+ (serial init, called via JALX from
                         * MIPS16 function library at 0x1232). */
                        store_32bit_word(m->cpu, rom_va + 0x3D0,
                            0x0BF008D8); /* j 0xBFC02360 */
                        store_32bit_word(m->cpu, rom_va + 0x3D4,
                            0x00000000); /* nop (delay slot) */
                        /* NK.exe jump trampoline at +0x2360 (after
                         * gen_handler which ends at +0x235C) */
                        store_32bit_word(m->cpu, rom_va + 0x2360,
                            0x3C08A006); /* lui $t0, 0xA006 */
                        store_32bit_word(m->cpu, rom_va + 0x2364,
                            0x25080004); /* addiu $t0, 0x0004 */
                        store_32bit_word(m->cpu, rom_va + 0x2368,
                            0x01000008); /* jr $t0 (→ 0xA0060004) */
                        store_32bit_word(m->cpu, rom_va + 0x236C,
                            0x00000000); /* nop */
                        /* Error recovery path: */
                        store_32bit_word(m->cpu, rom_va + 0x3EC,
                            0x0FF00126); /* jal 0xFC00498 (was +0x3DC) */
                        store_32bit_word(m->cpu, rom_va + 0x3F0,
                            0x24040004); /* li $a0, 4 (was +0x3E0) */
                        store_32bit_word(m->cpu, rom_va + 0x3F4,
                            0x0BF000E8); /* j 0xFC003A0 → +0x3B0 relocated */
                        store_32bit_word(m->cpu, rom_va + 0x3F8,
                            0x00000000); /* nop */

                        /* Patch delay loop SUB→SUBU at +0x51C/+0x520.
                         * The delay function uses SUB (traps on overflow)
                         * to compute elapsed Count ticks. In the emulator,
                         * Count can wrap past 0x80000000, causing signed
                         * overflow. SUBU is safe (same arithmetic, no trap). */
                        store_32bit_word(m->cpu, rom_va + 0x51C,
                            0x00C53023); /* subu a2,a2,a1 (was sub) */
                        store_32bit_word(m->cpu, rom_va + 0x520,
                            0x00461023); /* subu v0,v0,a2 (was sub) */

                        fprintf(stderr, "[BE300] Patched ROM BEV"
                            " vectors: TLB@+0x200, GenExc@+0x2300"
                            " (stub@+0x280),"
                            " boot code relocated +0x384→+0x394\n");
                    }
                } else {
                    fprintf(stderr, "[BE300] ROM file truncated"
                        " (%zu bytes), using ERET stubs\n", rom_read);
                    goto eret_stubs;
                }
            } else {
eret_stubs:
                ;
                uint32_t eret = 0x42000018u;
                static const uint32_t vec_offsets[] = {
                    0x000, 0x080, 0x100, 0x180,
                    0x200, 0x280, 0x300, 0x380
                };
                for (unsigned i = 0; i < sizeof(vec_offsets)/sizeof(vec_offsets[0]); i++) {
                    uint64_t va = 0xffffffff80000000ULL |
                                  (0x1FC00000ULL + vec_offsets[i]);
                    store_32bit_word(m->cpu, va, eret);
                }
                fprintf(stderr, "[BE300] Boot ROM not found,"
                    " using ERET stubs at PA 0x1FC00000\n");
            }
        }

        /*
         * Pre-load TLB with identity-mapped entries covering the
         * full 16MB SDRAM at both kseg2 (0xC0000000) and kuseg
         * (0x00000000).  WinCE uses kseg2 for kernel data and kuseg
         * for user-mode addresses.  Without TLB entries, every
         * access to these ranges causes a TLB refill exception which
         * sets EXL=1 permanently (no proper refill handler is
         * installed during the warm resume init path).
         *
         * Use 256KB pages: 32 entries × 2 pages × 256KB = 16MB.
         * Wire all entries to prevent TLBWR from evicting them.
         */
        /*
         * Install a minimal TLB refill handler at 0x80000000.
         * Identity maps VA → PA (PA = VA & 0x1FFFFFFF) using
         * 4KB pages.  This covers TLB misses during WinCE init
         * before the kernel installs its own handlers.
         *
         * MIPS code:
         *   mfc0 $k1, EntryHi    # get faulting VPN2
         *   srl  $k0, $k1, 7     # PFN in EntryLo position
         *   ori  $k0, $k0, 0x1F  # V=1,D=1,C=3,G=1
         *   mtc0 $k0, EntryLo0   # even page
         *   addiu $k1, $k0, 0x40 # odd page
         *   mtc0 $k1, EntryLo1
         *   tlbwr                # write TLB
         *   eret                 # return
         */
        if (!cfg->wince_cold_boot) {
            static const uint32_t tlb_handler[] = {
                0x401B5000,  /* mfc0 $k1, EntryHi    */
                0x001BD1C2,  /* srl  $k0, $k1, 7     */
                0x375A001F,  /* ori  $k0, $k0, 0x1F  */
                0x409A1000,  /* mtc0 $k0, EntryLo0   */
                0x275B0040,  /* addiu $k1, $k0, 0x40 */
                0x409B1800,  /* mtc0 $k1, EntryLo1   */
                0x42000006,  /* tlbwr                 */
                0x42000018,  /* eret                  */
            };
            wince_boot_install_synthetic_low_vectors(m, tlb_handler,
                sizeof(tlb_handler) / sizeof(tlb_handler[0]),
                "nand-setup");
            fprintf(stderr, "[BE300] Installed TLB refill handler"
                " at 0x80000000 + 0x80000180 (identity map)\n");
        } else {
            fprintf(stderr, "[BE300] Cold boot: skipping synthetic"
                " TLB handler (kernel installs its own)\n");
        }

        if (!cfg->wince_cold_boot) {
            wince_boot_apply_initial_seed(m);
        } else {
            /*
             * Seed the hibernate signature and flags so NK.exe's
             * pre-WAIT init takes the state-save path.  On real
             * hardware the ROM dispatcher populates these before
             * NK.exe runs; without them resume_ctx stays zeroed
             * and post-WAIT JR $ra crashes at address 0.
             */
            store_32bit_word(m->cpu, 0xffffffffa0002400ULL,
                UINT32_C(0x03020100));
            store_32bit_word(m->cpu, 0xffffffffa0002524ULL,
                UINT32_C(0x32100000));
            store_32bit_word(m->cpu, 0xffffffffa000254cULL,
                UINT32_C(0x00000003));
            fprintf(stderr, "[BE300] Cold boot: seeded hibernate"
                " state (PA 0x2400=0x03020100,"
                " PA 0x2524=0x32100000,"
                " PA 0x254C=0x00000003)\n");
        }

        /* Register VRC4173 latch (catch-all); pre-split to leave gaps for input device */
        extern void be300_register_vrc4173_latch(struct machine *, bool);
        be300_register_vrc4173_latch(gxm, cfg->log_mmio);

        /* Register NAND flash; pre-split to leave gap at 0x0A00A040 for input device */
        extern void be300_register_nand(struct machine *, nand_state_t *, bool);
        be300_register_nand(gxm, &m->nand, cfg->log_mmio);

        /* Register input devices AFTER latch/NAND (fills pre-carved gaps) */
        extern void be300_register_input(struct machine *, machine_t *, bool);
        be300_register_input(gxm, m, cfg->log_mmio);
        m->input_registered = true;
        wince_boot_note_spl_handoff(m);

    } else if (cfg->rom_path) {
        if (loader_load_rom(m, cfg->rom_path) != 0) {
            fprintf(stderr, "[BE300] Failed to load ROM image\n");
            free(m);
            return NULL;
        }
        m->boot_mode = BE300_BOOT_ROM;
    }

    wince_boot_attach_machine(m);

    fprintf(stderr, "[BE300] Machine created: %u MB SDRAM, VR4131 CPU\n",
            cfg->sdram_size / (1024 * 1024));
    return m;
}


static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void be300_runtime_start(machine_t *m)
{
    struct machine *gxm = m->gxe_machine;
    struct emul *emul = m->emul;

    if (m->runtime_initialized)
        return;

    fprintf(stderr, "[BE300] Starting emulation at PC=0x%08" PRIx64 "\n",
            m->cpu->pc);

    cpu_run_init(gxm);
    m->cpu->running = true;

    if (m->web_mode)
        timer_reset_state();
    else
        timer_start();
    console_init_main(emul);

    if (m->use_builtin_ui)
        ui_init(m);

    if (m->cfg.target_mhz > 0) {
        if (m->web_mode)
            fprintf(stderr, "[BE300] Web pacing target: %u MHz-equivalent\n",
                    m->cfg.target_mhz);
        else
            fprintf(stderr, "[BE300] Throttle: targeting %u MHz\n",
                    m->cfg.target_mhz);
    } else {
        fprintf(stderr, "[BE300] Throttle: disabled (unthrottled)\n");
    }

    m->throttle_target_ips = (uint64_t)m->cfg.target_mhz * 1000000ULL;
    m->throttle_wall_origin = 0;
    m->throttle_instr_origin = 0;
    m->last_report = 0;
    m->loop_count = 0;
    m->runtime_initialized = true;
    m->runtime_stopped = false;
    m->runtime_finalized = false;
    emul_executing = true;

    signal(SIGTERM, be300_handle_stop_signal);
    if (!m->web_mode)
        signal(SIGINT, be300_handle_stop_signal);

    host_io_set_serial_sink(be300_serial_sink, m);
    host_io_set_stdout_enabled(m->mirror_serial_to_stdout);
}

static void be300_runtime_finalize(machine_t *m)
{
    if (!m || !m->runtime_initialized || m->runtime_finalized)
        return;

    if (m->save_exit_screenshot)
        ui_save_screenshot(m);

    if (m->use_builtin_ui)
        ui_destroy(m);

    timer_stop();
    host_io_set_serial_sink(NULL, NULL);
    host_io_set_stdout_enabled(true);

    emul_executing = false;
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    cpu_run_deinit(m->gxe_machine);
    m->runtime_stopped = true;
    m->runtime_finalized = true;

    fprintf(stderr, "[BE300] Emulation stopped after %" PRIi64 " instructions\n",
            m->cpu->ninstrs);
}

static bool be300_run_batch(machine_t *m)
{
    struct machine *gxm = m->gxe_machine;

    __sync_synchronize();
    if (emul_shutdown) {
        fprintf(stderr, "[BE300] Loop exit: emul_shutdown is true\n");
        return false;
    }

    if (m->loop_count % 1000 == 0) {
        fprintf(stderr, "[BE300] Loop batch %d, PC=0x%08" PRIx64 "\n",
                m->loop_count, m->cpu->pc);
    }

    /* Detect user-mode execution (PC in kuseg: 0x00000000-0x7FFFFFFF) */
    {
        static int usermode_logged = 0;
        uint64_t pc = m->cpu->pc;
        if (!usermode_logged && pc < 0x80000000ULL && pc > 0x1000ULL) {
            fprintf(stderr, "[BE300] *** USER MODE DETECTED: PC=0x%08" PRIx64 " ***\n", pc);
            usermode_logged = 1;
        }
    }

    /*
     * Detect NK.exe entry and process ROMHDR COPYentry BEFORE the
     * kernel's startup code runs.  The SPL decompresses NK.exe to
     * PA 0x60000 and jumps to 0xA0060004 which redirects to
     * 0x80076B50.  The COPYentry copies the initialized data section
     * to PA 0x660000.  Without this, the kernel startup runs with
     * zeroed globals and silently fails to register OEMInit callbacks.
     */
    if (m->cfg.wince_cold_boot && !m->wince.cold_boot_copy_done) {
        uint32_t pc = (uint32_t)m->cpu->pc;
        uint32_t pa = pc & 0x1FFFFFFFu;
        /* Detect NK.exe executing: PC in PA 0x60000-0x100000 range.
         * SPL runs at PA 0xF00000+, so this catches the transition. */
        if (pa >= 0x60000u && pa < 0x100000u) {
            unsigned char buf[4];
            uint32_t ptoc = 0;

            if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                    0xffffffff80060044ULL, buf, 4,
                    MEM_READ, CACHE_DATA)) {
                ptoc = buf[0] | (buf[1]<<8) |
                       (buf[2]<<16) | (buf[3]<<24);
            }

            if (ptoc != 0) {
                uint64_t ptoc_va = 0xffffffff00000000ULL |
                                   (uint64_t)(int32_t)ptoc;
                uint32_t copy_entries, copy_offset;

                m->cpu->memory_rw(m->cpu, m->cpu->mem,
                    ptoc_va + 32, buf, 4, MEM_READ, CACHE_DATA);
                copy_entries = buf[0] | (buf[1]<<8) |
                               (buf[2]<<16) | (buf[3]<<24);
                m->cpu->memory_rw(m->cpu, m->cpu->mem,
                    ptoc_va + 36, buf, 4, MEM_READ, CACHE_DATA);
                copy_offset = buf[0] | (buf[1]<<8) |
                              (buf[2]<<16) | (buf[3]<<24);

                for (uint32_t ci = 0; ci < copy_entries && ci < 64;
                     ci++) {
                    uint64_t entry_va =
                        0xffffffff00000000ULL |
                        (uint64_t)(int32_t)(copy_offset + ci * 16);
                    uint32_t fields[4];
                    for (int fi = 0; fi < 4; fi++) {
                        m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            entry_va + fi * 4, buf, 4,
                            MEM_READ, CACHE_DATA);
                        fields[fi] = buf[0] | (buf[1]<<8) |
                                     (buf[2]<<16) | (buf[3]<<24);
                    }
                    uint32_t src = fields[0], dst = fields[1];
                    uint32_t cpylen = fields[2], dstlen = fields[3];
                    uint64_t src_va = 0xffffffff00000000ULL |
                                      (uint64_t)(int32_t)src;
                    uint64_t dst_va = 0xffffffff00000000ULL |
                                      (uint64_t)(int32_t)dst;

                    for (uint32_t off = 0; off < cpylen; off += 4) {
                        uint8_t tmp[4] = {0};
                        uint32_t chunk = cpylen - off;
                        if (chunk > 4) chunk = 4;
                        m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            src_va + off, tmp, chunk,
                            MEM_READ, CACHE_DATA);
                        uint32_t w = tmp[0] | (tmp[1]<<8) |
                                     (tmp[2]<<16) | (tmp[3]<<24);
                        store_32bit_word(m->cpu, dst_va + off, w);
                    }
                    for (uint32_t off = cpylen; off < dstlen;
                         off += 4)
                        store_32bit_word(m->cpu, dst_va + off, 0);

                    fprintf(stderr,
                        "[COLD_BOOT] Early COPYentry[%u]:"
                        " src=0x%08X dst=0x%08X"
                        " copy=%u total=%u\n",
                        ci, src, dst, cpylen, dstlen);
                }
                m->wince.cold_boot_copy_done = true;
            }
        }
    }

    if (!machine_run(gxm)) {
        wince_boot_note_fatal_stop(m, "machine-no-longer-running");
        fprintf(stderr, "[BE300] Loop exit: machine no longer running"
            " (mips16=%d halted=%d PC=0x%08X)\n",
            m->cpu->cd.mips.mips16,
            m->cpu->is_halted,
            (uint32_t)m->cpu->pc);

        /* Dump full CPU state at crash point */
        if (m->cfg.wince_cold_boot) {
            uint32_t pc = (uint32_t)m->cpu->pc;
            fprintf(stderr, "[COLD_CRASH] PC=0x%08X\n", pc);
            fprintf(stderr, "[COLD_CRASH] CP0: Status=0x%08X"
                " Cause=0x%08X EPC=0x%08X BadVA=0x%08X\n",
                (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS],
                (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE],
                (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC],
                (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR]);
            fprintf(stderr, "[COLD_CRASH] GPR:"
                " at=%08X v0=%08X v1=%08X a0=%08X\n",
                (uint32_t)m->cpu->cd.mips.gpr[1],
                (uint32_t)m->cpu->cd.mips.gpr[2],
                (uint32_t)m->cpu->cd.mips.gpr[3],
                (uint32_t)m->cpu->cd.mips.gpr[4]);
            fprintf(stderr, "[COLD_CRASH] GPR:"
                " t0=%08X t1=%08X t2=%08X t3=%08X\n",
                (uint32_t)m->cpu->cd.mips.gpr[8],
                (uint32_t)m->cpu->cd.mips.gpr[9],
                (uint32_t)m->cpu->cd.mips.gpr[10],
                (uint32_t)m->cpu->cd.mips.gpr[11]);
            fprintf(stderr, "[COLD_CRASH] GPR:"
                " sp=%08X fp=%08X ra=%08X gp=%08X\n",
                (uint32_t)m->cpu->cd.mips.gpr[29],
                (uint32_t)m->cpu->cd.mips.gpr[30],
                (uint32_t)m->cpu->cd.mips.gpr[31],
                (uint32_t)m->cpu->cd.mips.gpr[28]);
            fprintf(stderr, "[COLD_CRASH] GPR:"
                " s0=%08X s1=%08X s2=%08X s3=%08X"
                " s4=%08X s5=%08X s6=%08X s7=%08X\n",
                (uint32_t)m->cpu->cd.mips.gpr[16],
                (uint32_t)m->cpu->cd.mips.gpr[17],
                (uint32_t)m->cpu->cd.mips.gpr[18],
                (uint32_t)m->cpu->cd.mips.gpr[19],
                (uint32_t)m->cpu->cd.mips.gpr[20],
                (uint32_t)m->cpu->cd.mips.gpr[21],
                (uint32_t)m->cpu->cd.mips.gpr[22],
                (uint32_t)m->cpu->cd.mips.gpr[23]);

            /* Dump instructions around crash PC */
            fprintf(stderr, "[COLD_CRASH] Code around PC:\n");
            for (int i = -4; i < 8; i++) {
                unsigned char buf[4];
                uint64_t va = (uint64_t)(int32_t)pc + i * 4;
                if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                        va, buf, 4, MEM_READ, CACHE_DATA)) {
                    uint32_t w = buf[0] | (buf[1]<<8) |
                                 (buf[2]<<16) | (buf[3]<<24);
                    fprintf(stderr, "[COLD_CRASH]   0x%08" PRIx64
                        ": %08X%s\n", va, w,
                        (int64_t)va == (int64_t)(int32_t)pc
                            ? "  <-- PC" : "");
                }
            }
        }

        return false;
    }

    if (m->web_mode)
        timer_tick_manual();

    if (m->loop_count % 1000 == 0) {
        fprintf(stderr, "[BE300] Loop batch %d done\n", m->loop_count);
    }

    if (!m->web_mode && m->throttle_target_ips > 0) {
        uint64_t now_ns = monotonic_ns();
        uint64_t instrs  = (uint64_t)m->cpu->ninstrs;

        if (m->throttle_wall_origin == 0) {
            m->throttle_wall_origin = now_ns;
            m->throttle_instr_origin = instrs;
        } else {
            uint64_t elapsed_instrs = instrs - m->throttle_instr_origin;
            uint64_t target_ns = (elapsed_instrs * 1000000000ULL)
                                 / m->throttle_target_ips;
            uint64_t actual_ns = now_ns - m->throttle_wall_origin;

            if (target_ns > actual_ns) {
                uint64_t sleep_ns = target_ns - actual_ns;
                struct timespec ts;

                if (sleep_ns > 50000000ULL)
                    sleep_ns = 50000000ULL;

                ts.tv_sec = (time_t)(sleep_ns / 1000000000ULL);
                ts.tv_nsec = (long)(sleep_ns % 1000000000ULL);
                nanosleep(&ts, NULL);
            }

            if (elapsed_instrs >= 1000000ULL) {
                m->throttle_wall_origin = monotonic_ns();
                m->throttle_instr_origin = instrs;
            }
        }
    }

    console_flush();
    if (m->use_builtin_ui)
        ui_update(m);

    if (m->use_builtin_ui && ui_should_quit(m)) {
        fprintf(stderr, "[BE300] Loop exit: ui_should_quit is true\n");
        return false;
    }

    wince_boot_note_loop_observation(m);
    be300_log_cold_boot_late_loop(m);
    be300_maybe_restore_cold_boot_oal_block(m, "loop");

    if (m->cpu->ninstrs - m->last_report >= 50000000LL) {
        uint64_t *cp0 = m->cpu->cd.mips.coproc[0]->reg;
        fprintf(stderr, "[BE300] Progress: %" PRIi64 "M instrs,"
            " PC=0x%08" PRIx64
            " Status=0x%08X Cause=0x%08X EPC=0x%08X"
            " BadVA=0x%08X SP=0x%08X\n",
            m->cpu->ninstrs / 1000000LL, m->cpu->pc,
            (uint32_t)cp0[COP0_STATUS],
            (uint32_t)cp0[COP0_CAUSE],
            (uint32_t)cp0[COP0_EPC],
            (uint32_t)cp0[COP0_BADVADDR],
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP]);
        m->last_report = m->cpu->ninstrs;
    }

    /* Cold boot PC probes: one-shot logging and patches */
    if (m->cfg.wince_cold_boot && m->wince.cold_boot_wait_logged) {
        uint32_t pc32 = (uint32_t)m->cpu->pc;

        static const struct { uint32_t addr; uint32_t bit; const char *name; } probes[] = {
            { 0x8007B5F4, 0x0001, "install_exception_handlers" },
            { 0x800947C8, 0x0002, "kernel_init(pTOC)" },
            { 0x8007AB38, 0x0004, "OEMInit_callback_dispatcher" },
            { 0x80078BC0, 0x0008, "OAL_vtable_init" },
            { 0x80096894, 0x0010, "scheduler_dispatch" },
            { 0x800794C8, 0x0020, "OAL_init_entry" },
            { 0x800A5C78, 0x0040, "A5C78_OAL_callbacks" },
            { 0x800942B4, 0x0080, "942B4_real_kernel_init" },
            { 0x800964FC, 0x0100, "964FC_scheduler_init" },
            { 0x800AB990, 0x0200, "AB990_jalr_func_ptr" },
            { 0x80078D74, 0x0400, "78D74_init" },
            { 0x8007A65C, 0x0800, "7A65C_timer_setup" },
            { 0x800A78C8, 0x1000, "A78C8_timer_calc" },
            { 0x800A8934, 0x2000, "A8934_ISR_registration" },
            { 0x800A5E70, 0x4000, "A5E70_ICU_programming" },
            { 0x800A5FEC, 0x8000, "A5FEC_VRC4173_setup" },
            { 0x800A6090, 0x10000, "A6090_VRC4173_setup2" },
        };
        for (unsigned i = 0; i < sizeof(probes)/sizeof(probes[0]); i++) {
            if (pc32 == probes[i].addr &&
                !(m->wince.cold_boot_pc_probes_logged & probes[i].bit)) {
                m->wince.cold_boot_pc_probes_logged |= probes[i].bit;
                fprintf(stderr,
                    "[COLD_BOOT_PROBE] Hit %s at PC=0x%08X"
                    " (%" PRIi64 "M instrs, SP=0x%08X"
                    " RA=0x%08X A0=0x%08X)\n",
                    probes[i].name, pc32,
                    m->cpu->ninstrs / 1000000LL,
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A0]);

                /* When OAL_init_entry is hit, dump caller context */
                if (pc32 == 0x800794C8u) {
                    uint32_t sp = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP];
                    fprintf(stderr,
                        "[COLD_BOOT_PROBE] OAL_init caller:"
                        " RA=0x%08X SP=0x%08X"
                        " S0=0x%08X S1=0x%08X\n",
                        (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                        sp,
                        (uint32_t)m->cpu->cd.mips.gpr[16],
                        (uint32_t)m->cpu->cd.mips.gpr[17]);
                    /* Dump top 8 stack words */
                    fprintf(stderr,
                        "[COLD_BOOT_PROBE] Stack dump:");
                    for (int si = 0; si < 8; si++) {
                        unsigned char sb[4];
                        uint64_t sva = 0xffffffff00000000ULL
                            | (uint64_t)(int32_t)(sp + si * 4);
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                sva, sb, 4, MEM_READ, CACHE_DATA)) {
                            uint32_t sw = sb[0]|(sb[1]<<8)
                                |(sb[2]<<16)|(sb[3]<<24);
                            fprintf(stderr, " %08X", sw);
                        }
                    }
                    fprintf(stderr, "\n");
                }
            }
        }
    }

    /*
     * Intercept execution in the OAL init continuation block
     * (0x800794C0-0x8007959C) during cold-start.  This block
     * does PMU/DCU setup and ends in WAIT.  Memory patches may
     * not take effect due to dyntrans caching — intercept at
     * the run-loop level instead.
     */
    /*
     * Intercept execution in the OAL init / post-WAIT block
     * (0x80079480-0x800797E0) during cold-start.
     *
     * This block does: HW init calls → cache flush → PMU/DCU
     * setup → WAIT → NOP sled → checks → warm-path init →
     * GPR/CP0 restore from resume_ctx → JR $ra.
     *
     * There are multiple entry points (0x79480, 0x794C0,
     * 0x794C8, 0x79510) and internal JALs that keep RA
     * pointing within the block.  Memory patches via
     * store_32bit_word may not take effect due to dyntrans
     * caching.  Intercept at the run-loop level instead.
     *
     * Walk the stack to find a return address outside this
     * block and redirect there.
     */
    /*
     * Intercept execution in the OAL init / post-WAIT block
     * (0x80079480-0x800797E0) during cold-start init phase.
     *
     * Pragmatic cold boot cannot run the ROM's MIPS16 dispatcher, so
     * entering this block early is a dead-end. Resume at the true
     * cold-start continuation (0x8007B57C), then restore the original
     * OAL block later once kernel init has populated its core state.
     */
    if (m->cfg.wince_cold_boot && m->wince.cold_boot_wait_logged
        && m->wince.cold_boot_wait_count < 50
        && !g_cold_boot_oal_block_restored) {
        uint32_t pc32 = (uint32_t)m->cpu->pc;
        if (pc32 >= 0x80079480u && pc32 <= 0x800797E0u) {
            uint32_t sp = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP];
            uint32_t resume_pc = 0;
            uint32_t resume_slot = 0;
            bool found_resume_pc = be300_find_stack_resume_pc(m, sp,
                &resume_pc, &resume_slot);

            if (!m->wince.cold_boot_oal_intercept_logged) {
                m->wince.cold_boot_oal_intercept_logged = true;
                fprintf(stderr,
                    "[COLD_BOOT] OAL init intercept:"
                    " PC=0x%08X RA=0x%08X SP=0x%08X",
                    pc32,
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                    sp);
                fprintf(stderr, " stack:");
                for (int si = 0; si < 8; si++) {
                    unsigned char sb[4];
                    uint64_t sva = 0xffffffff00000000ULL
                        | (uint64_t)(int32_t)(sp + si * 4);
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            sva, sb, 4, MEM_READ, CACHE_DATA)) {
                        uint32_t sw = sb[0]|(sb[1]<<8)
                            |(sb[2]<<16)|(sb[3]<<24);
                        fprintf(stderr, " %08X", sw);
                    }
                }
                fprintf(stderr, "\n");
                if (found_resume_pc) {
                    fprintf(stderr,
                        "[COLD_BOOT] OAL intercept stack resume"
                        " candidate=0x%08X from [0x%08X]\n",
                        resume_pc, resume_slot);
                } else {
                    fprintf(stderr,
                        "[COLD_BOOT] OAL intercept stack resume"
                        " candidate not found, fallback=0x8007B57C\n");
                }
            }

            {
                int tlb_index = -1;
                uint32_t tlb_hi = 0;
                uint32_t tlb_lo0 = 0;
                uint32_t tlb_lo1 = 0;
                uint32_t tlb_mask = 0;
                uint64_t tlb_pa = 0;
                uint64_t live_pa = 0;
                uint64_t alias_pa = 0;
                bool tlb_valid = false;
                bool tlb_dirty = false;
                bool tlb_global = false;
                bool tlb_odd = false;
                const char *tlb_mode = NULL;

                if (found_resume_pc) {
                    m->cpu->pc = (int64_t)(int32_t)resume_pc;
                } else {
                    m->cpu->cd.mips.gpr[MIPS_GPR_SP] =
                        (int32_t)UINT32_C(0xFFFFD7E0);
                    m->cpu->pc =
                        (int64_t)(int32_t)UINT32_C(0x8007B57C);
                }
                if (!(m->wince.cold_boot_pc_probes_logged
                        & COLD_BOOT_PROBE_STACK_TLB_LOGGED)) {
                    m->wince.cold_boot_pc_probes_logged |=
                        COLD_BOOT_PROBE_STACK_TLB_LOGGED;
                    if (be300_find_tlb_match(m, UINT32_C(0xFFFFD000),
                            &tlb_index, &tlb_hi, &tlb_lo0, &tlb_lo1,
                            &tlb_mask, &tlb_pa, &tlb_valid, &tlb_dirty,
                            &tlb_global, &tlb_odd, &tlb_mode)
                        && tlb_valid
                        && be300_translate_va(m, UINT32_C(0xFFFFD000),
                            &live_pa)) {
                        m->wince.cold_boot_pc_probes_logged |=
                            COLD_BOOT_PROBE_STACK_TLB_VALID;
                        fprintf(stderr,
                            "[COLD_BOOT] Verified stack TLB[%u]"
                            " for VA 0xFFFFD000:"
                            " EntryHi=%08X EntryLo0=%08X"
                            " EntryLo1=%08X PageMask=%08X"
                            " mode=%s odd=%d dirty=%d global=%d"
                            " tlb_pa=0x%08" PRIx64
                            " live_pa=0x%08" PRIx64 "\n",
                            (unsigned)tlb_index,
                            tlb_hi,
                            tlb_lo0,
                            tlb_lo1,
                            tlb_mask,
                            tlb_mode ? tlb_mode : "-",
                            tlb_odd ? 1 : 0,
                            tlb_dirty ? 1 : 0,
                            tlb_global ? 1 : 0,
                            tlb_pa,
                            live_pa);
                    } else {
                        fprintf(stderr,
                            "[COLD_BOOT] Missing live stack TLB mapping"
                            " for VA 0xFFFFD000 after intercept"
                            " (match=%d valid=%d translate=%d)\n",
                            tlb_index >= 0 ? 1 : 0,
                            tlb_valid ? 1 : 0,
                            be300_translate_va(m, UINT32_C(0xFFFFD000),
                                NULL) ? 1 : 0);
                    }

                    if ((m->wince.cold_boot_pc_probes_logged
                            & COLD_BOOT_PROBE_STACK_TLB_VALID)
                        && !be300_translate_va(m, UINT32_C(0xFFFFD888),
                            &alias_pa)
                        && !(m->wince.cold_boot_pc_probes_logged
                            & COLD_BOOT_PROBE_D8XX_HELPER_INSTALLED)) {
                        be300_install_cold_boot_d8xx_helper(m,
                            UINT32_C(0xFFFFD888));
                    }
                }
            }
            m->cpu->is_halted = false;
        }
    }

    if (m->cfg.wince_cold_boot && m->wince.cold_boot_wait_logged) {
        uint64_t *cp0 = m->cpu->cd.mips.coproc[0]->reg;
        uint32_t badva = (uint32_t)cp0[COP0_BADVADDR];
        uint32_t badva_window = badva & UINT32_C(0xFFFFFFC0);

        if (badva_window == UINT32_C(0xFFFFD880)
            || badva_window == UINT32_C(0xFFFFD8C0)) {
            if (!(m->wince.cold_boot_pc_probes_logged
                    & COLD_BOOT_PROBE_D8XX_FAULT_LOGGED)) {
                m->wince.cold_boot_pc_probes_logged |=
                    COLD_BOOT_PROBE_D8XX_FAULT_LOGGED;
                fprintf(stderr,
                    "[COLD_BOOT] First D8xx alias fault:"
                    " PC=0x%08" PRIx64 " EPC=0x%08X"
                    " BadVA=0x%08X Status=0x%08X Cause=0x%08X"
                    " SP=0x%08X stack_tlb_valid=%d\n",
                    m->cpu->pc,
                    (uint32_t)cp0[COP0_EPC],
                    badva,
                    (uint32_t)cp0[COP0_STATUS],
                    (uint32_t)cp0[COP0_CAUSE],
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                    (m->wince.cold_boot_pc_probes_logged
                        & COLD_BOOT_PROBE_STACK_TLB_VALID) ? 1 : 0);
            }
            if ((m->wince.cold_boot_pc_probes_logged
                    & COLD_BOOT_PROBE_STACK_TLB_VALID)
                && !(m->wince.cold_boot_pc_probes_logged
                    & COLD_BOOT_PROBE_D8XX_HELPER_INSTALLED)) {
                be300_install_cold_boot_d8xx_helper(m, badva);
            }
        }
    }

    if (m->cpu->is_halted) {
        if (m->cfg.wince_cold_boot &&
            m->wince.cold_boot_wait_logged &&
            m->wince.cold_boot_wait_count < 3) {
            fprintf(stderr,
                "[COLD_BOOT_HALT] PC=0x%08X norm=0x%08X"
                " SP=0x%08X RA=0x%08X"
                " wait_logged=%d copy_done=%d count=%u\n",
                (uint32_t)m->cpu->pc,
                (uint32_t)m->cpu->pc & 0x1FFFFFFFu,
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                m->wince.cold_boot_wait_logged,
                m->wince.cold_boot_copy_done,
                m->wince.cold_boot_wait_count);
        }
        if (m->cfg.wince_cold_boot) {
            uint32_t wait_pc = (uint32_t)m->cpu->pc;
            uint32_t norm = wait_pc & 0x1FFFFFFFu;

            if (!m->wince.cold_boot_wait_logged &&
                norm >= 0x00060000u && norm < 0x00100000u) {
                /* NK.exe WAIT (PA in 0x60000-0x100000 range).
                 * Ignore SPL WAITs (PA 0xF00000+) — let the
                 * timer wake the SPL normally. */
                m->cpu->is_halted = false;
                m->cpu->pc += 4;
                m->wince.cold_boot_wait_logged = true;
                m->nand.wince_mode = true;
                fprintf(stderr,
                    "[COLD_BOOT] WAIT at PC=0x%08X (PA=0x%08X),"
                    " skipping to 0x%08" PRIx64 "\n",
                    wait_pc, norm, m->cpu->pc);

                /*
                 * Process WinCE ROMHDR COPYentry table.
                 *
                 * The decompressed NK.exe has an "ECEC" signature at
                 * offset 0x40 followed by a pTOC pointer.  The ROMHDR
                 * at pTOC contains ulCopyEntries/ulCopyOffset that
                 * describe data sections to copy from within the image
                 * to their runtime addresses.  On real hardware, the
                 * ROM's MIPS16 function at 0x9FC00C85 does this copy.
                 * We skip the ROM boot code, so we do it here.
                 *
                 * Without this copy, the kernel's initialized data
                 * section (globals, vtables) at PA 0x660000 is all
                 * zeros, causing the kernel startup to silently fail.
                 */
                {
                    unsigned char buf[4];
                    uint64_t ptoc_va;
                    uint32_t ptoc, copy_entries, copy_offset;

                    /* Read pTOC from NK.exe offset 0x44 */
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            0xffffffff80060044ULL, buf, 4,
                            MEM_READ, CACHE_DATA)) {
                        ptoc = buf[0] | (buf[1]<<8) |
                               (buf[2]<<16) | (buf[3]<<24);
                    } else {
                        ptoc = 0;
                    }

                    if (ptoc != 0) {
                        ptoc_va = 0xffffffff00000000ULL |
                                  (uint64_t)(int32_t)ptoc;

                        /* Read ulCopyEntries (offset 32) and
                           ulCopyOffset (offset 36) from ROMHDR */
                        m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            ptoc_va + 32, buf, 4,
                            MEM_READ, CACHE_DATA);
                        copy_entries = buf[0] | (buf[1]<<8) |
                                       (buf[2]<<16) | (buf[3]<<24);

                        m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            ptoc_va + 36, buf, 4,
                            MEM_READ, CACHE_DATA);
                        copy_offset = buf[0] | (buf[1]<<8) |
                                      (buf[2]<<16) | (buf[3]<<24);

                        fprintf(stderr, "[COLD_BOOT] ROMHDR pTOC"
                            "=0x%08X copyEntries=%u copyOffset"
                            "=0x%08X\n",
                            ptoc, copy_entries, copy_offset);

                        /* Process each COPYentry (16 bytes each) */
                        for (uint32_t ci = 0; ci < copy_entries && ci < 64;
                             ci++) {
                            uint64_t entry_va =
                                0xffffffff00000000ULL |
                                (uint64_t)(int32_t)(copy_offset + ci * 16);
                            uint32_t fields[4];
                            for (int fi = 0; fi < 4; fi++) {
                                m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                    entry_va + fi * 4, buf, 4,
                                    MEM_READ, CACHE_DATA);
                                fields[fi] = buf[0] | (buf[1]<<8) |
                                             (buf[2]<<16) | (buf[3]<<24);
                            }
                            uint32_t src = fields[0];
                            uint32_t dst = fields[1];
                            uint32_t cpylen = fields[2];
                            uint32_t dstlen = fields[3];

                            fprintf(stderr,
                                "[COLD_BOOT] COPYentry[%u]:"
                                " src=0x%08X dst=0x%08X"
                                " copy=%u total=%u\n",
                                ci, src, dst, cpylen, dstlen);

                            /* Copy data from src to dst */
                            uint64_t src_va = 0xffffffff00000000ULL |
                                              (uint64_t)(int32_t)src;
                            uint64_t dst_va = 0xffffffff00000000ULL |
                                              (uint64_t)(int32_t)dst;
                            for (uint32_t off = 0; off < cpylen;
                                 off += 4) {
                                uint32_t chunk = cpylen - off;
                                if (chunk > 4) chunk = 4;
                                uint8_t tmp[4] = {0};
                                m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                    src_va + off, tmp, chunk,
                                    MEM_READ, CACHE_DATA);
                                uint32_t w = tmp[0] | (tmp[1]<<8) |
                                             (tmp[2]<<16) | (tmp[3]<<24);
                                store_32bit_word(m->cpu,
                                    dst_va + off, w);
                            }
                            /* Zero-fill remainder */
                            for (uint32_t off = cpylen; off < dstlen;
                                 off += 4) {
                                store_32bit_word(m->cpu,
                                    dst_va + off, 0);
                            }
                        }
                    }
                }

                /* Probe key values after COPYentry for diagnostics */
                {
                    unsigned char pb[4];
                    uint32_t func_ptr_84 = 0, pa_1d004 = 0;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            0xffffffff80660084ULL, pb, 4,
                            MEM_READ, CACHE_DATA))
                        func_ptr_84 = pb[0]|(pb[1]<<8)
                            |(pb[2]<<16)|(pb[3]<<24);
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            0xffffffffa001d004ULL, pb, 4,
                            MEM_READ, CACHE_DATA))
                        pa_1d004 = pb[0]|(pb[1]<<8)
                            |(pb[2]<<16)|(pb[3]<<24);
                    fprintf(stderr,
                        "[COLD_BOOT] Post-COPYentry probes:"
                        " [0x80660084]=0x%08X (JALR target)"
                        " PA 0x1D004=0x%08X (AB990 guard)\n",
                        func_ptr_84, pa_1d004);
                }

                /*
                 * Set up TLB entries that the ROM's MIPS32 init
                 * normally creates before calling the MIPS16
                 * dispatcher.  The dispatcher accesses kseg3
                 * virtual addresses that need TLB mappings:
                 *
                 *   VA 0xF2000000 → PA 0x0A000000 (VRC4173 I/O)
                 *   VA 0xFF100000 → PA 0x09100000 (on-chip SRAM)
                 */
                {
                    uint64_t *cp0 = m->cpu->cd.mips.coproc[0]->reg;

                    /* TLB 0: VA 0xFF100000 → PA 0x09100000 (stack) */
                    cp0[COP0_PAGEMASK] = 0x0000;  /* 4KB pages */
                    cp0[COP0_ENTRYHI]  = 0xFF100000ULL;
                    cp0[COP0_ENTRYLO0] = (0x09100000u >> 12) << 6 | 0x17;
                    cp0[COP0_ENTRYLO1] = (0x09101000u >> 12) << 6 | 0x17;
                    cp0[COP0_INDEX]    = 0;
                    coproc_tlbwri(m->cpu, 0);

                    /* TLB 1: VA 0xF2000000 → PA 0x0A000000 (VRC4173)
                     * Use 256KB pages to cover 0xF2000000-0xF207FFFF */
                    cp0[COP0_PAGEMASK] = 0x0007F800; /* 256KB pages (VR4131: shift=11) */
                    cp0[COP0_ENTRYHI]  = 0xF2000000ULL;
                    cp0[COP0_ENTRYLO0] = (0x0A000000u >> 12) << 6 | 0x17;
                    cp0[COP0_ENTRYLO1] = (0x0A040000u >> 12) << 6 | 0x17;
                    cp0[COP0_INDEX]    = 1;
                    coproc_tlbwri(m->cpu, 0);

                    cp0[COP0_WIRED]    = 2;
                    cp0[COP0_PAGEMASK] = 0;  /* reset */
                    fprintf(stderr,
                        "[COLD_BOOT] TLB: 0xFF100000→PA 0x09100000,"
                        " 0xF2000000→PA 0x0A000000\n");
                }

                /*
                 * Call the ROM's MIPS16 boot dispatcher at 0x9FC00C20.
                 * On real hardware, the ROM calls JALR 0x9FC00C21
                 * (bit 0 = MIPS16 mode) after the section copier.
                 * The dispatcher populates:
                 *   - OEMInit callback table at PA 0x51680
                 *   - resume_ctx at PA 0x2200
                 *   - Dispatch tables, bootctx stub
                 *
                 * Set RA to the kernel cold-start at 0x8007B398
                 * so when the dispatcher returns (JR $ra in MIPS32),
                 * the kernel initializes page tables, TLB, section
                 * table, exception handlers, loads modules, and
                 * starts the shell.
                 *
                 * Set SP to 0x80003800 (same as ROM boot uses).
                 */
                /*
                 * NOTE: TLB entries already set up above:
                 *   TLB 0: VA 0xFF100000 → PA 0x09100000 (8KB dev_ram, stack)
                 *   TLB 1: VA 0xF2000000 → PA 0x0A000000 (VRC4173 I/O)
                 * Do NOT overwrite TLB 0 here — earlier code at lines
                 * 1163-1188 already has the correct mapping.
                 */

                /*
                 * The MIPS16 dispatcher at 0x9FC00C20 populates the
                 * OEMInit callback table and resume_ctx on real HW.
                 * However, the dispatched functions need VRC4173
                 * register emulation we don't have yet — they return
                 * 0 from unimplemented registers, causing the dispatch
                 * loop to run forever (SP underflows into kuseg).
                 *
                 * For now, skip the dispatcher and jump directly to
                 * the kernel cold-start.  The callback table has been
                 * injected from warm-boot captures (wince_resume_
                 * replay_data.h).  Set up the same state the ROM boot
                 * continuation at +0x3D0 would produce.
                 */
                store_32bit_word(m->cpu,
                    0xffffffffa00024fcULL, 0xA0060004u);

                /*
                 * Fix version marker at PA 0x2400.
                 * The SPL writes 0x03020101 but NK.exe expects
                 * 0x03020100.  The version check at 0x76CBC clears
                 * PA 0x254C on mismatch, which prevents the state-
                 * save function from running.  Also re-seed the
                 * hibernate signature and flags since the NK.exe
                 * pre-WAIT init may have modified them.
                 */
                store_32bit_word(m->cpu,
                    0xffffffffa0002400ULL, 0x03020100u);
                store_32bit_word(m->cpu,
                    0xffffffffa0002524ULL, 0x32100000u);
                store_32bit_word(m->cpu,
                    0xffffffffa000254cULL, 0x00000003u);

                /* Inject ALL warm-boot replay regions.  The kernel
                 * needs not just the OEMInit callback table but also
                 * dispatch tables, bootctx stub, and stack frame data
                 * to fully initialize.  The cold-start at 0x8007B398
                 * zeros page tables and installs handlers first, then
                 * kernel_init uses these data structures. */
                {
                    const wince_resume_region_t *regions =
                        wince_resume_replay_regions;
                    unsigned nregions = sizeof(wince_resume_replay_regions)
                        / sizeof(wince_resume_replay_regions[0]);
                    for (unsigned r = 0; r < nregions; r++) {
                        const wince_resume_region_t *reg = &regions[r];
                        for (unsigned i = 0; i < reg->word_count; i++) {
                            if (reg->valid_words[i]) {
                                uint64_t va = 0xffffffff80000000ULL
                                    | reg->pa + i * 4;
                                store_32bit_word(m->cpu, va,
                                    reg->words[i]);
                            }
                        }
                        fprintf(stderr,
                            "[COLD_BOOT] Injected %s at PA 0x%06X"
                            " (%u words)\n",
                            reg->name, reg->pa, reg->word_count);
                    }
                }

                /*
                 * Seed resume_ctx at PA 0x2200 for the post-WAIT
                 * resume path.  After kernel_init, the scheduler
                 * enters idle → WAIT.  The post-WAIT OAL code at
                 * 0x79668 restores GPRs/CP0 from resume_ctx and
                 * JR $ra.  Without seeding, SP=0 and RA=0 → crash.
                 *
                 * Resume_ctx layout (PA 0x2200):
                 *   GPR: 0x00-0x7C (packed, skips $t0)
                 *     0x6C=SP, 0x70=$fp, 0x74=RA, 0x78=HI, 0x7C=LO
                 *   CP0: 0x80-0xD0 (packed, skips some)
                 *     0x80=Index, 0x84=Random, 0x88=EntryLo0,
                 *     0x8C=EntryLo1, 0x90=Context, 0x94=PageMask,
                 *     0x98=Wired, 0x9C=Count, 0xA0=EntryHi,
                 *     0xA4=Compare, 0xA8=Status, 0xAC=Cause,
                 *     0xB0=EPC, 0xB4=Config
                 *
                 * Critical: Wired MUST be 2 so TLB entries 0,1
                 * (installed by cold-start) survive TLBWR eviction.
                 * Values from BE300Probe_v1.txt PA 0x2280-0x22B4.
                 */
                /* GPR seeds */
                store_32bit_word(m->cpu,
                    0xffffffffa000226cULL, 0xA00017E0u); /* SP (kseg1, no TLB) */
                store_32bit_word(m->cpu,
                    0xffffffffa0002274ULL, 0x800794C8u); /* RA → OAL init */
                /* CP0 seeds from hardware survey */
                store_32bit_word(m->cpu,
                    0xffffffffa0002290ULL, 0x00000220u); /* Context: PTEBase */
                store_32bit_word(m->cpu,
                    0xffffffffa0002294ULL, 0x00001800u); /* PageMask: 4KB pages */
                store_32bit_word(m->cpu,
                    0xffffffffa0002298ULL, 0x00000002u); /* Wired: protect entries 0,1 */
                store_32bit_word(m->cpu,
                    0xffffffffa00022a8ULL, 0x00008001u); /* Status: IE=1 IM7 KSU=0 */
                store_32bit_word(m->cpu,
                    0xffffffffa00022acULL, 0x00000000u); /* Cause: clear */
                store_32bit_word(m->cpu,
                    0xffffffffa00022b4ULL, 0x10135923u); /* Config: VR4131 hw value */

                /*
                 * Cold boot later reaches the VR41xx low-power idle
                 * helpers at 0x80079990 / 0x800799F8. GXemul does not
                 * implement those standby/suspend opcodes for this CPU,
                 * so replace them with NOPs and let the surrounding
                 * interrupt-ack path continue running.
                 */
                store_32bit_word(m->cpu,
                    0xffffffff80079990ULL, 0x00000000u); /* suspend -> nop */
                store_32bit_word(m->cpu,
                    0xffffffff800799F8ULL, 0x00000000u); /* standby -> nop */

                /*
                 * Inject TLB context at PA 0x2000 from hardware
                 * survey (wince_hw_seed_ctx_tlb_data, 32 entries
                 * × 16 bytes).  The TLB refill handler may use
                 * this table to reload evicted entries.
                 */
                for (unsigned ci = 0;
                     ci < sizeof(wince_hw_seed_ctx_tlb_data);
                     ci += 4) {
                    uint32_t w =
                        (uint32_t)wince_hw_seed_ctx_tlb_data[ci]
                        | ((uint32_t)wince_hw_seed_ctx_tlb_data[ci+1] << 8)
                        | ((uint32_t)wince_hw_seed_ctx_tlb_data[ci+2] << 16)
                        | ((uint32_t)wince_hw_seed_ctx_tlb_data[ci+3] << 24);
                    store_32bit_word(m->cpu,
                        0xffffffffa0002000ULL + ci, w);
                }

                /*
                 * Patch OAL init continuation at 0x800794C8 to
                 * JR $ra during cold-start.
                 *
                 * The code at 0x800794C8 is the "OAL init
                 * continuation" block: it runs 6 init functions,
                 * flushes cache, calls 3 more init functions, then
                 * falls through to PMU/DCU setup and WAIT at
                 * 0x80079598.  During the cold-start, one of the
                 * sub-calls from 0x800A5C78 enters this block.
                 * The internal JAL at 0x80079558 clobbers RA to
                 * 0x80079560, so when WAIT (patched to JR $ra)
                 * executes, it loops back to the PMU/DCU setup —
                 * infinite loop.
                 *
                 * Fix: patch the ENTRY to the block (0x800794C8)
                 * to JR $ra.  This prevents the block from running
                 * entirely when called during cold-start.  The
                 * init functions it runs are also called directly
                 * by the cold-start or 0x800A5C78, so nothing is
                 * lost.  Original instruction: JAL 0x79ADC
                 * (0x0C01E6B7).
                 */
                be300_capture_cold_boot_oal_block(m);
                store_32bit_word(m->cpu,
                    0xffffffff800794C8ULL, 0x03E00008u); /* JR $ra */
                store_32bit_word(m->cpu,
                    0xffffffff800794CCULL, 0x00000000u); /* NOP */
                /* NOP-fill the entire OAL init continuation block
                 * from 0x800794C0 to 0x8007959C (56 words).
                 * End with JR $ra at 0x80079598.  This prevents
                 * ALL entry points into this block.  The block
                 * does PMU/DCU setup and ends in WAIT — all of
                 * which is unnecessary during cold-start. */
                for (uint32_t pi = 0; pi < 56; pi++) {
                    store_32bit_word(m->cpu,
                        0xffffffff800794C0ULL + pi * 4,
                        0x00000000u); /* NOP */
                }
                /* Place JR $ra at the start and end */
                store_32bit_word(m->cpu,
                    0xffffffff800794C0ULL, 0x03E00008u); /* JR $ra */
                store_32bit_word(m->cpu,
                    0xffffffff800794C8ULL, 0x03E00008u); /* JR $ra */
                store_32bit_word(m->cpu,
                    0xffffffff80079598ULL, 0x03E00008u); /* JR $ra */
                /* Force invalidate dyntrans code cache so patches
                 * take effect. */
                if (m->cpu->invalidate_code_translation)
                    m->cpu->invalidate_code_translation(
                        m->cpu, 0, INVALIDATE_ALL);
                /* Verify patches by reading back */
                {
                    static const uint32_t verify_addrs[] = {
                        0x800794C0, 0x800794C8,
                        0x80079560, 0x80079598,
                    };
                    fprintf(stderr,
                        "[COLD_BOOT] Verify patches:");
                    for (unsigned vi = 0; vi < 4; vi++) {
                        unsigned char vb[4];
                        uint64_t vva = 0xffffffff00000000ULL
                            | (uint64_t)(int32_t)verify_addrs[vi];
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                vva, vb, 4, MEM_READ, CACHE_DATA)) {
                            uint32_t vw = vb[0]|(vb[1]<<8)
                                |(vb[2]<<16)|(vb[3]<<24);
                            fprintf(stderr, " [%08X]=%08X",
                                verify_addrs[vi], vw);
                        }
                    }
                    fprintf(stderr, "\n");
                }
                fprintf(stderr,
                    "[COLD_BOOT] Patched 0x800794C0/C8/9560/9598"
                    " → JR $ra (block all OAL init paths)\n");

                /*
                 * Keep the original TLB[1] setup. Patching v0 at
                 * 0x8007B4D4 also corrupts the later Index write and
                 * prevents the wired VA 0xFFFFD000 kernel stack entry
                 * from being installed.
                 */

                m->cpu->pc =
                    (int64_t)(int32_t)UINT32_C(0x8007B398);
                m->cpu->cd.mips.mips16 = 0;
                m->cpu->cd.mips.gpr[MIPS_GPR_SP] =
                    (int32_t)UINT32_C(0x80003800);
                m->cpu->is_halted = false;
                /*
                 * Clear BEV (bit 22) so exceptions use the
                 * kernel's handlers at PA 0x0000/0x0180 instead
                 * of the ROM's BEV vectors.  On real hardware,
                 * the ROM clears BEV before jumping to NK.exe.
                 * Also clear IE so the cold-start runs with
                 * interrupts disabled until it's ready.
                 */
                m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS] &=
                    ~(uint64_t)(0x00400001u);  /* clear BEV + IE */
                fprintf(stderr,
                    "[COLD_BOOT] Skipping MIPS16 dispatcher,"
                    " jumping to kernel cold-start at"
                    " 0x8007B398 (RA seed at PA 0xD8"
                    " = 0x8007B57C)\n");

                /* Dump decompressed NK.exe binary for analysis */
                {
                    const uint32_t nk_pa = 0x00060000u;
                    const uint32_t nk_size = 0x005F6AC8u;
                    FILE *nk_fp = fopen("nk_decompressed.bin", "wb");
                    if (nk_fp) {
                        uint8_t page[4096];
                        uint32_t off = 0;
                        while (off < nk_size) {
                            uint32_t chunk = nk_size - off;
                            if (chunk > sizeof(page)) chunk = sizeof(page);
                            uint64_t va = 0xffffffff80000000ULL | (nk_pa + off);
                            if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                    va, page, chunk, MEM_READ, CACHE_DATA))
                                fwrite(page, 1, chunk, nk_fp);
                            else
                                break;
                            off += chunk;
                        }
                        fclose(nk_fp);
                        fprintf(stderr,
                            "[COLD_BOOT] Dumped NK.exe (%u bytes)"
                            " to nk_decompressed.bin\n", off);
                    }
                }

                /* Dump OEMInit callback table at PA 0x51680 */
                fprintf(stderr, "[COLD_BOOT] OEMInit callback table"
                    " (PA 0x51680, 32 entries × 20 bytes):\n");
                for (int i = 0; i < 32; i++) {
                    unsigned char buf[20];
                    uint64_t va = 0xffffffffa0051680ULL + i * 20;
                    int ok = 1;
                    for (int b = 0; b < 20 && ok; b += 4)
                        if (!m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                va + b, buf + b, 4, MEM_READ, CACHE_DATA))
                            ok = 0;
                    if (!ok) continue;
                    uint32_t w[5];
                    for (int j = 0; j < 5; j++)
                        w[j] = buf[j*4] | (buf[j*4+1]<<8) |
                                (buf[j*4+2]<<16) | (buf[j*4+3]<<24);
                    /* Entry format: offset 4 = function pointer */
                    if (w[1] != 0)
                        fprintf(stderr, "[COLD_BOOT]   [%2d]"
                            " %08X %08X %08X %08X %08X\n",
                            i, w[0], w[1], w[2], w[3], w[4]);
                }

                /* Dump PA 0x24FC (kernel jump target used by ROM) */
                {
                    unsigned char buf[4];
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            0xffffffffa00024fcULL, buf, 4,
                            MEM_READ, CACHE_DATA)) {
                        uint32_t w = buf[0] | (buf[1]<<8) |
                                     (buf[2]<<16) | (buf[3]<<24);
                        fprintf(stderr, "[COLD_BOOT] PA 0x24FC"
                            " (ROM kernel jump target): %08X\n", w);
                    }
                }

                /* Dump instructions around WAIT for disassembly */
                fprintf(stderr, "[COLD_BOOT] Code around WAIT:\n");
                for (int i = -8; i <= 40; i++) {
                    unsigned char buf[4];
                    uint64_t va = (uint64_t)(int32_t)wait_pc + i * 4;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            va, buf, 4, MEM_READ, CACHE_DATA)) {
                        uint32_t w = buf[0] | (buf[1]<<8) |
                                     (buf[2]<<16) | (buf[3]<<24);
                        fprintf(stderr, "[COLD_BOOT]   0x%08" PRIx64
                            ": %08X%s\n", va, w,
                            (int64_t)va == (int64_t)(int32_t)wait_pc
                                ? "  <-- WAIT" : "");
                    }
                }

                /* Dump CP0 table at PA 0x2200 (resume_ctx) */
                fprintf(stderr, "[COLD_BOOT] resume_ctx at PA 0x2200:\n");
                for (int i = 0; i < 64; i++) {
                    unsigned char buf[4];
                    uint64_t va = 0xffffffffa0002200ULL + i * 4;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            va, buf, 4, MEM_READ, CACHE_DATA)) {
                        uint32_t w = buf[0] | (buf[1]<<8) |
                                     (buf[2]<<16) | (buf[3]<<24);
                        if (w != 0)
                            fprintf(stderr, "[COLD_BOOT]   PA 0x%04X"
                                ": %08X\n", 0x2200 + i * 4, w);
                    }
                }

                /* Dump exception vectors at PA 0x0000 */
                fprintf(stderr, "[COLD_BOOT] Low vectors (PA 0x0000):\n");
                for (int i = 0; i < 16; i++) {
                    unsigned char buf[4];
                    uint64_t va = 0xffffffff80000000ULL + i * 4;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            va, buf, 4, MEM_READ, CACHE_DATA)) {
                        uint32_t w = buf[0] | (buf[1]<<8) |
                                     (buf[2]<<16) | (buf[3]<<24);
                        if (w != 0)
                            fprintf(stderr, "[COLD_BOOT]   PA 0x%04X"
                                ": %08X\n", i * 4, w);
                    }
                }

                /* Dump CP0 state */
                fprintf(stderr, "[COLD_BOOT] CP0: Status=0x%08X"
                    " Cause=0x%08X EPC=0x%08X\n",
                    (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS],
                    (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE],
                    (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC]);
                fprintf(stderr, "[COLD_BOOT] GPR: SP=0x%08X"
                    " RA=0x%08X S0=0x%08X\n",
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                    (uint32_t)m->cpu->cd.mips.gpr[16]);

                /* Dump memory the three check functions will read */
                fprintf(stderr, "[COLD_BOOT] Check data:\n");
                /* PA 0x250C (check 3 arg) */
                {
                    unsigned char buf[4];
                    uint64_t va = 0xffffffffa000250cULL;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            va, buf, 4, MEM_READ, CACHE_DATA)) {
                        uint32_t w = buf[0] | (buf[1]<<8) |
                                     (buf[2]<<16) | (buf[3]<<24);
                        fprintf(stderr, "[COLD_BOOT]   PA 0x250C"
                            " (check3 arg target): %08X\n", w);
                    }
                }
                /* Dump PA 0x2500-0x2520 for context */
                for (int i = 0; i < 8; i++) {
                    unsigned char buf[4];
                    uint64_t va = 0xffffffffa0002500ULL + i * 4;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            va, buf, 4, MEM_READ, CACHE_DATA)) {
                        uint32_t w = buf[0] | (buf[1]<<8) |
                                     (buf[2]<<16) | (buf[3]<<24);
                        if (w != 0)
                            fprintf(stderr, "[COLD_BOOT]   PA 0x%04X"
                                ": %08X\n", 0x2500 + i * 4, w);
                    }
                }

                /*
                 * Arm PC-based probes for the three check return points.
                 * We'll log when we hit the BNE/BEQ after each check.
                 */
                /* Dump the three check function prologues */
                fprintf(stderr, "[COLD_BOOT] check1 @ 0x80079AC4:\n");
                for (int i = 0; i < 24; i++) {
                    unsigned char buf[4];
                    uint64_t va = 0xffffffff80079AC4ULL + i * 4;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            va, buf, 4, MEM_READ, CACHE_DATA)) {
                        uint32_t w = buf[0] | (buf[1]<<8) |
                                     (buf[2]<<16) | (buf[3]<<24);
                        fprintf(stderr, "[COLD_BOOT]   0x%08" PRIx64
                            ": %08X\n", va, w);
                    }
                }
                fprintf(stderr, "[COLD_BOOT] check2 @ 0x8007AFA8:\n");
                for (int i = 0; i < 24; i++) {
                    unsigned char buf[4];
                    uint64_t va = 0xffffffff8007AFA8ULL + i * 4;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            va, buf, 4, MEM_READ, CACHE_DATA)) {
                        uint32_t w = buf[0] | (buf[1]<<8) |
                                     (buf[2]<<16) | (buf[3]<<24);
                        fprintf(stderr, "[COLD_BOOT]   0x%08" PRIx64
                            ": %08X\n", va, w);
                    }
                }

                /*
                 * Dump code at the last known good PC range before
                 * the crash (0x80079750+) to trace control flow.
                 */
                fprintf(stderr, "[COLD_BOOT] Code at 0x80079750"
                    " (post-ICU, before crash):\n");
                for (int i = 0; i < 32; i++) {
                    unsigned char buf[4];
                    uint64_t va = 0xffffffff80079750ULL + i * 4;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            va, buf, 4, MEM_READ, CACHE_DATA)) {
                        uint32_t w = buf[0] | (buf[1]<<8) |
                                     (buf[2]<<16) | (buf[3]<<24);
                        fprintf(stderr, "[COLD_BOOT]   0x%08" PRIx64
                            ": %08X\n", va, w);
                    }
                }

                /* Also dump what's at/around PA 0x2400 (crash site) */
                fprintf(stderr, "[COLD_BOOT] Code/data at crash"
                    " site 0x80002400:\n");
                for (int i = -4; i < 8; i++) {
                    unsigned char buf[4];
                    uint64_t va = 0xffffffff80002400ULL + i * 4;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            va, buf, 4, MEM_READ, CACHE_DATA)) {
                        uint32_t w = buf[0] | (buf[1]<<8) |
                                     (buf[2]<<16) | (buf[3]<<24);
                        fprintf(stderr, "[COLD_BOOT]   PA 0x%04X"
                            ": %08X\n",
                            0x2400 + i * 4, w);
                    }
                }
            } else {
                /*
                 * Check if we're still in cold-start init phase.
                 * [0x80669554] is set by 0x800942B4 when the kernel
                 * memory pool is initialized.  Until then, sub-calls
                 * from 0x800A5C78 may hit WAIT, and the post-WAIT
                 * resume path would hijack execution via resume_ctx.
                 * Skip WAITs during this phase.
                 */
                unsigned char ck_buf[4];
                uint32_t ck_val = 0;
                bool in_init_phase = false;
                if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                        0xffffffff80669554ULL, ck_buf, 4,
                        MEM_READ, CACHE_DATA))
                    ck_val = ck_buf[0]|(ck_buf[1]<<8)
                            |(ck_buf[2]<<16)|(ck_buf[3]<<24);
                if (ck_val == 0 &&
                    m->wince.cold_boot_wait_count < 50)
                    in_init_phase = true;

                if (g_cold_boot_oal_block_restored
                    && wait_pc >= UINT32_C(0x80079480)
                    && wait_pc <= UINT32_C(0x800797E0)) {
                    m->wince.cold_boot_late_oal_wait_seen = true;
                    if (!(m->wince.cold_boot_pc_probes_logged
                            & COLD_BOOT_PROBE_RESTORED_WAIT_LOGGED)) {
                        m->wince.cold_boot_pc_probes_logged |=
                            COLD_BOOT_PROBE_RESTORED_WAIT_LOGGED;
                        fprintf(stderr,
                            "[COLD_BOOT] Restored OAL WAIT reached:"
                            " PC=0x%08X SP=0x%08X RA=0x%08X"
                            " ready_ptr=0x%08X in_init_phase=%d"
                            " wait_count=%u Status=0x%08X"
                            " Cause=0x%08X EPC=0x%08X\n",
                            wait_pc,
                            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                            ck_val,
                            in_init_phase ? 1 : 0,
                            m->wince.cold_boot_wait_count,
                            (uint32_t)m->cpu->cd.mips.coproc[0]
                                ->reg[COP0_STATUS],
                            (uint32_t)m->cpu->cd.mips.coproc[0]
                                ->reg[COP0_CAUSE],
                            (uint32_t)m->cpu->cd.mips.coproc[0]
                                ->reg[COP0_EPC]);
                    }
                }

                if (!in_init_phase)
                    be300_maybe_restore_cold_boot_oal_block(m, "halt");

                if (in_init_phase) {
                if (g_cold_boot_oal_block_restored
                    && wait_pc == UINT32_C(0x80079598)
                    && !(m->wince.cold_boot_pc_probes_logged
                        & COLD_BOOT_PROBE_RESTORED_HANDOFF_DONE)) {
                    uint32_t resume_target =
                        wince_resume_replay_snapshot.resume_target_pc;
                    uint32_t synthetic_ra =
                        wince_resume_replay_snapshot.synthetic_ra;
                    uint32_t slot_va =
                        (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP]
                        + UINT32_C(0x24);
                    uint32_t status =
                        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS];

                    if (synthetic_ra != 0) {
                        store_32bit_word(m->cpu,
                            be300_va32_to_mips64(slot_va),
                            synthetic_ra);
                        m->cpu->cd.mips.gpr[MIPS_GPR_RA] = synthetic_ra;
                    }
                    m->cpu->cd.mips.coproc[0]->reg[COP0_EPC] =
                        resume_target != 0
                            ? resume_target
                            : UINT32_C(0x800795B4);
                    if ((status & UINT32_C(0x8001)) != UINT32_C(0x8001)) {
                        status |= UINT32_C(0x8001);
                        m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS] = status;
                    }
                    m->cpu->pc =
                        (int64_t)(int32_t)UINT32_C(0x800795B4);
                    m->cpu->is_halted = false;
                    m->wince.cold_boot_pc_probes_logged |=
                        COLD_BOOT_PROBE_RESTORED_HANDOFF_DONE;
                    fprintf(stderr,
                        "[COLD_BOOT] Direct restored-WAIT handoff:"
                        " PC=0x800795B4 EPC=0x%08X"
                        " RA=0x%08X stack[%08X]=0x%08X\n",
                        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC],
                        (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                        slot_va,
                        synthetic_ra);
                    return true;
                }
                /*
                 * WAIT during cold-start init phase.
                 *
                 * The cold-start at 0x8007B5C0 calls 0x800A5C78,
                 * which calls sub-functions that hit WAIT.  The
                 * post-WAIT resume path at 0x79668 restores ALL
                 * GPRs from resume_ctx (PA 0x2200).  If resume_ctx
                 * has our initial seeds (RA=0x800794C8), execution
                 * is hijacked to OAL init and the cold-start never
                 * completes.
                 *
                 * Fix: save the current GPR/CP0 state into
                 * resume_ctx before each WAIT.  This is what the
                 * real hardware's state-save function at 0x76E68
                 * does, but we fail its gating checks.  After the
                 * post-WAIT resume path restores from resume_ctx,
                 * execution continues from the correct point.
                 *
                 * resume_ctx layout (PA 0x2200):
                 *   GPR: 0x00-0x7C (packed, skips $t0)
                 *     at(1)-a3(7), t1(9)-gp(28), sp, fp, ra, HI, LO
                 *   CP0: 0x80-0xD0
                 */
                {
                    /* Save GPRs to resume_ctx (PA 0x2200).
                     * Layout: packed, skips $t0 (reg 8). */
                    static const int gpr_map[] = {
                        1,2,3,4,5,6,7,  /* at-a3: offsets 0x00-0x18 */
                        9,10,11,12,13,14,15, /* t1-t7: 0x1C-0x34 */
                        16,17,18,19,20,21,22,23, /* s0-s7: 0x38-0x54 */
                        24,25,26,27,28, /* t8-gp: 0x58-0x68 */
                        29,30,31, /* sp,fp,ra: 0x6C-0x74 */
                    };
                    for (unsigned gi = 0;
                         gi < sizeof(gpr_map)/sizeof(gpr_map[0]);
                         gi++) {
                        uint32_t val = (uint32_t)
                            m->cpu->cd.mips.gpr[gpr_map[gi]];
                        store_32bit_word(m->cpu,
                            0xffffffffa0002200ULL + gi * 4, val);
                    }
                    /* HI, LO at offsets 0x78, 0x7C */
                    store_32bit_word(m->cpu,
                        0xffffffffa0002278ULL,
                        (uint32_t)m->cpu->cd.mips.hi);
                    store_32bit_word(m->cpu,
                        0xffffffffa000227cULL,
                        (uint32_t)m->cpu->cd.mips.lo);

                    /* Save key CP0 regs (offsets 0x80-0xD0) */
                    uint64_t *cp0r =
                        m->cpu->cd.mips.coproc[0]->reg;
                    static const struct { int reg; int off; } cp0_map[] = {
                        {COP0_INDEX,0x80}, {COP0_RANDOM,0x84},
                        {COP0_ENTRYLO0,0x88}, {COP0_ENTRYLO1,0x8C},
                        {COP0_CONTEXT,0x90}, {COP0_PAGEMASK,0x94},
                        {COP0_WIRED,0x98}, {COP0_COUNT,0x9C},
                        {COP0_ENTRYHI,0xA0}, {COP0_COMPARE,0xA4},
                        {COP0_STATUS,0xA8}, {COP0_CAUSE,0xAC},
                        {COP0_EPC,0xB0}, {COP0_CONFIG,0xB4},
                    };
                    for (unsigned ci = 0;
                         ci < sizeof(cp0_map)/sizeof(cp0_map[0]);
                         ci++) {
                        store_32bit_word(m->cpu,
                            0xffffffffa0002200ULL + cp0_map[ci].off,
                            (uint32_t)cp0r[cp0_map[ci].reg]);
                    }

                    /*
                     * Once the original OAL block has been restored,
                     * the idle WAIT at 0x80079598 must resume at the
                     * post-WAIT continuation (0x800795B4), not back
                     * into the PMU/DCU setup at RA=0x80079560.
                     */
                    if (g_cold_boot_oal_block_restored
                        && wait_pc == UINT32_C(0x80079598)) {
                        uint32_t resume_target =
                            wince_resume_replay_snapshot.resume_target_pc;
                        uint32_t synthetic_ra =
                            wince_resume_replay_snapshot.synthetic_ra;
                        uint32_t slot_va =
                            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP]
                            + UINT32_C(0x24);

                        store_32bit_word(m->cpu,
                            0xffffffffa0002274ULL,
                            UINT32_C(0x800795B4));
                        store_32bit_word(m->cpu,
                            0xffffffffa00022b0ULL,
                            resume_target != 0
                                ? resume_target
                                : UINT32_C(0x800795B4));
                        if (synthetic_ra != 0) {
                            store_32bit_word(m->cpu,
                                be300_va32_to_mips64(slot_va),
                                synthetic_ra);
                        }
                        if (!(m->wince.cold_boot_pc_probes_logged
                                & COLD_BOOT_PROBE_RESTORED_RA_LOGGED)) {
                            m->wince.cold_boot_pc_probes_logged |=
                                COLD_BOOT_PROBE_RESTORED_RA_LOGGED;
                            fprintf(stderr,
                                "[COLD_BOOT] Restored OAL WAIT resume_ctx"
                                " override: RA=0x800795B4"
                                " EPC=0x%08X stack[%08X]=0x%08X"
                                " (live RA=0x%08X)\n",
                                resume_target != 0
                                    ? resume_target
                                    : UINT32_C(0x800795B4),
                                slot_va,
                                synthetic_ra,
                                (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA]);
                        }
                    }
                }
                m->wince.cold_boot_wait_count++;
                if (m->wince.cold_boot_wait_count <= 10) {
                    fprintf(stderr,
                        "[COLD_BOOT] Saved GPR/CP0 to resume_ctx"
                        " before WAIT #%u at PC=0x%08X"
                        " (SP=0x%08X RA=0x%08X)\n",
                        m->wince.cold_boot_wait_count + 1,
                        (uint32_t)m->cpu->pc,
                        (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                        (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA]);
                }
                } else {
                /*
                 * Subsequent WAITs: the scheduler idle loop.
                 *
                 * WAIT #2 (count==1): OAL init at 0x794C8 has just
                 * run — callbacks initialized hardware.  Update
                 * resume_ctx RA from 0x794C8 (OAL init, one-time)
                 * to 0x80096894 (scheduler dispatch, steady-state).
                 * Also update SP and $s0 from hardware survey so
                 * the scheduler has proper kernel stack and context.
                 *
                 * WAIT #3+: scheduler idle, timer fires → scheduler
                 * checks for ready threads → dispatches or idles.
                 */
                m->wince.cold_boot_wait_count++;

                if (m->wince.cold_boot_wait_count == 1) {
                    /*
                     * First subsequent WAIT after cold-start.
                     * Do NOT switch resume_ctx to scheduler mode.
                     *
                     * The OAL init at RA=0x800794C8 is the correct
                     * resume target — it runs idempotent HW init and
                     * falls through to WAIT (the system idle point).
                     * The WinCE scheduler is interrupt-driven: the
                     * timer ISR dispatches threads, not the resume
                     * path.  Patching resume_ctx RA to the scheduler
                     * dispatch loop (0x80096894) caused a crash
                     * because the scheduler's stack frame at
                     * SP=0xFFFFD7B4 was uninitialized.
                     *
                     * Dump resume_ctx and TLB for diagnostics.
                     */
                    /* Dump current resume_ctx key fields */
                    {
                        unsigned char buf[4];
                        uint32_t ctx_ra = 0, ctx_sp = 0, ctx_s0 = 0;
                        uint32_t ctx_status = 0, ctx_epc = 0;
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                0xffffffffa0002274ULL, buf, 4,
                                MEM_READ, CACHE_DATA))
                            ctx_ra = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                0xffffffffa000226cULL, buf, 4,
                                MEM_READ, CACHE_DATA))
                            ctx_sp = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                0xffffffffa0002238ULL, buf, 4,
                                MEM_READ, CACHE_DATA))
                            ctx_s0 = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                0xffffffffa00022a8ULL, buf, 4,
                                MEM_READ, CACHE_DATA))
                            ctx_status = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                0xffffffffa00022b0ULL, buf, 4,
                                MEM_READ, CACHE_DATA))
                            ctx_epc = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                        fprintf(stderr,
                            "[COLD_BOOT] WAIT #2 resume_ctx:"
                            " RA=0x%08X SP=0x%08X S0=0x%08X"
                            " Status=0x%08X EPC=0x%08X\n",
                            ctx_ra, ctx_sp, ctx_s0,
                            ctx_status, ctx_epc);
                    }
                    fprintf(stderr,
                        "[COLD_BOOT] WAIT #2: keeping resume_ctx"
                        " (OAL init is idempotent, scheduler is"
                        " interrupt-driven)\n");

                    /* Dump kernel data structures to check
                     * if kernel_init populated them */
                    {
                        unsigned char buf[4];
                        uint32_t w;
                        /* Object table pointer [0x80660000] */
                        fprintf(stderr,
                            "[COLD_BOOT] Kernel data after init:\n");
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                0xffffffff80660000ULL, buf, 4,
                                MEM_READ, CACHE_DATA)) {
                            w = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                            fprintf(stderr,
                                "[COLD_BOOT]   [0x80660000] = 0x%08X"
                                " (obj table ptr)\n", w);
                            if (w != 0) {
                                /* Read first 4 entries of object table */
                                for (int j = 0; j < 4; j++) {
                                    uint64_t va = 0xffffffff00000000ULL
                                        | (uint64_t)(int32_t)(w + j*4);
                                    if (m->cpu->memory_rw(m->cpu,
                                            m->cpu->mem, va, buf, 4,
                                            MEM_READ, CACHE_DATA)) {
                                        uint32_t e = buf[0]|(buf[1]<<8)
                                            |(buf[2]<<16)|(buf[3]<<24);
                                        fprintf(stderr,
                                            "[COLD_BOOT]   [0x%08X+%d]"
                                            " = 0x%08X\n", w, j*4, e);
                                    }
                                }
                            }
                        }
                        /* First 16 words of .data (PA 0x660000) */
                        fprintf(stderr,
                            "[COLD_BOOT]   .data (PA 0x660000):");
                        for (int j = 0; j < 16; j++) {
                            if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                    0xffffffffa0660000ULL + j*4, buf, 4,
                                    MEM_READ, CACHE_DATA)) {
                                w = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                                if (j % 8 == 0 && j > 0)
                                    fprintf(stderr, "\n[COLD_BOOT]          ");
                                fprintf(stderr, " %08X", w);
                            }
                        }
                        fprintf(stderr, "\n");
                        /* Check ulRAMStart area (0x80660000+)
                         * for signs kernel_init wrote here */
                        {
                            int nz1k = 0, nz4k = 0, nz16k = 0;
                            for (int j = 0; j < 4096; j++) {
                                if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                        0xffffffffa0660000ULL + j*4, buf, 4,
                                        MEM_READ, CACHE_DATA)) {
                                    w = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                                    if (w != 0) {
                                        nz16k++;
                                        if (j < 1024) nz4k++;
                                        if (j < 256) nz1k++;
                                    }
                                }
                            }
                            fprintf(stderr,
                                "[COLD_BOOT]   .data non-zero:"
                                " 1KB=%d/256 4KB=%d/1024"
                                " 16KB=%d/4096\n",
                                nz1k, nz4k, nz16k);
                        }
                        /* Trace cold-start function execution by
                         * checking side effects of each call */
                        fprintf(stderr,
                            "[COLD_BOOT]   Cold-start call trace:\n");
                        /* 0x8007B5BC: sh PRId to [0x80669514]
                         * (LUI 0x8067 + ADDIU 0x9514 = 0x80669514) */
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                0xffffffff80669514ULL, buf, 4,
                                MEM_READ, CACHE_DATA)) {
                            w = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                            fprintf(stderr,
                                "[COLD_BOOT]     [0x80669514] PRId"
                                " = 0x%04X (%s)\n", w & 0xFFFF,
                                (w & 0xFFFF) != 0
                                    ? "sh PRId ran" : "NOT reached");
                        }
                        /* 0x800A5C78 writes 25 to [0x806696D4]
                         * after all its sub-calls (step 18) */
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                0xffffffff806696D4ULL, buf, 4,
                                MEM_READ, CACHE_DATA)) {
                            w = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                            fprintf(stderr,
                                "[COLD_BOOT]     [0x806696D4]"
                                " = %u (A5C78 step18 %s)\n", w,
                                w == 25 ? "reached" : "NOT reached");
                        }
                        /* 0x800A5C78 writes to [0x8066956C] near
                         * end (step 23, after LBTR check) */
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                0xffffffff8066956CULL, buf, 4,
                                MEM_READ, CACHE_DATA)) {
                            w = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                            fprintf(stderr,
                                "[COLD_BOOT]     [0x8066956C]"
                                " = 0x%08X (A5C78 late %s)\n", w,
                                w != 0 ? "reached" : "NOT reached");
                        }
                        /* 0x800964FC writes to [0x80668C20] */
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                0xffffffff80668C20ULL, buf, 4,
                                MEM_READ, CACHE_DATA)) {
                            w = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                            fprintf(stderr,
                                "[COLD_BOOT]     [0x80668C20]"
                                " = 0x%08X (964FC %s)\n", w,
                                w != 0 ? "ran" : "NOT reached");
                        }
                        /* Check key addresses that 0x800942B4
                         * should have written */
                        {
                            uint32_t addrs[] = {
                                0x0066D000, /* should be 1 (init flag) */
                                0x0066D004, /* should be "MIKE" */
                                0x0066D008, /* should be 0 or data */
                            };
                            for (unsigned j = 0; j < 3; j++) {
                                if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                        0xffffffffa0000000ULL | addrs[j],
                                        buf, 4, MEM_READ, CACHE_DATA)) {
                                    w = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                                    fprintf(stderr,
                                        "[COLD_BOOT]   PA 0x%06X"
                                        " = 0x%08X%s\n",
                                        addrs[j], w,
                                        addrs[j]==0x66D004 && w==0x4D494B45
                                            ? " (MIKE)" : "");
                                }
                            }
                            /* Check [0x80669554] — pointer set by
                             * 0x800942B4 to kseg1(ulRAMFree)
                             * (LUI 0x8067 + offset 0x9554 =
                             *  0x80670000 - 0x6AAC = 0x80669554) */
                            if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                    0xffffffff80669554ULL, buf, 4,
                                    MEM_READ, CACHE_DATA)) {
                                w = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                                fprintf(stderr,
                                    "[COLD_BOOT]   [0x80669554]"
                                    " = 0x%08X (free RAM ptr)\n", w);
                            }
                        }
                        /* Check free RAM at ulRAMFree=0x8066D000
                         * where kernel_init allocates */
                        {
                            int nz_free = 0;
                            uint32_t first_nz = 0, last_nz = 0;
                            for (int j = 0; j < 8192; j++) {
                                if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                        0xffffffffa066D000ULL + j*4, buf, 4,
                                        MEM_READ, CACHE_DATA)) {
                                    w = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                                    if (w != 0) {
                                        nz_free++;
                                        uint32_t addr = 0x66D000+j*4;
                                        if (first_nz == 0) first_nz = addr;
                                        last_nz = addr;
                                    }
                                }
                            }
                            fprintf(stderr,
                                "[COLD_BOOT]   Free RAM (PA 0x66D000+"
                                " 32KB): %d/8192 non-zero",
                                nz_free);
                            if (nz_free > 0)
                                fprintf(stderr,
                                    " first=0x%06X last=0x%06X",
                                    first_nz, last_nz);
                            fprintf(stderr, "\n");
                        }
                        /* Dump PA 0x2400 version marker */
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                0xffffffffa0002400ULL, buf, 4,
                                MEM_READ, CACHE_DATA)) {
                            w = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                            fprintf(stderr,
                                "[COLD_BOOT]   PA 0x2400"
                                " (version): 0x%08X\n", w);
                        }
                        /* Dump ROMHDR at pTOC (0x80655C54) */
                        fprintf(stderr,
                            "[COLD_BOOT]   ROMHDR at pTOC"
                            " (0x80655C54):");
                        for (int j = 0; j < 16; j++) {
                            if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                    0xffffffff80655C54ULL + j*4, buf, 4,
                                    MEM_READ, CACHE_DATA)) {
                                w = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                                if (j % 8 == 0 && j > 0)
                                    fprintf(stderr,
                                        "\n[COLD_BOOT]                    ");
                                fprintf(stderr, " %08X", w);
                            }
                        }
                        fprintf(stderr, "\n");
                        /* Section table at PA 0x18C0 (first 8) */
                        fprintf(stderr,
                            "[COLD_BOOT]   Section table (PA 0x18C0):");
                        for (int j = 0; j < 8; j++) {
                            if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                    0xffffffffa00018c0ULL + j*4, buf, 4,
                                    MEM_READ, CACHE_DATA)) {
                                w = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                                fprintf(stderr, " %08X", w);
                            }
                        }
                        fprintf(stderr, "\n");
                        /* Page table at PA 0x1000 (first 8) */
                        fprintf(stderr,
                            "[COLD_BOOT]   Page table (PA 0x1000):");
                        for (int j = 0; j < 8; j++) {
                            if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                    0xffffffffa0001000ULL + j*4, buf, 4,
                                    MEM_READ, CACHE_DATA)) {
                                w = buf[0]|(buf[1]<<8)|(buf[2]<<16)|(buf[3]<<24);
                                fprintf(stderr, " %08X", w);
                            }
                        }
                        fprintf(stderr, "\n");
                        /* Check current live GPR state */
                        fprintf(stderr,
                            "[COLD_BOOT]   Live GPR: PC=0x%08X"
                            " SP=0x%08X RA=0x%08X"
                            " S0=0x%08X V0=0x%08X\n",
                            (uint32_t)m->cpu->pc,
                            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                            (uint32_t)m->cpu->cd.mips.gpr[16],
                            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_V0]);
                    }
                }

                {
                    uint32_t status =
                        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS];
                    if (!(status & 0x1u)) {
                        /* IE not set — enable it plus IM[7] for timer */
                        status |= 0x8001u;
                        m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS] =
                            status;
                    }
                }
                if (m->wince.cold_boot_wait_count <= 5) {
                    fprintf(stderr,
                        "[COLD_BOOT] WAIT #%u at PC=0x%08" PRIx64
                        ", halted with IE=1"
                        " (Status=0x%08X Cause=0x%08X"
                        " EPC=0x%08X BadVA=0x%08X)\n",
                        m->wince.cold_boot_wait_count + 1,
                        m->cpu->pc,
                        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS],
                        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE],
                        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC],
                        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR]);
                }
                /* On first subsequent WAIT, dump the exception handlers
                   that OAL init installed at PA 0x0000 */
                if (m->wince.cold_boot_wait_count == 1) {
                    fprintf(stderr,
                        "[COLD_BOOT] Exception handlers after"
                        " OAL init:\n");
                    fprintf(stderr, "[COLD_BOOT] TLB Refill"
                        " (PA 0x0000):\n");
                    for (int i = 0; i < 32; i++) {
                        unsigned char buf[4];
                        uint64_t va = 0xffffffff80000000ULL + i*4;
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                va, buf, 4, MEM_READ, CACHE_DATA)) {
                            uint32_t w = buf[0] | (buf[1]<<8) |
                                         (buf[2]<<16) | (buf[3]<<24);
                            fprintf(stderr, "[COLD_BOOT]   0x%04X"
                                ": %08X\n", i * 4, w);
                        }
                    }
                    fprintf(stderr, "[COLD_BOOT] General Exception"
                        " (PA 0x0180):\n");
                    for (int i = 0; i < 32; i++) {
                        unsigned char buf[4];
                        uint64_t va = 0xffffffff80000180ULL + i*4;
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                va, buf, 4, MEM_READ, CACHE_DATA)) {
                            uint32_t w = buf[0] | (buf[1]<<8) |
                                         (buf[2]<<16) | (buf[3]<<24);
                            fprintf(stderr, "[COLD_BOOT]   0x%04X"
                                ": %08X\n", 0x180 + i * 4, w);
                        }
                    }
                    /* Dump resume_ctx to see what OAL wrote */
                    fprintf(stderr, "[COLD_BOOT] resume_ctx"
                        " after OAL init:\n");
                    for (int i = 0; i < 64; i++) {
                        unsigned char buf[4];
                        uint64_t va = 0xffffffffa0002200ULL + i*4;
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                va, buf, 4, MEM_READ, CACHE_DATA)) {
                            uint32_t w = buf[0] | (buf[1]<<8) |
                                         (buf[2]<<16) | (buf[3]<<24);
                            if (w != 0)
                                fprintf(stderr,
                                    "[COLD_BOOT]   PA 0x%04X"
                                    ": %08X\n", 0x2200 + i*4, w);
                        }
                    }
                }
            }
            }  /* close outer else (cold-start phase check) */
        } else if (m->wince.hibernate_redirect_count < 5) {
            uint32_t norm = (uint32_t)m->cpu->pc & 0x1FFFFFFFu;
            if (norm >= 0x00060000u && norm < 0x00100000u) {
                uint64_t old_pc = m->cpu->pc;
                static const uint32_t tlb_h[] = {
                    0x401B5000, 0x001BD1C2, 0x375A001F,
                    0x409A1000, 0x275B0040, 0x409B1800,
                    0x42000006, 0x42000018,
                };

                wince_boot_install_synthetic_low_vectors(m, tlb_h,
                    sizeof(tlb_h) / sizeof(tlb_h[0]),
                    m->cfg.wince_resume_replay
                        ? "resume-replay"
                        : "cold-boot-redirect");

                if (m->cfg.wince_resume_replay) {
                    uint32_t target_pc = 0;
                    uint32_t target_sp = 0;

                    if (!wince_boot_prepare_resume_replay(m,
                        (uint32_t)old_pc, &target_pc, &target_sp)) {
                        wince_boot_note_fatal_stop(m,
                            "resume-replay-prepare-failed");
                        fprintf(stderr,
                            "[BE300] Resume replay prepare failed"
                            " at halt PC=0x%08" PRIx64 "\n",
                            old_pc);
                        return false;
                    }

                    m->cpu->pc = (uint64_t)(int32_t)target_pc;
                    if (target_sp != 0)
                        m->cpu->cd.mips.gpr[MIPS_GPR_SP] = target_sp;
                    m->cpu->cd.mips.coproc[0]->reg[COP0_EPC] =
                        m->wince.replay_resume_target_pc != 0
                            ? m->wince.replay_resume_target_pc
                            : target_pc;
                    m->cpu->is_halted = false;
                    m->wince.hibernate_redirect_count++;

                    fprintf(stderr,
                        "[BE300] Resume replay: PC 0x%08" PRIx64
                        " -> 0x%08X EPC=0x%08X SP=%s0x%08X\n",
                        old_pc,
                        target_pc,
                        m->wince.replay_resume_target_pc != 0
                            ? m->wince.replay_resume_target_pc
                            : target_pc,
                        target_sp != 0 ? "" : "(keep) ",
                        target_sp);
                    wince_boot_note_cold_boot_redirect(
                        m, "hibernate-to-resume-replay");
                    return true;
                }

                m->cpu->pc += 0x9C;
                m->cpu->is_halted = false;
                m->wince.hibernate_redirect_count++;
                m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS] = 0x1000FF00u;
                m->cpu->cd.mips.coproc[0]->reg[COP0_WIRED] = 0;

                fprintf(stderr,
                    "[BE300] Cold boot: skip hibernate+resume,"
                    " PC 0x%08" PRIx64 " → 0x%08" PRIx64
                    " Status=0x%08X\n",
                    old_pc, m->cpu->pc,
                    (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS]);

                if (m->cfg.log_wince_stall
                    && m->wince.hibernate_redirect_count == 1) {
                    fprintf(stderr, "[COLD_INIT] Dumping 0x80079DF8:\n");
                    for (int i = 0; i < 128; i++) {
                        unsigned char buf[4];
                        uint64_t addr = 0xffffffff80079DF8ULL + i * 4;
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                addr, buf, 4, MEM_READ, CACHE_DATA)) {
                            uint32_t w = buf[0] | (buf[1]<<8) |
                                         (buf[2]<<16) | (buf[3]<<24);
                            fprintf(stderr, "[COLD_INIT] 0x%08" PRIx64
                                ": %08X\n", addr, w);
                        }
                    }
                }

                {
                    static const uint32_t cp0_seed[] = {
                        0x00000000, 0x00000007, 0x00000000, 0x00000000,
                        0x00000000, 0x00001800, 0x00000000, 0x00000000,
                        0x00000000, 0x00000000, 0x00000000, 0x00000000,
                        0x1000FF00, 0x00000000, 0x00000000, 0x00000C80,
                        0x00000000, 0x00000000, 0x00000000, 0x00000000,
                    };
                    uint64_t base = 0xffffffff80002280ULL;
                    for (unsigned si = 0; si < sizeof(cp0_seed)/4; si++)
                        store_32bit_word(m->cpu, base + si * 4,
                            cp0_seed[si]);
                }
                wince_boot_apply_resume_seed(m);
                wince_boot_note_cold_boot_redirect(
                    m, "hibernate-to-cold-boot");
            }
        }
    }

    if (++m->loop_count % 100 == 0) {
        /* keep hook point for extremely verbose diagnostics */
    }

    return true;
}

int be300_step(machine_t *m, uint32_t max_batches)
{
    if (!m)
        return -1;
    if (m->boot_mode == BE300_BOOT_NONE) {
        fprintf(stderr, "[BE300] Cannot run before boot media is loaded\n");
        return -1;
    }
    if (m->runtime_finalized)
        return 0;
    if (max_batches == 0)
        max_batches = 1;

    be300_runtime_start(m);

    for (uint32_t i = 0; i < max_batches; i++) {
        if (!be300_run_batch(m)) {
            be300_runtime_finalize(m);
            return 0;
        }
    }

    return 1;
}

void be300_run(machine_t *m)
{
    while (be300_step(m, 1) > 0) {
    }
}

machine_t *be300_create_web(uint32_t sdram_mb, uint32_t target_mhz,
                            bool sfb_5bit_green)
{
    machine_config_t cfg = {
        .trace = false,
        .log_mmio = false,
        .sfb_5bit_green = sfb_5bit_green,
        .log_nand_legacy = false,
        .log_wince_stall = false,
        .wince_hw_seed = false,
        .wince_resume_replay = false,
        .wince_resume_replay_full = false,
        .rom_path = NULL,
        .kernel_path = NULL,
        .cmdline = NULL,
        .ram_path = NULL,
        .nand_path = NULL,
        .sdram_size = (sdram_mb ? sdram_mb : 16u) * 1024u * 1024u,
        .target_mhz = target_mhz,
    };
    machine_t *m = be300_create(&cfg);
    if (!m)
        return NULL;

    m->web_mode = true;
    m->use_builtin_ui = false;
    m->save_exit_screenshot = false;
    m->mirror_serial_to_stdout = false;
    return m;
}

int be300_boot_linux_from_memory(machine_t *m,
                                 const void *kernel_data,
                                 size_t kernel_size,
                                 const char *cmdline)
{
    return be300_boot_linux_memory_internal(m, kernel_data, kernel_size, cmdline);
}

int be300_copy_frame_rgba8888(machine_t *m, uint8_t *dst, size_t dst_len,
                              uint32_t *width_out, uint32_t *height_out)
{
    const uint16_t *src;
    uint32_t width;
    uint32_t height;

    if (!m || !dst || !width_out || !height_out)
        return -1;

    width = m->fb_width ? m->fb_width : 240u;
    height = m->fb_height ? m->fb_height : 320u;
    if (dst_len < (size_t)width * height * 4u)
        return -1;

    if (!m->fb_data) {
        if (!m->gxe_machine || !m->gxe_machine->fb || !m->gxe_machine->fb->framebuffer)
            return 0;
        m->fb_data = m->gxe_machine->fb->framebuffer;
    }

    src = (const uint16_t *)m->fb_data;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint16_t pixel = src[y * m->fb_stride + x];
            uint32_t r = (pixel >> 11) & 0x1Fu;
            uint32_t b = pixel & 0x1Fu;
            uint32_t g;
            size_t off = ((size_t)y * width + x) * 4u;

            if (m->cfg.sfb_5bit_green) {
                g = (pixel >> 5) & 0x1Fu;
                g = (g << 3) | (g >> 2);
            } else {
                g = (pixel >> 5) & 0x3Fu;
                g = (g << 2) | (g >> 4);
            }

            r = (r << 3) | (r >> 2);
            b = (b << 3) | (b >> 2);

            dst[off + 0] = (uint8_t)r;
            dst[off + 1] = (uint8_t)g;
            dst[off + 2] = (uint8_t)b;
            dst[off + 3] = 0xFFu;
        }
    }

    *width_out = width;
    *height_out = height;
    return 1;
}

size_t be300_drain_serial(machine_t *m, char *dst, size_t dst_len)
{
    size_t count = 0;

    if (!m || !dst || dst_len == 0)
        return 0;

    while (count < dst_len && m->serial_count > 0) {
        dst[count++] = m->serial_ring[m->serial_tail];
        m->serial_tail = (m->serial_tail + 1u) % BE300_SERIAL_RING_CAP;
        m->serial_count--;
    }

    return count;
}

void be300_set_touch(machine_t *m, bool down, uint16_t x, uint16_t y)
{
    if (!m)
        return;
    m->touch_down = down;
    m->touch_x = x;
    m->touch_y = y;
    __sync_synchronize();
}

void be300_set_buttons(machine_t *m, uint8_t btn_set1, uint8_t btn_set2)
{
    if (!m)
        return;
    m->btn_set1 = btn_set1;
    m->btn_set2 = btn_set2;
    __sync_synchronize();
}

void be300_stop(machine_t *m)
{
    (void)m;
    emul_shutdown = true;
    __sync_synchronize();
}


void be300_destroy(machine_t *m)
{
    if (!m) return;

    be300_stop(m);
    be300_runtime_finalize(m);

    wince_boot_detach_machine(m);

    if (m->nand_data) {
        free(m->nand_data);
        m->nand_data = NULL;
    }

    free(m);

    /*
     * Clear GXemul global state so that be300_create() can be called again.
     * interrupt_handler_register() aborts with exit(1) on duplicate names,
     * and timer_add() appends to a global list — both must be reset between
     * machine instantiations (e.g. kernel switch from the Android UI).
     */
    interrupt_handler_clear_all();
    timer_remove_all();
}
