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
#include "wince_resume_replay_data.h"

static machine_t *g_active_wince_machine = NULL;
static const char *wince_gpr_names[] = MIPS_REGISTER_NAMES;

#define WINCE_EXCCODE_BIT(code) (UINT32_C(1) << (code))

enum {
    WINCE_FAULT_SITE_PTE_WALK = UINT32_C(1) << 0,
    WINCE_FAULT_SITE_NULL_D0  = UINT32_C(1) << 1,
    WINCE_FAULT_SITE_NULL_PC  = UINT32_C(1) << 2,
    WINCE_FAULT_SITE_REPLAY_STUB_MISS = UINT32_C(1) << 3,
};

typedef struct {
    uint32_t pc;
    const char *label;
    int32_t code_start_rel;
    uint32_t code_size;
    uint32_t stack_size;
} wince_replay_pc_probe_desc_t;

typedef struct wince_fault_site_desc wince_fault_site_desc_t;
typedef void (*wince_fault_site_dump_fn)(machine_t *m,
    const wince_fault_site_desc_t *site, uint32_t fault_pc, uint32_t fault_va,
    uint32_t exccode);

static const char *format_word_or_unknown(char *buf, size_t buf_size, bool ok,
    uint32_t value);

typedef struct {
    const char *mnemonic;
    uint32_t rs;
    uint32_t rt;
    int32_t offset;
    bool store;
} wince_memop_info_t;

typedef struct {
    const char *label;
    uint32_t    pa;
    uint32_t    size;
} wince_replay_write_watch_desc_t;

typedef struct {
    const char *label;
    uint32_t    pa;
} wince_mmio_watch_desc_t;

typedef struct {
    uint32_t epc;
    const char *label;
} wince_replay_epc_probe_desc_t;

struct wince_fault_site_desc {
    uint32_t log_bit;
    uint32_t pc;
    uint32_t exccode_mask;
    const char *label;
    wince_fault_site_dump_fn dump;
};

static void log_fault_site_pte_walk(machine_t *m,
    const wince_fault_site_desc_t *site, uint32_t fault_pc, uint32_t fault_va,
    uint32_t exccode);
static void log_fault_site_null_d0(machine_t *m,
    const wince_fault_site_desc_t *site, uint32_t fault_pc, uint32_t fault_va,
    uint32_t exccode);
static void log_fault_site_null_pc(machine_t *m,
    const wince_fault_site_desc_t *site, uint32_t fault_pc, uint32_t fault_va,
    uint32_t exccode);
static void log_fault_site_replay_stub_miss(machine_t *m,
    const wince_fault_site_desc_t *site, uint32_t fault_pc, uint32_t fault_va,
    uint32_t exccode);
static const wince_resume_region_t *find_replay_region_by_name(
    const wince_resume_snapshot_t *snapshot, const char *name);
static void log_replay_region_compare(machine_t *m,
    const wince_resume_region_t *region, const char *label);
static void log_replay_pc_state(machine_t *m, const char *label, uint32_t pc);
static void invalidate_all(machine_t *m);
static bool probable_low_va_target(uint32_t value);
static bool range_overlaps(uint64_t base_a, uint64_t size_a, uint64_t base_b,
    uint64_t size_b);
static void synthesize_word_after_write(machine_t *m, uint32_t word_pa,
    uint64_t write_pa, const unsigned char *new_data, size_t len,
    uint32_t *before_out, uint32_t *after_out);

static const wince_replay_pc_probe_desc_t wince_replay_pc_probes[] = {
    { UINT32_C(0xA00795B4), "resume_oal_entry", -0x10, 0x40u, 0x00u },
    { UINT32_C(0x800895D8), "resume_kernel_895d8", -0x10, 0x40u, 0x00u },
    { UINT32_C(0x8007A114), "resume_kernel_7a114", -0x10, 0x40u, 0x40u },
    { UINT32_C(0x800A5E94), "resume_kernel_a5e94", -0x10, 0x40u, 0x40u },
    { UINT32_C(0x80079724), "resume_kernel_79724", -0x10, 0x40u, 0x40u },
    { UINT32_C(0x00011790), "resume_stub_entry", -0x10, 0x40u, 0x40u },
    { UINT32_C(0x000117A8), "resume_stub_return", -0x10, 0x40u, 0x40u },
    { UINT32_C(0x8008B478), "corridor_8008b478", -0x10, 0x40u, 0x40u },
    { UINT32_C(0x8008B52C), "corridor_8008b52c", -0x10, 0x40u, 0x40u },
    { UINT32_C(0x80094E8C), "corridor_80094e8c", -0x10, 0x40u, 0x40u },
    { UINT32_C(0x8008B254), "loop_8008b254", -0x20, 0x60u, 0x60u },
    { UINT32_C(0x8008B264), "loop_8008b264", -0x20, 0x60u, 0x60u },
    { UINT32_C(0x8008B274), "loop_8008b274", -0x20, 0x60u, 0x60u },
    { UINT32_C(0x8008B278), "loop_8008b278", -0x20, 0x60u, 0x60u },
    { UINT32_C(0x8008B3F0), "loop_8008b3f0", -0x20, 0x60u, 0x60u },
    { UINT32_C(0x8008B42C), "loop_8008b42c", -0x20, 0x60u, 0x60u },
    { UINT32_C(0x8008B6D8), "loop_8008b6d8", -0x20, 0x60u, 0x60u },
    { UINT32_C(0x8008B8EC), "loop_8008b8ec", -0x20, 0x60u, 0x60u },
    { UINT32_C(0x8008B994), "loop_8008b994", -0x20, 0x60u, 0x60u },
    { UINT32_C(0x80094F24), "loop_80094f24", -0x20, 0x60u, 0x60u },
    { UINT32_C(0x80095004), "loop_80095004", -0x20, 0x60u, 0x60u },
    { UINT32_C(0x8009511C), "loop_8009511c", -0x20, 0x60u, 0x60u },
    { UINT32_C(0x800953D4), "loop_800953d4", -0x20, 0x60u, 0x60u },
    { UINT32_C(0x80096C40), "loop_80096c40", -0x20, 0x60u, 0x60u },
    { UINT32_C(0x800A7B3C), "resume_poll_7b3c", -0x20, 0x80u, 0x60u },
    { UINT32_C(0x800A7B64), "resume_poll_7b64", -0x20, 0x80u, 0x60u },
    { UINT32_C(0x800A7B68), "resume_poll_7b68", -0x20, 0x80u, 0x60u },
    { UINT32_C(0x800A7BC4), "resume_poll_7bc4", -0x20, 0x80u, 0x60u },
    { UINT32_C(0x800A7BC8), "resume_poll_7bc8", -0x20, 0x80u, 0x60u },
    { UINT32_C(0xBFC00000), "bev_refill_000", -0x10, 0x40u, 0x00u },
    { UINT32_C(0xBFC00200), "bev_general_200", -0x10, 0x40u, 0x00u },
    { UINT32_C(0xBFC00380), "bev_interrupt_380", -0x10, 0x40u, 0x00u },
};

static const wince_fault_site_desc_t wince_fault_sites[] = {
    {
        WINCE_FAULT_SITE_PTE_WALK,
        UINT32_C(0x8008B6FC),
        WINCE_EXCCODE_BIT(EXCEPTION_TLBL) | WINCE_EXCCODE_BIT(EXCEPTION_TLBS),
        "pte-walk",
        log_fault_site_pte_walk,
    },
    {
        WINCE_FAULT_SITE_NULL_D0,
        UINT32_C(0x8008B28C),
        WINCE_EXCCODE_BIT(EXCEPTION_TLBL) | WINCE_EXCCODE_BIT(EXCEPTION_TLBS),
        "null-d0",
        log_fault_site_null_d0,
    },
    {
        WINCE_FAULT_SITE_NULL_PC,
        UINT32_C(0x00000000),
        WINCE_EXCCODE_BIT(EXCEPTION_TLBL) | WINCE_EXCCODE_BIT(EXCEPTION_TLBS),
        "null-pc",
        log_fault_site_null_pc,
    },
    {
        WINCE_FAULT_SITE_REPLAY_STUB_MISS,
        UINT32_C(0x000117A8),
        WINCE_EXCCODE_BIT(EXCEPTION_TLBL) | WINCE_EXCCODE_BIT(EXCEPTION_TLBS),
        "replay-stub-miss",
        log_fault_site_replay_stub_miss,
    },
};

static const wince_replay_write_watch_desc_t wince_replay_write_watches[] = {
    { "dispatch_page_5100", UINT32_C(0x00051000), UINT32_C(0x0800) },
    { "bootctx_counter_57000", UINT32_C(0x00057000), UINT32_C(0x0020) },
    { "dispatch_ptr_660160", UINT32_C(0x00660160), UINT32_C(0x00A0) },
    { "bootctx_stub_63d0", UINT32_C(0x000063D0), UINT32_C(0x0040) },
    { "obj_table_66bfc0", UINT32_C(0x0066BFC0), UINT32_C(0x0040) },
    { "callback_table_1740", UINT32_C(0x00051740), UINT32_C(0x0040) },
};

static const wince_mmio_watch_desc_t wince_mmio_watches[] = {
    { "bootctx_aa000630", UINT32_C(0x0A000630) },
    { "bootctx_aa000634", UINT32_C(0x0A000634) },
    { "bootctx_aa000638", UINT32_C(0x0A000638) },
    { "bootctx_aa00063c", UINT32_C(0x0A00063C) },
    { "status_1b00", UINT32_C(0x0A001B00) },
    { "status_1b10", UINT32_C(0x0A001B10) },
    { "status_1b50", UINT32_C(0x0A001B50) },
    { "ack_1120", UINT32_C(0x0A001120) },
    { "ack_112c", UINT32_C(0x0A00112C) },
    { "ack_1b20", UINT32_C(0x0A001B20) },
    { "ack_1b2c", UINT32_C(0x0A001B2C) },
    { "bootctx_aa001b54", UINT32_C(0x0A001B54) },
    { "bootctx_aa001b58", UINT32_C(0x0A001B58) },
    { "bootctx_aa000028", UINT32_C(0x0A000028) },
    { "bootctx_aa01a0e4", UINT32_C(0x0A01A0E4) },
};

static const wince_replay_epc_probe_desc_t wince_replay_bootctx_epc_probes[] = {
    { UINT32_C(0xA00063D4), "bootctx_epc_63d4" },
    { UINT32_C(0xA00063DC), "bootctx_epc_63dc" },
    { UINT32_C(0xA00063E4), "bootctx_epc_63e4" },
    { UINT32_C(0xA00063EC), "bootctx_epc_63ec" },
    { UINT32_C(0xA00063F4), "bootctx_epc_63f4" },
    { UINT32_C(0xA0006404), "bootctx_epc_6404" },
};

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

static void dump_pa_window(machine_t *m, const char *label, uint32_t pa,
    uint32_t size)
{
    uint32_t off;

    for (off = 0; off < size; off += 16u) {
        fprintf(stderr,
            "[WINCE_HANDLER] %s+0x%03X PA=0x%08X: %08X %08X %08X %08X\n",
            label,
            off,
            pa + off,
            load_pa_word(m, pa + off + 0u),
            load_pa_word(m, pa + off + 4u),
            load_pa_word(m, pa + off + 8u),
            load_pa_word(m, pa + off + 12u));
    }
}

static void dump_vrc4173_latch_window(const char *label, uint32_t pa,
    uint32_t size)
{
    uint32_t off;

    for (off = 0; off < size; off += 16u) {
        uint32_t w0 = 0;
        uint32_t w1 = 0;
        uint32_t w2 = 0;
        uint32_t w3 = 0;
        bool ok0 = be300_vrc4173_latch_read_u32(pa + off + 0u, &w0);
        bool ok1 = be300_vrc4173_latch_read_u32(pa + off + 4u, &w1);
        bool ok2 = be300_vrc4173_latch_read_u32(pa + off + 8u, &w2);
        bool ok3 = be300_vrc4173_latch_read_u32(pa + off + 12u, &w3);
        char b0[9];
        char b1[9];
        char b2[9];
        char b3[9];

        fprintf(stderr,
            "[WINCE_HANDLER] %s+0x%03X PA=0x%08X: %s %s %s %s\n",
            label,
            off,
            pa + off,
            format_word_or_unknown(b0, sizeof(b0), ok0, w0),
            format_word_or_unknown(b1, sizeof(b1), ok1, w1),
            format_word_or_unknown(b2, sizeof(b2), ok2, w2),
            format_word_or_unknown(b3, sizeof(b3), ok3, w3));
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

static bool decode_memop(uint32_t insn, wince_memop_info_t *out)
{
    uint32_t opcode = (insn >> 26) & 0x3Fu;
    const char *mnemonic = NULL;
    bool store = false;

    switch (opcode) {
    case 0x20: mnemonic = "lb"; break;
    case 0x21: mnemonic = "lh"; break;
    case 0x22: mnemonic = "lwl"; break;
    case 0x23: mnemonic = "lw"; break;
    case 0x24: mnemonic = "lbu"; break;
    case 0x25: mnemonic = "lhu"; break;
    case 0x26: mnemonic = "lwr"; break;
    case 0x28: mnemonic = "sb"; store = true; break;
    case 0x29: mnemonic = "sh"; store = true; break;
    case 0x2A: mnemonic = "swl"; store = true; break;
    case 0x2B: mnemonic = "sw"; store = true; break;
    case 0x30: mnemonic = "ll"; break;
    case 0x31: mnemonic = "lwc1"; break;
    case 0x35: mnemonic = "ldc1"; break;
    case 0x38: mnemonic = "sc"; store = true; break;
    case 0x39: mnemonic = "swc1"; store = true; break;
    case 0x3D: mnemonic = "sdc1"; store = true; break;
    default:
        return false;
    }

    if (!out)
        return true;

    out->mnemonic = mnemonic;
    out->rs = (insn >> 21) & 0x1Fu;
    out->rt = (insn >> 16) & 0x1Fu;
    out->offset = (int32_t)(int16_t)(insn & 0xFFFFu);
    out->store = store;
    return true;
}

static void log_memop_probe(machine_t *m, uint32_t fault_pc, uint32_t fault_va)
{
    uint32_t insn = 0;
    wince_memop_info_t memop;

    if (!load_va_word(m, fault_pc, &insn)) {
        fprintf(stderr,
            "[WINCE_FAULT] decode site=0x%08X instruction=unreadable\n",
            fault_pc);
        return;
    }

    if (!decode_memop(insn, &memop)) {
        fprintf(stderr,
            "[WINCE_FAULT] decode site=0x%08X instruction=0x%08X"
            " opcode=0x%02X unhandled\n",
            fault_pc,
            insn,
            (insn >> 26) & 0x3Fu);
        return;
    }

    {
        uint32_t base_value = (uint32_t)m->cpu->cd.mips.gpr[memop.rs];
        uint32_t rt_value = (uint32_t)m->cpu->cd.mips.gpr[memop.rt];
        uint32_t eff_addr =
            (uint32_t)((int32_t)base_value + (int32_t)memop.offset);

        fprintf(stderr,
            "[WINCE_FAULT] decode site=0x%08X insn=0x%08X %s"
            " base=%s(0x%08X) rt=%s(0x%08X) off=%d eff=0x%08X"
            " badva_match=%d store=%d\n",
            fault_pc,
            insn,
            memop.mnemonic,
            wince_gpr_names[memop.rs],
            base_value,
            wince_gpr_names[memop.rt],
            rt_value,
            memop.offset,
            eff_addr,
            eff_addr == fault_va ? 1 : 0,
            memop.store ? 1 : 0);

        if (base_value != 0) {
            dump_va_window(m, "base_ptr", base_value & ~UINT32_C(0x0F),
                0x40u);
            if ((eff_addr & ~UINT32_C(0x0F))
                != (base_value & ~UINT32_C(0x0F))) {
                dump_va_window(m, "target_va", eff_addr & ~UINT32_C(0x0F),
                    0x20u);
            }
        }
    }
}

static bool decode_tlb_match(machine_t *m, const struct mips_tlb *tlb,
    uint32_t va, uint64_t *pa_out, bool *valid_out, bool *dirty_out,
    bool *global_out, bool *odd_out, uint32_t *page_mask_out)
{
    /*
     * BE-300 uses a VR4131 (R4100-style MMU4K). Mirror GXemul's live
     * translation rules so the diagnostic matches the actual fault path.
     */
    const uint64_t vpn2_mask = ENTRYHI_R_MASK | ENTRYHI_VPN2_MASK
        | UINT64_C(0x1800);
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
    uint32_t page_mask;
    bool odd;
    bool global;
    bool valid;
    bool dirty;

    if (!m || !m->cpu || !m->cpu->cd.mips.coproc[0] || !tlb)
        return false;

    pmask = (uint32_t)tlb->mask & pagemask_mask;
    cached_hi = tlb->hi;
    cached_lo0 = tlb->lo0;
    cached_lo1 = tlb->lo1;

    if (pmask == 0) {
        entry_vpn2 = (cached_hi & vpn2_mask) >> pagemask_shift;
        vaddr_vpn2 = (((uint64_t)va) & vpn2_mask) >> pagemask_shift;
        page_mask = (UINT32_C(1) << (pagemask_shift - 1)) - 1u;
        odd = ((uint32_t)va >> (pagemask_shift - 1)) & 1u;
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
        vaddr_vpn2 = (((uint64_t)va) & vpn2_mask) >> (pageshift + 1);
        page_mask = (UINT32_C(1) << pageshift) - 1u;
        odd = ((uint32_t)va >> pageshift) & 1u;
    }

    entry_asid = cached_hi & ENTRYHI_ASID;
    current_asid = m->cpu->cd.mips.coproc[0]->reg[COP0_ENTRYHI] & ENTRYHI_ASID;
    global = (cached_lo0 & ENTRYLO_G) != 0 && (cached_lo1 & ENTRYLO_G) != 0;

    if (entry_vpn2 != vaddr_vpn2)
        return false;
    if (entry_asid != current_asid && !global)
        return false;

    lo = odd ? cached_lo1 : cached_lo0;
    valid = (lo & ENTRYLO_V) != 0;
    dirty = (lo & ENTRYLO_D) != 0;
    pfn = (lo & ENTRYLO_PFN_MASK) >> ENTRYLO_PFN_SHIFT;
    paddr = ((pfn << pfn_shift) & ~((uint64_t)page_mask))
        | (((uint64_t)va) & page_mask);

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
    return true;
}

static void dump_tlb_matches(machine_t *m, const char *label, uint32_t va)
{
    struct mips_coproc *cp0;
    uint32_t asid;
    int i;
    int matches = 0;
    int raw_dumped = 0;

    if (!m || !m->cpu || !m->cpu->cd.mips.coproc[0])
        return;

    cp0 = m->cpu->cd.mips.coproc[0];
    asid = (uint32_t)cp0->reg[COP0_ENTRYHI] & ENTRYHI_ASID;

    fprintf(stderr,
        "[WINCE_TLB] label=%s va=0x%08X current_entryhi=0x%08X asid=0x%02X"
        " tlbs=%d\n",
        label ? label : "-",
        va,
        (uint32_t)cp0->reg[COP0_ENTRYHI],
        asid,
        cp0->nr_of_tlbs);

    for (i = 0; i < cp0->nr_of_tlbs; i++) {
        uint64_t pa = 0;
        bool valid = false;
        bool dirty = false;
        bool global = false;
        bool odd = false;
        uint32_t page_mask = 0;

        if (!decode_tlb_match(m, &cp0->tlbs[i], va, &pa, &valid, &dirty,
            &global, &odd, &page_mask)) {
            continue;
        }

        fprintf(stderr,
            "[WINCE_TLB] label=%s match idx=%02d hi=0x%08X lo0=0x%08X"
            " lo1=0x%08X mask=0x%08X odd=%d valid=%d dirty=%d global=%d"
            " page_mask=0x%03X pa=0x%08" PRIx64 "\n",
            label ? label : "-",
            i,
            (uint32_t)cp0->tlbs[i].hi,
            (uint32_t)cp0->tlbs[i].lo0,
            (uint32_t)cp0->tlbs[i].lo1,
            (uint32_t)cp0->tlbs[i].mask,
            odd ? 1 : 0,
            valid ? 1 : 0,
            dirty ? 1 : 0,
            global ? 1 : 0,
            page_mask,
            pa);
        matches++;
    }

    if (matches != 0)
        return;

    fprintf(stderr, "[WINCE_TLB] label=%s no-match\n", label ? label : "-");
    for (i = 0; i < cp0->nr_of_tlbs; i++) {
        uint32_t hi = (uint32_t)cp0->tlbs[i].hi;
        uint32_t lo0 = (uint32_t)cp0->tlbs[i].lo0;
        uint32_t lo1 = (uint32_t)cp0->tlbs[i].lo1;
        uint32_t mask = (uint32_t)cp0->tlbs[i].mask;
        bool interesting = hi != 0 || lo0 != 0 || lo1 != 0 || mask != 0;

        if (!interesting)
            continue;
        if (raw_dumped >= 12)
            break;
        if (raw_dumped >= 8 && hi < UINT32_C(0xFFFF8000))
            continue;

        fprintf(stderr,
            "[WINCE_TLB] label=%s raw idx=%02d hi=0x%08X lo0=0x%08X"
            " lo1=0x%08X mask=0x%08X\n",
            label ? label : "-",
            i,
            hi,
            lo0,
            lo1,
            mask);
        raw_dumped++;
    }
}

static bool replay_mode_enabled(machine_t *m)
{
    return m && m->wince.active && m->wince.use_resume_replay
        && m->wince.resume_mode == WINCE_RESUME_MODE_REPLAY;
}

static bool replay_region_word(machine_t *m, const wince_resume_region_t *region,
    uint32_t pa, uint32_t *value_out, bool *valid_out)
{
    uint32_t word_index;

    (void)m;
    if (!region || pa < region->pa || pa >= region->pa + region->size)
        return false;
    if (((pa - region->pa) & UINT32_C(0x3)) != 0)
        return false;

    word_index = (pa - region->pa) / 4u;
    if (word_index >= region->word_count)
        return false;

    if (value_out)
        *value_out = region->words[word_index];
    if (valid_out)
        *valid_out = region->valid_words[word_index] != 0;
    return true;
}

static const wince_resume_region_t *find_replay_region_by_pa(
    const wince_resume_snapshot_t *snapshot, uint32_t pa)
{
    uint32_t i;

    if (!snapshot)
        return NULL;

    for (i = 0; i < snapshot->region_count; i++) {
        const wince_resume_region_t *region = &snapshot->regions[i];

        if (pa >= region->pa && pa < region->pa + region->size)
            return region;
    }

    return NULL;
}

static const wince_resume_region_t *find_replay_region_by_name(
    const wince_resume_snapshot_t *snapshot, const char *name)
{
    uint32_t i;

    if (!snapshot || !name)
        return NULL;

    for (i = 0; i < snapshot->region_count; i++) {
        const wince_resume_region_t *region = &snapshot->regions[i];

        if (region->name && strcmp(region->name, name) == 0)
            return region;
    }

    return NULL;
}

static uint32_t replay_region_index(const wince_resume_snapshot_t *snapshot,
    const wince_resume_region_t *region)
{
    return (uint32_t)(region - snapshot->regions);
}

static void log_replay_region_compare(machine_t *m,
    const wince_resume_region_t *region, const char *label)
{
    uint32_t off;

    if (!m->wince.log_stall || !region)
        return;

    for (off = 0; off < region->size; off += 16u) {
        char e0[9];
        char e1[9];
        char e2[9];
        char e3[9];
        uint32_t exp0 = 0, exp1 = 0, exp2 = 0, exp3 = 0;
        uint32_t live0 = load_pa_word(m, region->pa + off + 0u);
        uint32_t live1 = load_pa_word(m, region->pa + off + 4u);
        uint32_t live2 = load_pa_word(m, region->pa + off + 8u);
        uint32_t live3 = load_pa_word(m, region->pa + off + 12u);
        bool v0 = false, v1 = false, v2 = false, v3 = false;

        (void)replay_region_word(m, region, region->pa + off + 0u, &exp0, &v0);
        (void)replay_region_word(m, region, region->pa + off + 4u, &exp1, &v1);
        (void)replay_region_word(m, region, region->pa + off + 8u, &exp2, &v2);
        (void)replay_region_word(m, region, region->pa + off + 12u, &exp3, &v3);

        fprintf(stderr,
            "[WINCE_REPLAY_CMP] region=%s label=%s off=0x%03X"
            " expected=%s/%s/%s/%s live=%08X/%08X/%08X/%08X"
            " mask=%d%d%d%d\n",
            region->name,
            label ? label : "-",
            off,
            format_word_or_unknown(e0, sizeof(e0), v0, exp0),
            format_word_or_unknown(e1, sizeof(e1), v1, exp1),
            format_word_or_unknown(e2, sizeof(e2), v2, exp2),
            format_word_or_unknown(e3, sizeof(e3), v3, exp3),
            live0, live1, live2, live3,
            v0 ? 1 : 0, v1 ? 1 : 0, v2 ? 1 : 0, v3 ? 1 : 0);
    }
}

static uint32_t ctz32_nonzero(uint32_t value)
{
#if defined(__clang__) || defined(__GNUC__)
    return (uint32_t)__builtin_ctz(value);
#else
    uint32_t bit = 0;

    while (((value >> bit) & 1u) == 0)
        bit++;
    return bit;
#endif
}

static void log_replay_watch_write(machine_t *m, const char *label,
    uint32_t word_pa, uint32_t before, uint32_t after)
{
    fprintf(stderr,
        "[WINCE_REPLAY_WATCH] label=%s paddr=0x%08X before=0x%08X"
        " after=0x%08X pc=0x%08" PRIx64 "\n",
        label ? label : "-",
        word_pa,
        before,
        after,
        (uint64_t)m->cpu->pc);
}

static void maybe_log_replay_dispatch_state(machine_t *m, uint32_t pc)
{
    const wince_resume_region_t *ptr_region;
    const wince_resume_region_t *page_region;
    uint32_t sysint1 = 0;
    uint32_t sysint2 = 0;
    uint32_t giuintl = 0;
    bool sysint1_ok;
    bool sysint2_ok;
    bool giuintl_ok;
    uint32_t dispatch_index = UINT32_MAX;
    uint32_t dispatch_bit = UINT32_MAX;
    uint32_t ptr_pa = 0;
    uint32_t live_ptr = 0;
    uint32_t replay_ptr = 0;
    uint32_t slot_va = 0;
    uint32_t live_slot = 0;
    uint32_t replay_slot = 0;
    bool replay_ptr_ok = false;
    bool live_slot_ok = false;
    bool replay_slot_ok = false;
    const char *source = "none";

    if (!m->wince.log_stall)
        return;

    sysint1_ok = load_va_word(m, UINT32_C(0xAF000080), &sysint1);
    sysint2_ok = load_va_word(m, UINT32_C(0xAF0000A0), &sysint2);
    giuintl_ok = load_va_word(m, UINT32_C(0xAF000088), &giuintl);
    ptr_region = find_replay_region_by_name(&wince_resume_replay_snapshot,
        "dispatch_ptr_table_660170");
    page_region = find_replay_region_by_name(&wince_resume_replay_snapshot,
        "callback_table_a0051680");

    if (sysint1_ok && sysint1 != 0 && (sysint1 & (sysint1 - 1u)) == 0) {
        dispatch_bit = ctz32_nonzero(sysint1);
        source = "sysint1";
        /*
         * The active replay loop consistently arrives here with the ETIMER
         * source mapped to the first dispatch page. Keep the decode narrow
         * and log the raw bit alongside the derived slot.
         */
        if (sysint1 == UINT32_C(0x00000004) || sysint1 == UINT32_C(0x00000008))
            dispatch_index = 0;
        else if (dispatch_bit >= 2u)
            dispatch_index = dispatch_bit - 2u;
    } else if (sysint2_ok && sysint2 != 0 && (sysint2 & (sysint2 - 1u)) == 0) {
        dispatch_bit = ctz32_nonzero(sysint2);
        dispatch_index = dispatch_bit;
        source = "sysint2";
    } else if (giuintl_ok && giuintl != 0
        && (giuintl & (giuintl - 1u)) == 0) {
        dispatch_bit = ctz32_nonzero(giuintl);
        dispatch_index = dispatch_bit;
        source = "giu";
    }

    if (ptr_region && dispatch_index != UINT32_MAX
        && dispatch_index < ptr_region->word_count) {
        ptr_pa = ptr_region->pa + dispatch_index * 4u;
        live_ptr = load_pa_word(m, ptr_pa);
        (void)replay_region_word(m, ptr_region, ptr_pa, &replay_ptr,
            &replay_ptr_ok);
    }

    if (live_ptr != 0)
        slot_va = live_ptr;
    else if (replay_ptr_ok)
        slot_va = replay_ptr;

    if (slot_va != 0)
        live_slot_ok = load_va_word(m, slot_va, &live_slot);
    if (page_region && slot_va != 0
        && (slot_va & UINT32_C(0x1FFFFFFF)) >= page_region->pa
        && (slot_va & UINT32_C(0x1FFFFFFF)) < page_region->pa + page_region->size) {
        (void)replay_region_word(m, page_region,
            slot_va & UINT32_C(0x1FFFFFFF), &replay_slot, &replay_slot_ok);
    }

    {
        char ptr_buf[9];
        char slot_buf[9];
        char replay_slot_buf[9];

        fprintf(stderr,
            "[WINCE_REPLAY_DISPATCH] pc=0x%08X sysint1=%s(0x%08X)"
            " sysint2=%s(0x%08X) giuintl=%s(0x%08X) source=%s",
            pc,
            sysint1_ok ? "ok" : "bad", sysint1,
            sysint2_ok ? "ok" : "bad", sysint2,
            giuintl_ok ? "ok" : "bad", giuintl,
            source);
        if (dispatch_bit != UINT32_MAX)
            fprintf(stderr, " bit=%u", dispatch_bit);
        if (dispatch_index != UINT32_MAX)
            fprintf(stderr, " table_index=%u", dispatch_index);
        if (ptr_pa != 0)
            fprintf(stderr, " ptr_pa=0x%08X", ptr_pa);
        fprintf(stderr, " live_ptr=0x%08X replay_ptr=%s",
            live_ptr,
            format_word_or_unknown(ptr_buf, sizeof(ptr_buf), replay_ptr_ok,
                replay_ptr));
        if (slot_va != 0)
            fprintf(stderr, " slot_va=0x%08X", slot_va);
        fprintf(stderr, " live_slot=%s replay_slot=%s\n",
            format_word_or_unknown(slot_buf, sizeof(slot_buf), live_slot_ok,
                live_slot),
            format_word_or_unknown(replay_slot_buf, sizeof(replay_slot_buf),
                replay_slot_ok,
                replay_slot));
    }
    if (ptr_pa != 0) {
        fprintf(stderr,
            "[WINCE_REPLAY_DISPATCH] detail ptr_pa=0x%08X live_ptr=0x%08X",
            ptr_pa, live_ptr);
        if (replay_ptr_ok)
            fprintf(stderr, " replay_ptr=0x%08X", replay_ptr);
        else
            fprintf(stderr, " replay_ptr=????????");
        fprintf(stderr, "\n");
    }
    if (slot_va != 0) {
        fprintf(stderr,
            "[WINCE_REPLAY_DISPATCH] detail slot_va=0x%08X", slot_va);
        if (live_slot_ok)
            fprintf(stderr, " live_slot=0x%08X", live_slot);
        else
            fprintf(stderr, " live_slot=????????");
        if (replay_slot_ok)
            fprintf(stderr, " replay_slot=0x%08X", replay_slot);
        else
            fprintf(stderr, " replay_slot=????????");
        fprintf(stderr, "\n");
    }

    dump_va_window(m, "dispatch_a0057000", UINT32_C(0xA0057000), 0x20u);
    dump_pa_window(m, "dispatch_57000_pa", UINT32_C(0x00057000), 0x20u);
    dump_vrc4173_latch_window("dispatch_aa000020_raw",
        UINT32_C(0x0A000020), 0x20u);
    dump_vrc4173_latch_window("dispatch_aa000630_raw",
        UINT32_C(0x0A000630), 0x10u);
    dump_vrc4173_latch_window("dispatch_aa001b00_raw",
        UINT32_C(0x0A001B00), 0x60u);
    dump_vrc4173_latch_window("dispatch_aa01a0e0_raw",
        UINT32_C(0x0A01A0E0), 0x20u);

    if (ptr_region)
        log_replay_region_compare(m, ptr_region, "dispatch-ptr");
    if (page_region)
        log_replay_region_compare(m, page_region, "dispatch-page");
}

static bool in_kseg0_or_kseg1(uint32_t va)
{
    return (va & UINT32_C(0xE0000000)) == UINT32_C(0x80000000)
        || (va & UINT32_C(0xE0000000)) == UINT32_C(0xA0000000);
}

static bool plausible_callback_target(uint32_t va)
{
    return va != 0 && (va & UINT32_C(0x3)) == 0
        && (in_kseg0_or_kseg1(va) || probable_low_va_target(va));
}

static bool replay_pc_in_bootctx_loop_corridor(uint32_t pc)
{
    return pc == UINT32_C(0x8008B240)
        || pc == UINT32_C(0x8008B254)
        || pc == UINT32_C(0x8008B264)
        || pc == UINT32_C(0x8008B274)
        || pc == UINT32_C(0x8008B278)
        || pc == UINT32_C(0x8008B3F0);
}

static void log_bootctx_stub_poststate(machine_t *m, const char *label)
{
    const wince_resume_region_t *bootctx_region;
    uint32_t pc;

    if (!m || !m->wince.log_stall)
        return;

    pc = (uint32_t)m->cpu->pc;
    log_replay_pc_state(m, label ? label : "bootctx_poststate", pc);
    dump_va_window(m, "bootctx_stub_code", UINT32_C(0xA00063D0), 0x40u);
    dump_pa_window(m, "bootctx_stub_pa", UINT32_C(0x000063D0), 0x40u);
    dump_va_window(m, "bootctx_a0057000", UINT32_C(0xA0057000), 0x20u);
    dump_pa_window(m, "bootctx_57000_pa", UINT32_C(0x00057000), 0x20u);
    dump_vrc4173_latch_window("bootctx_aa000020_raw",
        UINT32_C(0x0A000020), 0x20u);
    dump_vrc4173_latch_window("bootctx_aa000630_raw",
        UINT32_C(0x0A000630), 0x10u);
    dump_vrc4173_latch_window("bootctx_aa001b00_raw",
        UINT32_C(0x0A001B00), 0x60u);
    dump_vrc4173_latch_window("bootctx_aa01a0e0_raw",
        UINT32_C(0x0A01A0E0), 0x20u);
    bootctx_region = find_replay_region_by_name(&wince_resume_replay_snapshot,
        "bootctx_stub_63d0");
    if (bootctx_region)
        log_replay_region_compare(m, bootctx_region, label);
}

static void maybe_log_replay_bootctx_stub_poststate(machine_t *m)
{
    uint32_t pc;
    uint32_t epc;

    if (!m || !replay_mode_enabled(m) || !m->wince.cold_boot_redirected
        || !m->wince.log_stall || m->wince.replay_bootctx_stub_poststate_logged
        || !m->wince.replay_callback_redirect_attempted) {
        return;
    }

    pc = (uint32_t)m->cpu->pc;
    if (!replay_pc_in_bootctx_loop_corridor(pc))
        return;

    epc = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC];
    if (epc < UINT32_C(0xA00063D0) || epc >= UINT32_C(0xA0006410))
        return;

    m->wince.replay_bootctx_stub_poststate_logged = true;
    log_bootctx_stub_poststate(m, "bootctx_poststate");
}

static void log_bootctx_stub_epc_probe(machine_t *m, const char *label,
    uint32_t epc)
{
    uint32_t pc;
    uint32_t sp;
    uint32_t epc_pa;
    char code_label[64];
    char pa_label[64];
    char stack_label[64];

    if (!m || !m->wince.log_stall)
        return;

    pc = (uint32_t)m->cpu->pc;
    sp = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP];
    epc_pa = epc & UINT32_C(0x1FFFFFFF);

    log_replay_pc_state(m, label ? label : "bootctx_epc", pc);
    fprintf(stderr,
        "[WINCE_BOOTCTX] label=%s live_pc=0x%08X epc=0x%08X epc_pa=0x%08X"
        " ra=0x%08X sp=0x%08X\n",
        label ? label : "-",
        pc,
        epc,
        epc_pa,
        (uint32_t)m->cpu->cd.mips.gpr[31],
        sp);

    snprintf(code_label, sizeof(code_label), "%s_code",
        label ? label : "bootctx_epc");
    dump_va_window(m, code_label, epc & ~UINT32_C(0x1F), 0x40u);

    snprintf(pa_label, sizeof(pa_label), "%s_pa",
        label ? label : "bootctx_epc");
    dump_pa_window(m, pa_label, epc_pa & ~UINT32_C(0x1F), 0x40u);

    if (sp != 0) {
        snprintf(stack_label, sizeof(stack_label), "%s_stack",
            label ? label : "bootctx_epc");
        dump_va_window(m, stack_label, sp - UINT32_C(0x20), 0x40u);
    }

    dump_vrc4173_latch_window("bootctx_aa000020_raw",
        UINT32_C(0x0A000020), 0x20u);
    dump_vrc4173_latch_window("bootctx_aa000630_raw",
        UINT32_C(0x0A000630), 0x10u);
    dump_vrc4173_latch_window("bootctx_aa001b00_raw",
        UINT32_C(0x0A001B00), 0x60u);
    dump_vrc4173_latch_window("bootctx_aa01a0e0_raw",
        UINT32_C(0x0A01A0E0), 0x20u);
}

static void maybe_log_replay_bootctx_stub_epc_probe(machine_t *m)
{
    size_t i;
    uint32_t pc;
    uint32_t epc;

    if (!m || !replay_mode_enabled(m) || !m->wince.cold_boot_redirected
        || !m->wince.log_stall || !m->wince.replay_callback_redirect_attempted) {
        return;
    }

    pc = (uint32_t)m->cpu->pc;
    if (!replay_pc_in_bootctx_loop_corridor(pc))
        return;

    epc = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC];
    for (i = 0; i < sizeof(wince_replay_bootctx_epc_probes)
        / sizeof(wince_replay_bootctx_epc_probes[0]); i++) {
        const wince_replay_epc_probe_desc_t *probe =
            &wince_replay_bootctx_epc_probes[i];
        uint32_t bit = UINT32_C(1) << i;

        if (probe->epc != epc)
            continue;
        if ((m->wince.replay_bootctx_epc_probe_logged_mask & bit) != 0)
            return;

        m->wince.replay_bootctx_epc_probe_logged_mask |= bit;
        log_bootctx_stub_epc_probe(m, probe->label, epc);
        return;
    }
}

static void maybe_probe_replay_callback_redirect(machine_t *m)
{
    uint32_t pc;
    uint32_t epc;
    uint32_t sp;
    uint32_t stack_target = 0;
    uint32_t slot_addr = 0;
    uint32_t slot_value = 0;
    bool stack_target_ok;
    bool slot_addr_ok;
    bool slot_value_ok = false;
    bool slot_addr_in_table = false;
    uint32_t slot_index = UINT32_MAX;
    uint32_t redirect_target = 0;
    const wince_resume_region_t *dispatch_ptr_region;
    const wince_resume_region_t *callback_region;
    const wince_resume_region_t *bootctx_region;

    if (!replay_mode_enabled(m) || !m->wince.cold_boot_redirected
        || !m->wince.log_stall || m->wince.replay_callback_redirect_attempted) {
        return;
    }

    pc = (uint32_t)m->cpu->pc;
    if (pc != UINT32_C(0x8008B254) && pc != UINT32_C(0x8008B3F0))
        return;

    epc = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC];
    if (epc < UINT32_C(0xA0051680) || epc >= UINT32_C(0xA0051880))
        return;

    sp = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP];
    stack_target_ok = load_va_word(m, sp + UINT32_C(0x08), &stack_target);
    slot_addr_ok = load_va_word(m, sp + UINT32_C(0x0C), &slot_addr);
    if (slot_addr_ok && slot_addr >= UINT32_C(0xA0051680)
        && slot_addr < UINT32_C(0xA0051880)) {
        slot_addr_in_table = true;
        slot_index = (slot_addr - UINT32_C(0xA0051680)) / 4u;
        slot_value_ok = load_va_word(m, slot_addr, &slot_value);
    }

    dispatch_ptr_region = find_replay_region_by_name(&wince_resume_replay_snapshot,
        "dispatch_ptr_table_660170");
    callback_region = find_replay_region_by_name(&wince_resume_replay_snapshot,
        "callback_table_a0051680");
    bootctx_region = find_replay_region_by_name(&wince_resume_replay_snapshot,
        "bootctx_stub_63d0");

    fprintf(stderr,
        "[WINCE_REPLAY_CALLBACK] pc=0x%08X epc=0x%08X sp=0x%08X"
        " stack_target=%s slot_addr=%s",
        pc,
        epc,
        sp,
        stack_target_ok ? "ok" : "bad",
        slot_addr_ok ? "ok" : "bad");
    if (stack_target_ok)
        fprintf(stderr, "(0x%08X)", stack_target);
    if (slot_addr_ok)
        fprintf(stderr, "(0x%08X)", slot_addr);
    if (slot_addr_in_table)
        fprintf(stderr, " slot_index=%u", slot_index);
    if (slot_value_ok)
        fprintf(stderr, " slot_value=0x%08X", slot_value);
    else if (slot_addr_in_table)
        fprintf(stderr, " slot_value=????????");
    fprintf(stderr, "\n");

    dump_pa_window(m, "callback_objptr_660000", UINT32_C(0x00660000), 0x60u);
    dump_pa_window(m, "callback_dispatch_ptr_660160", UINT32_C(0x00660160), 0x40u);
    dump_pa_window(m, "callback_obj_66bfc0", UINT32_C(0x0066BFC0), 0x40u);
    dump_va_window(m, "callback_table_1740", UINT32_C(0xA0051740), 0x40u);
    dump_va_window(m, "callback_target_63d0", UINT32_C(0xA00063D0), 0x40u);
    dump_va_window(m, "callback_stack_window", sp, 0x20u);
    if (dispatch_ptr_region)
        log_replay_region_compare(m, dispatch_ptr_region, "callback-dispatch-ptr");
    if (callback_region)
        log_replay_region_compare(m, callback_region, "callback-table");
    if (bootctx_region)
        log_replay_region_compare(m, bootctx_region, "callback-bootctx-stub");

    if (slot_value_ok && plausible_callback_target(slot_value))
        redirect_target = slot_value;
    else if (stack_target_ok && plausible_callback_target(stack_target))
        redirect_target = stack_target;

    m->wince.replay_callback_redirect_attempted = true;
    if (redirect_target == 0) {
        fprintf(stderr,
            "[WINCE_REPLAY_CALLBACK] redirect_applied=0 reason=no-target\n");
        return;
    }

    m->cpu->cd.mips.coproc[0]->reg[COP0_EPC] = redirect_target;
    fprintf(stderr,
        "[WINCE_REPLAY_CALLBACK] redirect_applied=1 new_epc=0x%08X"
        " source=%s\n",
        redirect_target,
        (slot_value_ok && plausible_callback_target(slot_value))
            ? "slot_value"
            : "stack_target");
    dump_code_window(m, redirect_target, 4u, 4u);
}

static bool apply_replay_snapshot(machine_t *m)
{
    bool applied = false;
    bool apply_all_words = m->cfg.wince_resume_replay_full;
    uint32_t i;

    for (i = 0; i < wince_resume_replay_snapshot.region_count; i++) {
        const wince_resume_region_t *region = &wince_resume_replay_snapshot.regions[i];
        uint32_t word_index;

        if (region->pa + region->size > m->cfg.sdram_size)
            continue;

        for (word_index = 0; word_index < region->word_count; word_index++) {
            uint32_t pa;

            if (!apply_all_words && region->valid_words[word_index] == 0)
                continue;

            pa = region->pa + word_index * 4u;
            store_32bit_word(m->cpu, pa_to_kseg0(pa), region->words[word_index]);
            applied = true;
        }
    }

    if (applied)
        invalidate_all(m);

    if (applied && m->wince.log_stall) {
        fprintf(stderr,
            "[WINCE_REPLAY] apply_snapshot mode=%s\n",
            apply_all_words ? "full" : "stable");
    }
    return applied;
}

static bool probable_low_va_target(uint32_t value)
{
    return value != 0 && (value & UINT32_C(0x3)) == 0
        && value < UINT32_C(0x00200000);
}

static uint32_t decode_replay_resume_target(machine_t *m)
{
    uint32_t from_snapshot = wince_resume_replay_snapshot.resume_target_pc;
    uint32_t from_ctx = load_pa_word(m, 0x000022B0u);

    if (probable_low_va_target(from_snapshot))
        return from_snapshot;
    if (probable_low_va_target(from_ctx))
        return from_ctx;
    return 0;
}

static uint32_t decode_replay_resume_entry(uint32_t halt_pc)
{
    if (halt_pc == 0)
        return 0;
    return halt_pc + UINT32_C(0x1C);
}

static uint32_t decode_replay_resume_sp(machine_t *m)
{
    uint32_t from_snapshot = wince_resume_replay_snapshot.resume_stack_pointer;

    (void)m;
    if (from_snapshot != 0)
        return from_snapshot;
    return UINT32_C(0xFFFFD7E0);
}

static void restore_replay_cp0_fields(machine_t *m)
{
    uint32_t i;
    bool applied_any = false;

    for (i = 0; i < wince_resume_replay_snapshot.cp0_field_count; i++) {
        const wince_resume_cp0_field_t *field =
            &wince_resume_replay_snapshot.cp0_fields[i];

        m->cpu->cd.mips.coproc[0]->reg[field->reg] = field->value;
        applied_any = true;
        if (m->wince.log_stall) {
            fprintf(stderr,
                "[WINCE_REPLAY] cp0_restore name=%s reg=%u value=0x%08X\n",
                field->name ? field->name : "-",
                field->reg,
                field->value);
        }
    }

    if (!applied_any) {
        /*
         * Minimal replay exception state: leave the direct survey-driven
         * target/stack intact, but clear BEV and arm the mask bits so the
         * first exception uses the low vectors instead of bouncing into ROM.
         */
        m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS] = UINT32_C(0x1000FF00);
        m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE] = 0;
        m->cpu->cd.mips.coproc[0]->reg[COP0_WIRED] = 0;
        if (m->wince.log_stall) {
            fprintf(stderr,
                "[WINCE_REPLAY] cp0_restore_default"
                " status=0x%08X cause=0x%08X wired=0x%08X\n",
                (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS],
                (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE],
                (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_WIRED]);
        }
    }
}

static void install_replay_helper_tlb(machine_t *m)
{
    struct mips_coproc *cp0;
    int entry_index;

    if (!m || !m->cpu || !m->cpu->cd.mips.coproc[0])
        return;
    if (!replay_mode_enabled(m) || !m->cfg.wince_resume_replay_full)
        return;
    if (m->wince.replay_helper_tlb_installed)
        return;

    cp0 = m->cpu->cd.mips.coproc[0];
    entry_index = cp0->nr_of_tlbs - 1;
    if (entry_index < 0)
        return;

    /*
     * Full replay now reaches the real low-vector save path again. The
     * handler state it touches lives in the low SDRAM windows already seeded
     * at PA 0x00001000..0x00002FFF, but replay was still missing the live
     * kseg2 alias used by the handler at 0xFFFFD888/0xFFFFD8C0.
     *
     * Install a narrow helper pair for that alias only:
     *   0xFFFFD800 -> 0x00001000
     *   paired odd leaf -> 0x00002000
     *
     * Keep this replay-only and explicit so the next run shows whether the
     * remaining blocker is missing live MMU state or something later.
     */
    mips_coproc_tlb_set_entry(m->cpu, entry_index, 4096,
        UINT32_C(0xFFFFD800), UINT32_C(0x00001000), UINT32_C(0x00002000),
        1, 1, 1, 1, 1, 0, 3, 3);
    cp0->tlbs[entry_index].hi = UINT32_C(0xFFFFD800);
    cp0->tlbs[entry_index].lo0 = UINT32_C(0x0000019F);
    cp0->tlbs[entry_index].lo1 = UINT32_C(0x000001DF);
    m->wince.replay_helper_tlb_installed = true;
    invalidate_all(m);

    if (m->wince.log_stall) {
        fprintf(stderr,
            "[WINCE_REPLAY] helper_tlb idx=%d va=0x%08X"
            " pa0=0x%08X pa1=0x%08X\n",
            entry_index,
            UINT32_C(0xFFFFD800),
            UINT32_C(0x00001000),
            UINT32_C(0x00002000));
        dump_tlb_matches(m, "helper_d888", UINT32_C(0xFFFFD888));
        dump_tlb_matches(m, "helper_d8c0", UINT32_C(0xFFFFD8C0));
    }
}

static void log_replay_snapshot(machine_t *m, const char *label)
{
    uint32_t i;

    if (!m->wince.log_stall)
        return;

    fprintf(stderr,
        "[WINCE_REPLAY] label=%s samples=%u required_support=%u"
        " target_pc=0x%08X target_sp=0x%08X synthetic_ra=0x%08X"
        " regions=%u\n",
        label ? label : "-",
        wince_resume_replay_sample_count,
        wince_resume_replay_required_support,
        m->wince.replay_resume_target_pc,
        m->wince.replay_resume_stack_pointer,
        m->wince.replay_synthetic_ra,
        wince_resume_replay_snapshot.region_count);

    for (i = 0; i < wince_resume_replay_snapshot.region_count; i++)
        log_replay_region_compare(m, &wince_resume_replay_snapshot.regions[i],
            label);
}

static void log_replay_pc_state(machine_t *m, const char *label, uint32_t pc)
{
    fprintf(stderr,
        "[WINCE_REPLAY_PC] label=%s pc=0x%08X ra=0x%08X sp=0x%08X"
        " s0=0x%08X s1=0x%08X s2=0x%08X"
        " status=0x%08X cause=0x%08X epc=0x%08X badva=0x%08X"
        " wired=0x%08X entryhi=0x%08X pagemask=0x%08X\n",
        label ? label : "-",
        pc,
        (uint32_t)m->cpu->cd.mips.gpr[31],
        (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
        (uint32_t)m->cpu->cd.mips.gpr[16],
        (uint32_t)m->cpu->cd.mips.gpr[17],
        (uint32_t)m->cpu->cd.mips.gpr[18],
        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS],
        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE],
        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC],
        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR],
        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_WIRED],
        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_ENTRYHI],
        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_PAGEMASK]);
}

static void dump_replay_pc_probe(machine_t *m,
    const wince_replay_pc_probe_desc_t *probe, uint32_t pc)
{
    uint32_t epc = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC];
    uint32_t badva = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR];
    uint32_t s0 = (uint32_t)m->cpu->cd.mips.gpr[16];

    if (probe->code_size != 0) {
        uint32_t base = pc + (uint32_t)probe->code_start_rel;

        dump_va_window(m, probe->label, base, probe->code_size);
    }

    if (probe->stack_size != 0) {
        uint32_t sp = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP];

        if (sp != 0) {
            uint32_t stack_base = sp - (probe->stack_size / 2u);
            char stack_label[64];

            snprintf(stack_label, sizeof(stack_label), "%s_stack",
                probe->label);
            dump_va_window(m, stack_label, stack_base, probe->stack_size);
        }
    }

    if ((pc == UINT32_C(0x8008B254) || pc == UINT32_C(0x8008B3F0))
        && badva != 0) {
        dump_tlb_matches(m, probe->label, badva);
        dump_tlb_matches(m, "helper_d888", UINT32_C(0xFFFFD888));
        dump_tlb_matches(m, "helper_d8c0", UINT32_C(0xFFFFD8C0));
        if (epc != 0)
            dump_va_window(m, "replay_epc", epc & ~UINT32_C(0x1F), 0x40u);
    }

    if (pc == UINT32_C(0x800A7B68)
        || pc == UINT32_C(0x80094F24)
        || pc == UINT32_C(0x80095004)
        || pc == UINT32_C(0x8009511C)
        || pc == UINT32_C(0x800953D4)
        || pc == UINT32_C(0x8008B8EC)
        || pc == UINT32_C(0x8008B994)) {
        maybe_log_replay_dispatch_state(m, pc);
        dump_va_window(m, "resume_poll_dispatch", UINT32_C(0xA0051000),
            0x40u);
    }

    if ((pc == UINT32_C(0x80094F24)
        || pc == UINT32_C(0x80095004)
        || pc == UINT32_C(0x8009511C)
        || pc == UINT32_C(0x800953D4)
        || pc == UINT32_C(0x8008B8EC)
        || pc == UINT32_C(0x8008B994))
        && s0 != 0) {
        char s0_label[64];

        snprintf(s0_label, sizeof(s0_label), "%s_s0_f0", probe->label);
        dump_va_window(m, s0_label, s0 + UINT32_C(0xF0), 0x60u);
    }
}

static void maybe_log_replay_resume_entry_probe(machine_t *m)
{
    uint32_t pc;
    uint32_t entry_pc;

    if (!replay_mode_enabled(m) || !m->wince.cold_boot_redirected
        || !m->wince.log_stall)
        return;

    pc = (uint32_t)m->cpu->pc;
    entry_pc = m->wince.replay_resume_entry_pc;
    if (entry_pc == 0)
        return;

    if (!m->wince.replay_resume_entry_logged && pc == entry_pc) {
        m->wince.replay_resume_entry_logged = true;
        log_replay_pc_state(m, "resume_oal_entry_live", pc);
        dump_va_window(m, "resume_oal_entry_code", entry_pc, 0x40u);
        dump_pa_window(m, "resume_ctx_cp0", 0x00002280u, 0x50u);
        return;
    }

    if (!m->wince.replay_resume_entry_logged || m->wince.replay_resume_exit_logged)
        return;

    if (pc < entry_pc || pc >= entry_pc + UINT32_C(0x24)) {
        m->wince.replay_resume_exit_logged = true;
        log_replay_pc_state(m, "resume_oal_exit", pc);
    }
}

static void maybe_log_replay_pc_probe(machine_t *m)
{
    size_t i;
    uint32_t pc = (uint32_t)m->cpu->pc;

    if (!replay_mode_enabled(m) || !m->wince.cold_boot_redirected
        || !m->wince.log_stall)
        return;

    for (i = 0; i < sizeof(wince_replay_pc_probes) / sizeof(wince_replay_pc_probes[0]);
        i++) {
        const wince_replay_pc_probe_desc_t *probe = &wince_replay_pc_probes[i];
        uint32_t bit = UINT32_C(1) << i;

        if (probe->pc != pc)
            continue;
        if ((m->wince.replay_pc_probe_logged_mask & bit) != 0)
            continue;

        m->wince.replay_pc_probe_logged_mask |= bit;
        log_replay_pc_state(m, probe->label, pc);
        dump_replay_pc_probe(m, probe, pc);
        log_replay_snapshot(m, probe->label);
    }
}

static void synthesize_word_after_write(machine_t *m, uint32_t word_pa,
    uint64_t write_pa, const unsigned char *new_data, size_t len,
    uint32_t *before_out, uint32_t *after_out)
{
    unsigned char bytes[4];
    uint32_t before;
    size_t i;

    before = load_pa_word(m, word_pa);
    bytes[0] = (unsigned char)(before & 0xFFu);
    bytes[1] = (unsigned char)((before >> 8) & 0xFFu);
    bytes[2] = (unsigned char)((before >> 16) & 0xFFu);
    bytes[3] = (unsigned char)((before >> 24) & 0xFFu);

    for (i = 0; i < len; i++) {
        uint64_t cur_pa = write_pa + i;

        if (cur_pa < word_pa || cur_pa >= (uint64_t)word_pa + 4u)
            continue;
        bytes[cur_pa - word_pa] = new_data[i];
    }

    if (before_out)
        *before_out = before;
    if (after_out) {
        *after_out = (uint32_t)bytes[0]
            | ((uint32_t)bytes[1] << 8)
            | ((uint32_t)bytes[2] << 16)
            | ((uint32_t)bytes[3] << 24);
    }
}

static void log_replay_write(machine_t *m, const wince_resume_region_t *region,
    uint32_t word_pa, uint32_t before, uint32_t after, bool valid_expected,
    uint32_t expected, const char *kind)
{
    fprintf(stderr,
        "[WINCE_REPLAY_WRITE] region=%s kind=%s paddr=0x%08X"
        " before=0x%08X after=0x%08X",
        region->name,
        kind,
        word_pa,
        before,
        after);
    if (valid_expected)
        fprintf(stderr, " expected=0x%08X", expected);
    fprintf(stderr, " pc=0x%08" PRIx64 "\n", (uint64_t)m->cpu->pc);
}

static void maybe_log_replay_write_watches(machine_t *m, uint64_t paddr,
    const unsigned char *new_data, size_t len)
{
    size_t i;

    if (!m || !new_data || len == 0)
        return;

    for (i = 0; i < sizeof(wince_replay_write_watches)
        / sizeof(wince_replay_write_watches[0]); i++) {
        const wince_replay_write_watch_desc_t *watch =
            &wince_replay_write_watches[i];
        uint32_t bit = UINT32_C(1) << i;
        uint32_t first_pa;
        uint32_t last_pa;
        uint32_t word_pa;

        if ((m->wince.replay_watch_write_logged_mask & bit) != 0)
            continue;
        if (!range_overlaps(paddr, (uint64_t)len, watch->pa, watch->size))
            continue;

        first_pa = (uint32_t)paddr & ~UINT32_C(0x3);
        last_pa = (uint32_t)(paddr + len - 1u) & ~UINT32_C(0x3);
        for (word_pa = first_pa; word_pa <= last_pa; word_pa += 4u) {
            uint32_t before = 0;
            uint32_t after = 0;

            if (word_pa < watch->pa || word_pa >= watch->pa + watch->size)
                continue;
            synthesize_word_after_write(m, word_pa, paddr, new_data, len,
                &before, &after);
            m->wince.replay_watch_write_logged_mask |= bit;
            log_replay_watch_write(m, watch->label, word_pa, before, after);
            break;
        }
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
    if (strcmp(name, "low_sdram_0000") == 0)
        return true;
    if (strcmp(name, "low_sdram_1880") == 0)
        return true;
    if (strcmp(name, "low_sdram_1ac0") == 0)
        return true;
    if (strcmp(name, "high_sdram_fd40e0") == 0)
        return true;
    if (strcmp(name, "resume_ctx") == 0)
        return true;
    if (strcmp(name, "ctx_tlb") == 0)
        return true;
    if (!resume_only && strcmp(name, "ctx_high_page") == 0)
        return true;
    return false;
}

static bool allow_va_seed_region(const char *name, bool resume_only)
{
    if (!name)
        return false;
    if (!resume_only)
        return false;
    if (strcmp(name, "callback_slot_70e0") == 0)
        return true;
    return false;
}

static bool store_va_region(machine_t *m, uint32_t va, const uint8_t *data,
    uint32_t size)
{
    uint32_t off = 0;

    if (!m || !m->cpu || !data)
        return false;

    while (off < size) {
        uint32_t chunk = size - off;
        if (chunk > 256u)
            chunk = 256u;
        if (!m->cpu->memory_rw(m->cpu, m->cpu->mem,
            va32_to_mips64(va + off), (unsigned char *)(data + off), chunk,
            MEM_WRITE, CACHE_DATA | NO_EXCEPTIONS)) {
            return false;
        }
        off += chunk;
    }

    return true;
}

static bool fallback_pa_for_va_seed(const char *name, uint32_t *pa_out)
{
    if (!name || !pa_out)
        return false;
    if (strcmp(name, "callback_slot_70e0") == 0) {
        *pa_out = UINT32_C(0x00FD40E0);
        return true;
    }
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

static bool fault_site_is_logged(machine_t *m,
    const wince_fault_site_desc_t *site)
{
    return (m->wince.fault_site_logged_mask & site->log_bit) != 0;
}

static void mark_fault_site_logged(machine_t *m,
    const wince_fault_site_desc_t *site)
{
    m->wince.fault_site_logged_mask |= site->log_bit;
}

static const wince_fault_site_desc_t *find_fault_site(uint32_t pc_norm,
    uint32_t exccode)
{
    size_t i;

    for (i = 0; i < sizeof(wince_fault_sites) / sizeof(wince_fault_sites[0]);
        i++) {
        const wince_fault_site_desc_t *site = &wince_fault_sites[i];

        if (site->pc != pc_norm)
            continue;
        if ((site->exccode_mask & WINCE_EXCCODE_BIT(exccode)) == 0)
            continue;
        return site;
    }

    return NULL;
}

static void log_fault_site(machine_t *m, const wince_fault_site_desc_t *site,
    uint32_t fault_pc, uint32_t fault_va, uint32_t exccode,
    const char *source)
{
    uint32_t cause;
    uint32_t epc;
    char detail[64];

    if (!m->wince.active || !m->wince.log_stall
        || !m->wince.cold_boot_redirected || !site
        || fault_site_is_logged(m, site))
        return;

    mark_fault_site_logged(m, site);
    cause = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE];
    epc = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC];
    snprintf(detail, sizeof(detail), "%s-%08x",
        source ? source : "fault",
        site->pc);

    (void)log_checkpoint_header(m, "fault_site", detail);
    fprintf(stderr,
        "[WINCE_FAULT] site=%s pc=0x%08X epc=0x%08X cause=0x%08X"
        " exccode=%u badva=0x%08X source=%s\n",
        site->label,
        fault_pc,
        epc,
        cause,
        exccode,
        fault_va,
        source ? source : "-");
    dump_code_window(m, fault_pc, 4u, 4u);
    dump_gpr_window(m);
    log_memop_probe(m, fault_pc, fault_va);
    if (site->dump)
        site->dump(m, site, fault_pc, fault_va, exccode);
}

static void log_fault_site_pte_walk(machine_t *m,
    const wince_fault_site_desc_t *site, uint32_t fault_pc, uint32_t fault_va,
    uint32_t exccode)
{
    uint32_t index;
    uint32_t random;
    uint32_t entryhi;
    uint32_t dir_off;
    uint32_t pte_off;
    uint32_t leaf_off;
    uint32_t root_va;
    uint32_t root_entry = 0;
    uint32_t second_va = 0;
    uint32_t second_entry = 0;
    bool root_ok;
    bool second_ok = false;

    index = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_INDEX];
    random = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_RANDOM];
    entryhi = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_ENTRYHI];

    dir_off = (fault_va >> 23) & 0xFCu;
    pte_off = (fault_va >> 14) & 0x7FCu;
    leaf_off = (fault_va >> 10) & 0x3Cu;
    root_va = 0xFFFFD8C0u + dir_off;
    root_ok = load_va_word(m, root_va, &root_entry);
    if (root_ok) {
        second_va = root_entry + pte_off;
        second_ok = load_va_word(m, second_va, &second_entry);
    }

    fprintf(stderr,
        "[WINCE_HANDLER] site=%s PC=0x%08X exccode=%u BadVA=0x%08X"
        " Index=0x%08X Random=0x%08X EntryHi=0x%08X"
        " owner=%d ready=%d\n",
        site->label,
        fault_pc,
        exccode,
        fault_va,
        index,
        random,
        entryhi,
        (int)m->wince.vector_owner,
        m->wince.vectors_ready ? 1 : 0);
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

static void log_fault_site_null_d0(machine_t *m,
    const wince_fault_site_desc_t *site, uint32_t fault_pc, uint32_t fault_va,
    uint32_t exccode)
{
    const uint32_t slot_va = UINT32_C(0xFFFFDAC0);
    uint32_t slot_value = 0;
    uint64_t slot_pa = 0;
    bool slot_mapped = translate_va(m, slot_va, &slot_pa);
    bool slot_ok = load_va_word(m, slot_va, &slot_value);

    fprintf(stderr,
        "[WINCE_HANDLER] site=%s PC=0x%08X exccode=%u BadVA=0x%08X"
        " producer_va=0x%08X mapped=%d",
        site->label,
        fault_pc,
        exccode,
        fault_va,
        slot_va,
        slot_mapped ? 1 : 0);
    if (slot_mapped)
        fprintf(stderr, " producer_pa=0x%08" PRIx64, slot_pa);
    if (slot_ok)
        fprintf(stderr, " slot_value=0x%08X", slot_value);
    else
        fprintf(stderr, " slot_value=????????");
    fprintf(stderr, "\n");

    log_va_probe(m, "va_daa0", 0xFFFFDAA0u);
    log_va_probe(m, "va_dac0", slot_va);
    log_va_probe(m, "va_dae0", 0xFFFFDAE0u);
    log_va_probe(m, "va_db00", 0xFFFFDB00u);
    dump_va_window(m, "va_da80", 0xFFFFDA80u, 0xC0u);

    if (slot_mapped && slot_pa <= UINT32_MAX)
        dump_pa_window(m, "pa_1a80", ((uint32_t)slot_pa) & ~UINT32_C(0x3F),
            0x100u);

    if (slot_ok && slot_value != 0) {
        log_va_probe(m, "slot_target", slot_value);
        dump_va_window(m, "slot_target", slot_value & ~UINT32_C(0x0F),
            0x40u);
    }
}

static void log_fault_site_null_pc(machine_t *m,
    const wince_fault_site_desc_t *site, uint32_t fault_pc, uint32_t fault_va,
    uint32_t exccode)
{
    uint32_t epc = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC];
    uint32_t ra = (uint32_t)m->cpu->cd.mips.gpr[31];
    uint32_t t9 = (uint32_t)m->cpu->cd.mips.gpr[25];
    const uint32_t callback_slot_va = UINT32_C(0x000170E4);
    uint32_t callback_slot_value = 0;
    uint64_t callback_slot_pa = 0;
    bool callback_slot_mapped = translate_va(m, callback_slot_va,
        &callback_slot_pa);
    bool callback_slot_ok = load_va_word(m, callback_slot_va,
        &callback_slot_value);

    fprintf(stderr,
        "[WINCE_HANDLER] site=%s PC=0x%08X exccode=%u BadVA=0x%08X"
        " EPC=0x%08X RA=0x%08X T9=0x%08X\n",
        site->label,
        fault_pc,
        exccode,
        fault_va,
        epc,
        ra,
        t9);
    fprintf(stderr,
        "[WINCE_HANDLER] site=%s callback_slot_va=0x%08X mapped=%d",
        site->label,
        callback_slot_va,
        callback_slot_mapped ? 1 : 0);
    if (callback_slot_mapped)
        fprintf(stderr, " callback_slot_pa=0x%08" PRIx64, callback_slot_pa);
    if (callback_slot_ok)
        fprintf(stderr, " callback_slot_value=0x%08X", callback_slot_value);
    else
        fprintf(stderr, " callback_slot_value=????????");
    fprintf(stderr, "\n");
    dump_code_window(m, epc, 6u, 4u);
    if (ra != 0)
        dump_code_window(m, ra - UINT32_C(8), 0u, 4u);
    dump_va_window(m, "va_170c0", 0x000170C0u, 0x40u);
    if (callback_slot_mapped && callback_slot_pa <= UINT32_MAX) {
        dump_pa_window(m, "pa_170c0",
            ((uint32_t)callback_slot_pa) & ~UINT32_C(0x1F), 0x60u);
    }
    if (replay_mode_enabled(m)) {
        fprintf(stderr,
            "[WINCE_REPLAY] null_pc target_pc=0x%08X target_sp=0x%08X"
            " synthetic_ra=0x%08X\n",
            m->wince.replay_resume_target_pc,
            m->wince.replay_resume_stack_pointer,
            m->wince.replay_synthetic_ra);
        log_replay_snapshot(m, "null-pc");
    }
}

static void log_fault_site_replay_stub_miss(machine_t *m,
    const wince_fault_site_desc_t *site, uint32_t fault_pc, uint32_t fault_va,
    uint32_t exccode)
{
    uint32_t sp = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP];
    uint32_t ra = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA];
    uint32_t s0 = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_S0];
    uint32_t s1 = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_S1];
    uint32_t s2 = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_S2];
    uint32_t stack_base_pa = UINT32_C(0x00001000);
    uint32_t stack_base_va = sp & ~UINT32_C(0x0FFF);

    fprintf(stderr,
        "[WINCE_HANDLER] site=%s PC=0x%08X exccode=%u BadVA=0x%08X"
        " SP=0x%08X RA=0x%08X S0=0x%08X S1=0x%08X S2=0x%08X"
        " replay_target=0x%08X synthetic_ra=0x%08X\n",
        site->label,
        fault_pc,
        exccode,
        fault_va,
        sp,
        ra,
        s0,
        s1,
        s2,
        m->wince.replay_resume_target_pc,
        m->wince.replay_synthetic_ra);
    fprintf(stderr,
        "[WINCE_HANDLER] site=%s stack_alias va_base=0x%08X pa_base=0x%08X"
        " badva_off=0x%03X sp_off=0x%03X\n",
        site->label,
        stack_base_va,
        stack_base_pa,
        fault_va - stack_base_va,
        sp - stack_base_va);

    dump_tlb_matches(m, "replay_stub_badva", fault_va);
    dump_tlb_matches(m, "replay_stub_sp", sp);
    dump_va_window(m, "stub_stack_va", stack_base_va + 0x780u, 0x120u);
    dump_pa_window(m, "stub_stack_pa", stack_base_pa + 0x780u, 0x120u);
    dump_pa_window(m, "ctx_tlb", 0x00002000u, 0x80u);
    dump_pa_window(m, "resume_ctx", 0x000022A0u, 0x40u);
    if (replay_mode_enabled(m)) {
        log_replay_region_compare(m, &wince_resume_replay_snapshot.regions[0],
            site->label);
        log_replay_region_compare(m, &wince_resume_replay_snapshot.regions[1],
            site->label);
    }
}

static void maybe_apply_synthetic_ra_replay(machine_t *m, struct cpu *cpu,
    uint32_t epc)
{
    uint32_t sp;
    uint32_t slot_va;
    uint32_t slot_pa = UINT32_C(0x00001804);
    unsigned char bytes[4];
    int wrote = 0;

    if (!replay_mode_enabled(m) || !m->wince.cold_boot_redirected
        || m->wince.replay_synthetic_ra_attempted)
        return;
    if (epc != m->wince.replay_resume_target_pc)
        return;
    if (m->wince.replay_synthetic_ra == 0)
        return;

    sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
    if (sp == 0) {
        sp = m->wince.replay_resume_stack_pointer;
        cpu->cd.mips.gpr[MIPS_GPR_SP] = sp;
    }

    slot_va = sp + UINT32_C(0x24);
    bytes[0] = (unsigned char)(m->wince.replay_synthetic_ra & 0xFFu);
    bytes[1] = (unsigned char)((m->wince.replay_synthetic_ra >> 8) & 0xFFu);
    bytes[2] = (unsigned char)((m->wince.replay_synthetic_ra >> 16) & 0xFFu);
    bytes[3] = (unsigned char)((m->wince.replay_synthetic_ra >> 24) & 0xFFu);

    wrote = cpu->memory_rw(cpu, cpu->mem, va32_to_mips64(slot_va), bytes, 4,
        MEM_WRITE, CACHE_DATA | NO_EXCEPTIONS);
    if (!wrote) {
        store_32bit_word(cpu, pa_to_kseg0(slot_pa), m->wince.replay_synthetic_ra);
        wrote = 1;
    }

    cpu->cd.mips.gpr[31] = m->wince.replay_synthetic_ra;
    m->wince.replay_synthetic_ra_attempted = true;

    if (m->wince.log_stall) {
        fprintf(stderr,
            "[WINCE_REPLAY_RA] applied=%d epc=0x%08X slot_va=0x%08X"
            " fallback_pa=0x%08X synthetic_ra=0x%08X\n",
            wrote ? 1 : 0,
            epc,
            slot_va,
            slot_pa,
            m->wince.replay_synthetic_ra);
    }
}

static void maybe_log_fault_site(machine_t *m)
{
    uint32_t epc;
    uint32_t badvaddr;
    uint32_t exccode;
    uint32_t pc_norm;
    const wince_fault_site_desc_t *site = NULL;
    const char *source = NULL;

    if (!m->wince.active || !m->wince.log_stall
        || !m->wince.cold_boot_redirected)
        return;

    epc = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC];
    badvaddr = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR];
    pc_norm = (uint32_t)m->cpu->pc;
    exccode = (((uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE])
        & CAUSE_EXCCODE_MASK) >> CAUSE_EXCCODE_SHIFT;

    site = find_fault_site(pc_norm, exccode);
    if (site) {
        source = "pc";
        log_fault_site(m, site, pc_norm, badvaddr, exccode, source);
        return;
    }

    site = find_fault_site(epc, exccode);
    if (!site)
        return;

    source = "epc";
    log_fault_site(m, site, epc, badvaddr, exccode, source);
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

static bool apply_va_seed_regions(machine_t *m, bool resume_only)
{
    bool applied = false;
    uint32_t i;

    for (i = 0; i < wince_hw_vseed_region_count; i++) {
        const wince_va_seed_region_t *region = &wince_hw_vseed_regions[i];

        if (!allow_va_seed_region(region->name, resume_only))
            continue;
        if (!store_va_region(m, region->va, region->data, region->size)) {
            uint32_t fallback_pa = 0;

            if (!fallback_pa_for_va_seed(region->name, &fallback_pa))
                continue;
            store_buf(m->cpu, pa_to_kseg0(fallback_pa),
                (const char *)region->data, region->size);
            if (m->wince.log_stall) {
                fprintf(stderr,
                    "[WINCE_SEED] va-fallback name=%s va=0x%08X pa=0x%08X"
                    " size=0x%04X\n",
                    region->name,
                    region->va,
                    fallback_pa,
                    region->size);
            }
        }
        applied = true;
    }

    if (applied)
        invalidate_all(m);
    return applied;
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
    m->wince.use_resume_replay = m->cfg.wince_resume_replay;
    m->wince.resume_mode = m->cfg.wince_resume_replay
        ? WINCE_RESUME_MODE_REPLAY
        : WINCE_RESUME_MODE_INIT_SEED;
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
    bool applied_pa;
    bool applied_va;

    if (!m->wince.active || !m->wince.use_hw_seed || m->wince.initial_seed_applied)
        return;

    applied_pa = apply_pa_seed_regions(m, false);
    applied_va = apply_va_seed_regions(m, false);
    m->wince.initial_seed_applied = true;

    if (m->wince.log_stall) {
        fprintf(stderr,
            "[WINCE_SEED] initial applied_pa=%d applied_va=%d"
            " pa_regions=%u va_regions=%u\n",
            applied_pa ? 1 : 0,
            applied_va ? 1 : 0,
            wince_hw_seed_region_count,
            wince_hw_vseed_region_count);
    }
}

void wince_boot_apply_resume_seed(machine_t *m)
{
    bool applied_pa;
    bool applied_va;

    if (!m->wince.active || !m->wince.use_hw_seed || m->wince.resume_seed_applied)
        return;

    applied_pa = apply_pa_seed_regions(m, true);
    applied_va = apply_va_seed_regions(m, true);
    m->wince.resume_seed_applied = true;

    if (m->wince.log_stall) {
        fprintf(stderr, "[WINCE_SEED] resume applied_pa=%d applied_va=%d\n",
            applied_pa ? 1 : 0,
            applied_va ? 1 : 0);
    }
}

bool wince_boot_prepare_resume_replay(machine_t *m, uint32_t halt_pc,
    uint32_t *target_pc, uint32_t *target_sp)
{
    bool applied_snapshot;
    uint32_t entry_pc;

    if (!replay_mode_enabled(m))
        return false;

    wince_boot_apply_resume_seed(m);
    applied_snapshot = apply_replay_snapshot(m);
    m->wince.replay_snapshot_applied = applied_snapshot
        || m->wince.replay_snapshot_applied;
    m->wince.replay_resume_halt_pc = halt_pc;
    m->wince.replay_resume_entry_pc = decode_replay_resume_entry(halt_pc);
    m->wince.replay_resume_target_pc = decode_replay_resume_target(m);
    m->wince.replay_resume_stack_pointer = decode_replay_resume_sp(m);
    m->wince.replay_synthetic_ra = wince_resume_replay_snapshot.synthetic_ra;
    m->wince.replay_resume_entry_logged = false;
    m->wince.replay_resume_exit_logged = false;

    restore_replay_cp0_fields(m);
    install_replay_helper_tlb(m);

    if (!m->wince.replay_snapshot_logged) {
        log_replay_snapshot(m, "redirect");
        m->wince.replay_snapshot_logged = true;
    }

    entry_pc = m->wince.replay_resume_entry_pc;
    if (target_pc)
        *target_pc = entry_pc;
    if (target_sp)
        *target_sp = 0;

    if (m->wince.log_stall) {
        fprintf(stderr,
            "[WINCE_REPLAY] prepared applied_snapshot=%d"
            " halt_pc=0x%08X entry_pc=0x%08X low_target=0x%08X"
            " replay_sp=0x%08X synthetic_ra=0x%08X\n",
            applied_snapshot ? 1 : 0,
            halt_pc,
            entry_pc,
            m->wince.replay_resume_target_pc,
            m->wince.replay_resume_stack_pointer,
            m->wince.replay_synthetic_ra);
        dump_va_window(m, "resume_halt_code", halt_pc, 0x60u);
        dump_va_window(m, "resume_entry_code", entry_pc, 0x40u);
        dump_pa_window(m, "resume_ctx_cp0", 0x00002280u, 0x50u);
    }

    return entry_pc != 0 && m->wince.replay_resume_target_pc != 0;
}

void wince_boot_on_vr41xx_tick(struct machine *gxm, struct cpu *cpu)
{
    machine_t *m = wince_boot_from_gx(gxm);
    (void)cpu;

    if (!m || !m->wince.active)
        return;

    scan_low_vectors(m);
    maybe_note_first_exception(m);
    maybe_log_fault_site(m);
    maybe_log_replay_pc_probe(m);
    maybe_log_replay_resume_entry_probe(m);
    maybe_probe_replay_callback_redirect(m);
    maybe_log_replay_bootctx_stub_epc_probe(m);
    maybe_log_replay_bootctx_stub_poststate(m);
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
    uint32_t pc_norm;
    uint32_t epc;
    const wince_fault_site_desc_t *site;

    if (!cpu || !cpu->machine)
        return false;

    m = wince_boot_from_gx(cpu->machine);
    if (!m)
        return false;
    pc_norm = (uint32_t)cpu->pc;
    epc = (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_EPC];
    site = find_fault_site(pc_norm, (uint32_t)exccode);
    if (!site)
        return false;

    log_fault_site(m, site, pc_norm, (uint32_t)vaddr,
        (uint32_t)exccode, "low-ref");
    if (site->pc == 0x00000000u)
        maybe_apply_synthetic_ra_replay(m, cpu, epc);
    return true;
}

void wince_boot_note_ram_write(struct cpu *cpu, uint64_t paddr,
    const unsigned char *old_data, const unsigned char *new_data, size_t len)
{
    machine_t *m;
    const wince_resume_region_t *region;
    uint32_t first_pa;
    uint32_t last_pa;
    uint32_t word_pa;

    (void)old_data;

    if (!cpu || !cpu->machine || !new_data || len == 0)
        return;

    m = wince_boot_from_gx(cpu->machine);
    if (!m || !replay_mode_enabled(m) || !m->wince.cold_boot_redirected)
        return;

    first_pa = (uint32_t)paddr & ~UINT32_C(0x3);
    last_pa = (uint32_t)(paddr + len - 1u) & ~UINT32_C(0x3);
    maybe_log_replay_write_watches(m, paddr, new_data, len);

    for (word_pa = first_pa; word_pa <= last_pa; word_pa += 4u) {
        uint32_t before = 0;
        uint32_t after = 0;
        uint32_t expected = 0;
        bool valid_expected = false;
        uint32_t region_bit;
        uint32_t region_index;

        region = find_replay_region_by_pa(&wince_resume_replay_snapshot, word_pa);
        if (!region)
            continue;

        region_index = replay_region_index(&wince_resume_replay_snapshot, region);
        region_bit = UINT32_C(1) << region_index;
        synthesize_word_after_write(m, word_pa, paddr, new_data, len, &before,
            &after);
        (void)replay_region_word(m, region, word_pa, &expected, &valid_expected);

        if ((m->wince.replay_region_write_logged_mask & region_bit) == 0) {
            m->wince.replay_region_write_logged_mask |= region_bit;
            log_replay_write(m, region, word_pa, before, after, valid_expected,
                expected, "first-write");
        }

        if (valid_expected && after != expected
            && (m->wince.replay_region_mismatch_logged_mask & region_bit) == 0) {
            m->wince.replay_region_mismatch_logged_mask |= region_bit;
            log_replay_write(m, region, word_pa, before, after, true, expected,
                "first-mismatch");
        }
    }
}

bool wince_boot_override_vrc4173_read(struct cpu *cpu, uint32_t paddr,
    size_t len, uint64_t *value_io)
{
    machine_t *m;
    uint32_t raw_value;
    uint32_t cmd_1b54 = 0;
    uint32_t cmd_1b58 = 0;
    uint32_t synth_value;

    if (!cpu || !cpu->machine || !value_io || len != 4u
        || paddr != UINT32_C(0x0A001B50)) {
        return false;
    }

    m = wince_boot_from_gx(cpu->machine);
    if (!m || !replay_mode_enabled(m) || !m->wince.cold_boot_redirected
        || !m->cfg.wince_resume_replay_full) {
        return false;
    }

    raw_value = (uint32_t)*value_io;
    if (raw_value != 0)
        return false;
    if (!be300_vrc4173_latch_read_u32(UINT32_C(0x0A001B54), &cmd_1b54)
        || !be300_vrc4173_latch_read_u32(UINT32_C(0x0A001B58), &cmd_1b58)) {
        return false;
    }

    synth_value = (cmd_1b54 & UINT32_C(0x7)) | (cmd_1b58 & UINT32_C(0x8));
    if (synth_value == 0)
        return false;

    *value_io = synth_value;
    if (m->wince.log_stall && !m->wince.replay_vrc4173_1b50_synth_logged) {
        m->wince.replay_vrc4173_1b50_synth_logged = true;
        fprintf(stderr,
            "[WINCE_VRC4173] synth_read paddr=0x%08X raw=0x%08X"
            " cmd_1b54=0x%08X cmd_1b58=0x%08X synth=0x%08X"
            " pc=0x%08" PRIx64 "\n",
            paddr,
            raw_value,
            cmd_1b54,
            cmd_1b58,
            synth_value,
            (uint64_t)cpu->pc);
    }
    return true;
}

void wince_boot_note_mmio_access(struct cpu *cpu, uint64_t paddr,
    int writeflag, uint64_t value, size_t len)
{
    machine_t *m;
    size_t i;
    uint32_t *logged_mask;
    const char *op;

    if (!cpu || !cpu->machine)
        return;
    if (writeflag != MEM_WRITE && writeflag != MEM_READ)
        return;

    m = wince_boot_from_gx(cpu->machine);
    if (!m || !replay_mode_enabled(m) || !m->wince.cold_boot_redirected
        || !m->wince.log_stall) {
        return;
    }

    logged_mask = writeflag == MEM_WRITE
        ? &m->wince.replay_mmio_watch_logged_mask
        : &m->wince.replay_mmio_read_logged_mask;
    op = writeflag == MEM_WRITE ? "W" : "R";

    for (i = 0; i < sizeof(wince_mmio_watches) / sizeof(wince_mmio_watches[0]);
        i++) {
        const wince_mmio_watch_desc_t *watch = &wince_mmio_watches[i];
        uint32_t bit = UINT32_C(1) << i;

        if ((*logged_mask & bit) != 0)
            continue;
        if (!range_overlaps(paddr, (uint64_t)len, watch->pa, 4u))
            continue;

        *logged_mask |= bit;
        fprintf(stderr,
            "[WINCE_MMIO] op=%s label=%s paddr=0x%08" PRIx64
            " value=0x%08" PRIx64 " len=%zu pc=0x%08" PRIx64 "\n",
            op,
            watch->label,
            paddr,
            value,
            len,
            (uint64_t)cpu->pc);
        if (writeflag == MEM_WRITE
            && (strcmp(watch->label, "bootctx_aa000630") == 0
            || strcmp(watch->label, "bootctx_aa000634") == 0
            || strcmp(watch->label, "bootctx_aa000638") == 0
            || strcmp(watch->label, "bootctx_aa00063c") == 0
            || strcmp(watch->label, "bootctx_aa001b54") == 0
            || strcmp(watch->label, "bootctx_aa001b58") == 0
            || strcmp(watch->label, "bootctx_aa000028") == 0
            || strcmp(watch->label, "bootctx_aa01a0e4") == 0)) {
            log_bootctx_stub_poststate(m, watch->label);
        }
    }
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
