#include "wince_boot.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cop0.h"
#include "cpu.h"
#include "cpu_mips.h"
#include "machine.h"
#include "memory.h"
#include "wince_hw_seed_data.h"

static machine_t *g_active_wince_machine = NULL;

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
    unsigned char buf[4];

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

static bool translate_va(machine_t *m, uint32_t va, uint64_t *paddr_out)
{
    uint64_t paddr;

    if (!m || !m->cpu || !m->cpu->translate_v2p)
        return false;
    if (!m->cpu->translate_v2p(m->cpu, va32_to_mips64(va), &paddr,
        FLAG_NOEXCEPTIONS))
        return false;

    if (paddr_out)
        *paddr_out = paddr;
    return true;
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

static const char *format_word_or_unknown(char *buf, size_t buf_size, bool ok,
    uint32_t value)
{
    if (!ok)
        return "????????";

    snprintf(buf, buf_size, "%08X", value);
    return buf;
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

    if (pa < 0x00000400u)
        set_vector_write_observer(m, false);
    for (i = 0; i < word_count; i++)
        store_32bit_word(m->cpu, pa_to_kseg0(pa + (uint32_t)(i * 4u)), words[i]);
    if (pa < 0x00000400u)
        set_vector_write_observer(m, true);
}

static bool allow_pa_seed_region(const char *name, bool resume_only)
{
    if (!name)
        return false;
    if (strncmp(name, "low_sdram_", 10) == 0)
        return true;
    if (strcmp(name, "resume_ctx") == 0)
        return true;
    if (strcmp(name, "ctx_tlb") == 0)
        return true;
    if (!resume_only && strcmp(name, "ctx_high_page") == 0)
        return true;
    return false;
}

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

    if (!m->wince.active || !m->wince.log_stall)
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
            invalidate_all(m);
            if (m->wince.log_stall && !m->wince.low_vector_guest_write_logged) {
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

    if ((m->wince.vector_owner == WINCE_VECTOR_GUEST
        || m->wince.vector_owner == WINCE_VECTOR_SEEDED)
        && ready && !m->wince.vectors_ready) {
        m->wince.vectors_ready = true;
        maybe_log_checkpoint(m, "vector_owner", "guest-low-vectors");
    }
}

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

static void log_va_probe(machine_t *m, const char *label, uint32_t va)
{
    uint32_t value = 0;
    uint64_t paddr = 0;
    bool mapped = translate_va(m, va, &paddr);
    bool readable = load_va_word(m, va, &value);

    fprintf(stderr, "[WINCE_HANDLER] %-12s VA=0x%08X mapped=%d",
        label, va, mapped ? 1 : 0);
    if (mapped)
        fprintf(stderr, " PA=0x%08" PRIx64, paddr);
    if (readable)
        fprintf(stderr, " value=%08X", value);
    else
        fprintf(stderr, " value=????????");
    fprintf(stderr, "\n");
}

static void log_handler_fault(machine_t *m, uint32_t pc_norm,
    uint32_t fault_va, uint32_t exccode, const char *detail)
{
    uint32_t cause;
    uint32_t epc;
    uint32_t index;
    uint32_t random;
    uint32_t entryhi;
    uint32_t t0;
    uint32_t t1;
    uint32_t k0;
    uint32_t k1;
    uint32_t s0;
    uint32_t sp;
    uint32_t ra;
    uint32_t dir_off;
    uint32_t pte_off;
    uint32_t leaf_off;
    uint32_t root_va;
    uint32_t root_entry = 0;
    uint32_t second_va = 0;
    uint32_t second_entry = 0;
    bool root_ok;
    bool second_ok = false;

    if (!m->wince.active || !m->wince.log_stall
        || !m->wince.cold_boot_redirected
        || m->wince.handler_fault_logged)
        return;
    if (pc_norm != 0x8008B6FCu)
        return;

    m->wince.handler_fault_logged = true;
    cause = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE];
    epc = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC];
    index = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_INDEX];
    random = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_RANDOM];
    entryhi = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_ENTRYHI];
    t0 = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_T0];
    t1 = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_T1];
    k0 = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_K0];
    k1 = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_K1];
    s0 = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_S0];
    sp = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP];
    ra = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA];

    dir_off = (fault_va >> 23) & 0xFCu;
    pte_off = (fault_va >> 14) & 0x7FCu;
    leaf_off = (fault_va >> 10) & 0x3Cu;
    root_va = 0xFFFFD8C0u + dir_off;
    root_ok = load_va_word(m, root_va, &root_entry);
    if (root_ok) {
        second_va = root_entry + pte_off;
        second_ok = load_va_word(m, second_va, &second_entry);
    }

    (void)log_checkpoint_header(m, "handler_fault", detail);
    fprintf(stderr,
        "[WINCE_HANDLER] fault_site PC=0x%08X EPC=0x%08X Cause=0x%08X"
        " exccode=%u BadVA=0x%08X Index=0x%08X Random=0x%08X"
        " EntryHi=0x%08X owner=%d ready=%d\n",
        pc_norm,
        epc,
        cause,
        exccode,
        fault_va,
        index,
        random,
        entryhi,
        (int)m->wince.vector_owner,
        m->wince.vectors_ready ? 1 : 0);
    fprintf(stderr,
        "[WINCE_HANDLER] regs t0=0x%08X t1=0x%08X k0=0x%08X k1=0x%08X"
        " s0=0x%08X sp=0x%08X ra=0x%08X\n",
        t0, t1, k0, k1, s0, sp, ra);
    fprintf(stderr,
        "[WINCE_HANDLER] walk dir_off=0x%02X pte_off=0x%03X"
        " leaf_off=0x%02X root_va=0x%08X root=%s second_va=0x%08X"
        " second=%s\n",
        dir_off,
        pte_off,
        leaf_off,
        root_va,
        root_ok ? "ok" : "unreadable",
        second_va,
        second_ok ? "ok" : "unreadable");
    if (root_ok)
        fprintf(stderr, "[WINCE_HANDLER] root_entry=0x%08X\n", root_entry);
    if (second_ok)
        fprintf(stderr, "[WINCE_HANDLER] second_entry=0x%08X\n",
            second_entry);

    log_va_probe(m, "va_d888", 0xFFFFD888u);
    log_va_probe(m, "va_d890", 0xFFFFD890u);
    log_va_probe(m, "va_d89c", 0xFFFFD89Cu);
    log_va_probe(m, "va_d8a8", 0xFFFFD8A8u);
    log_va_probe(m, "va_d8c0", 0xFFFFD8C0u);
    dump_va_window(m, "va_d880", 0xFFFFD880u, 0x30u);
    dump_va_window(m, "va_d8c0", 0xFFFFD8C0u, 0x40u);
}

static void maybe_log_handler_fault(machine_t *m)
{
    uint32_t cause;
    uint32_t epc;
    uint32_t badvaddr;
    uint32_t exccode;
    uint32_t pc_norm;

    if (!m->wince.active || !m->wince.log_stall
        || !m->wince.cold_boot_redirected || m->wince.handler_fault_logged)
        return;

    cause = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE];
    epc = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC];
    badvaddr = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR];
    pc_norm = (uint32_t)m->cpu->pc;
    exccode = (cause & CAUSE_EXCCODE_MASK) >> CAUSE_EXCCODE_SHIFT;

    if (pc_norm != 0x8008B6FCu && epc != 0x8008B6FCu)
        return;

    log_handler_fault(m, epc == 0x8008B6FCu ? epc : pc_norm, badvaddr,
        exccode, "epc-8008b6fc");
}

static bool apply_pa_seed_regions(machine_t *m, bool resume_only)
{
    bool applied = false;
    bool touched_low_vectors = false;
    uint32_t i;

    for (i = 0; i < wince_hw_seed_region_count; i++) {
        const wince_pa_seed_region_t *region = &wince_hw_seed_regions[i];

        if (!allow_pa_seed_region(region->name, resume_only))
            continue;
        if (region->pa + region->size > m->cfg.sdram_size)
            continue;
        if (region->pa < 0x00000400u)
            set_vector_write_observer(m, false);
        store_buf(m->cpu, pa_to_kseg0(region->pa), (const char *)region->data,
            region->size);
        if (region->pa < 0x00000400u)
            set_vector_write_observer(m, true);
        if (region->pa < 0x00000400u)
            touched_low_vectors = true;
        applied = true;
    }

    if (!applied)
        return false;

    invalidate_all(m);
    if (touched_low_vectors) {
        m->wince.vector_owner = WINCE_VECTOR_SEEDED;
        m->wince.vectors_ready = true;
    }
    return true;
}

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
    m->wince.log_stall = m->cfg.log_wince_stall;
    m->wince.use_hw_seed = m->cfg.wince_hw_seed;
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

    if (m->wince.log_stall && reason)
        fprintf(stderr, "[WINCE_CKPT] synthetic_low_vectors reason=%s\n",
            reason);
}

void wince_boot_apply_initial_seed(machine_t *m)
{
    bool applied;

    if (!m->wince.active || !m->wince.use_hw_seed || m->wince.initial_seed_applied)
        return;

    applied = apply_pa_seed_regions(m, false);
    m->wince.initial_seed_applied = true;

    if (m->wince.log_stall) {
        fprintf(stderr,
            "[WINCE_SEED] initial applied=%d pa_regions=%u va_regions=%u\n",
            applied ? 1 : 0,
            wince_hw_seed_region_count,
            wince_hw_vseed_region_count);
    }
}

void wince_boot_apply_resume_seed(machine_t *m)
{
    bool applied;

    if (!m->wince.active || !m->wince.use_hw_seed || m->wince.resume_seed_applied)
        return;

    applied = apply_pa_seed_regions(m, true);
    m->wince.resume_seed_applied = true;

    if (m->wince.log_stall) {
        fprintf(stderr, "[WINCE_SEED] resume applied=%d\n",
            applied ? 1 : 0);
    }
}

void wince_boot_on_vr41xx_tick(struct machine *gxm, struct cpu *cpu)
{
    machine_t *m = wince_boot_from_gx(gxm);
    (void)cpu;

    if (!m || !m->wince.active)
        return;

    scan_low_vectors(m);
    maybe_note_first_exception(m);
    maybe_log_handler_fault(m);
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
    if (m->wince.log_stall) {
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
    if (m->wince.vectors_ready) {
        if (m->wince.log_stall && !m->wince.timer_release_logged) {
            fprintf(stderr, "[WINCE_CKPT] timer_irq_gate released owner=%d\n",
                (int)m->wince.vector_owner);
            m->wince.timer_release_logged = true;
        }
        return true;
    }

    if (m->wince.log_stall && !m->wince.timer_gate_logged) {
        fprintf(stderr, "[WINCE_CKPT] timer_irq_gate active waiting_for_vectors\n");
        m->wince.timer_gate_logged = true;
    }
    return false;
}

bool wince_boot_note_low_reference_fault(struct cpu *cpu, uint64_t vaddr,
    int exccode)
{
    machine_t *m;

    if (!cpu || !cpu->machine)
        return false;

    m = wince_boot_from_gx(cpu->machine);
    if (!m)
        return false;
    if ((uint32_t)cpu->pc != 0x8008B6FCu)
        return false;
    if (m->wince.handler_fault_logged)
        return true;

    log_handler_fault(m, (uint32_t)cpu->pc, (uint32_t)vaddr,
        (uint32_t)exccode, "low-ref-8008b6fc");
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
    if (m->wince.vector_owner != WINCE_VECTOR_GUEST) {
        m->wince.vector_owner = WINCE_VECTOR_GUEST;
        m->wince.vectors_ready = false;
        if (m->wince.log_stall && !m->wince.low_vector_guest_write_logged) {
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
