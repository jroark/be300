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

    /*
     *  Callback table frame copy:
     *
     *  ROM function 0x10EC deliberately changes SP from the normal stack
     *  (0xA0007Cxx) to the callback table area (0x80008080).  When it
     *  calls the epilogue at 0xAD0, the epilogue reads the saved register
     *  frame from SP+16 = 0x80008090.  But the prologue at 0xA98 saved
     *  the registers at the OLD stack (0xA0007C60).  The processing code
     *  should copy the frame, but it fails because $s6/$s7 (which become
     *  $a2/$a0 via MOVR32) are not properly initialized.
     *
     *  Fix: when the JALX to epilogue 0xAD0 is about to fire from the
     *  callback setup function (SP=0x80008080), copy the saved frame from
     *  the old stack to the callback table area.
     */
    #define EPILOGUE_0AD0_PC    0x9FC00AD0u
    #define CALLBACK_TABLE_SP   0x80008080u
    #define OLD_STACK_FRAME     0xA0007C60u  /* prologue saved here */
    {
        static int frame_copy_done = 0;
        uint32_t pc32 = (uint32_t)cpu->pc;
        /* Detect: we're in the JALX delay slot targeting 0xAD0,
         * and SP is the callback table address. The delay slot
         * at 0x11AC (LI a0, 16) hasn't executed yet when the
         * JALX delay mechanism fires. Check for the instruction
         * just before: 0x11A8 is the JAL, delay slot is 0x11AC. */
        if (!frame_copy_done &&
            (pc32 & 0x3FFF) == 0x11ACu &&
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP] ==
            CALLBACK_TABLE_SP) {
            frame_copy_done = 1;
            /* Copy 40-byte frame: prologue 0xA98 saves 40 bytes
             * (s0-s7, s8, v0 at offsets 0-36).
             * Epilogue reads from SP+a0 where a0=16 (delay slot).
             * So write to 0x80008080+16 = 0x80008090. */
            uint32_t dst_va = CALLBACK_TABLE_SP + 16;
            uint32_t src_va = OLD_STACK_FRAME;
            fprintf(stderr,
                "[FRAME_COPY] copying 40 bytes from 0x%08X"
                " to 0x%08X\n", src_va, dst_va);
            for (int i = 0; i < 40; i += 4) {
                uint8_t buf[4];
                cpu->memory_rw(cpu, cpu->mem,
                    (uint64_t)(src_va + i), buf, 4,
                    MEM_READ, CACHE_DATA);
                cpu->memory_rw(cpu, cpu->mem,
                    (uint64_t)(dst_va + i), buf, 4,
                    MEM_WRITE, CACHE_DATA);
            }
        }
    }
}

void wince_boot_on_vr41xx_tick(struct machine *gxm, struct cpu *cpu)
{
    machine_t *m = wince_boot_from_gx(gxm);

    if (!m || !m->wince.active)
        return;

    scan_low_vectors(m);
    maybe_track_low_vector_runtime_changes(m);
    maybe_note_first_exception(m);
    maybe_log_cold_boot_scheduler_probe(m, (uint32_t)cpu->pc);
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
