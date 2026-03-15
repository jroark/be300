/*
 * mem_alias.c — Memory aliasing & kseg mapping
 *
 * Manages kseg0/kseg1/kuseg memory aliases and PA coherence for Unicorn's
 * flat memory model. The VR4131 uses MIPS kseg0 (0x80000000) and kseg1
 * (0xA0000000) as cached/uncached views of physical memory. Unicorn treats
 * these as separate address ranges, so we must maintain coherence manually.
 *
 * Extracted from machine.c.
 */

#include "mem_alias.h"
#include "machine.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* sdram_alias_pa_offset                                               */
/* ------------------------------------------------------------------ */

bool sdram_alias_pa_offset(const machine_t *m, uint64_t addr, uint64_t *off_out)
{
    uint64_t sdram_size = (uint64_t)m->cfg.sdram_size;
    uint64_t off = UINT64_MAX;

    if (addr < sdram_size) {
        off = addr;
    } else if (addr >= UINT64_C(0x0000000080000000) &&
               addr < UINT64_C(0x0000000080000000) + sdram_size) {
        off = addr - UINT64_C(0x0000000080000000);
    } else if (addr >= UINT64_C(0x00000000A0000000) &&
               addr < UINT64_C(0x00000000A0000000) + sdram_size) {
        off = addr - UINT64_C(0x00000000A0000000);
    } else if (addr >= UINT64_C(0xFFFFFFFF80000000) &&
               addr < UINT64_C(0xFFFFFFFF80000000) + sdram_size) {
        off = addr - UINT64_C(0xFFFFFFFF80000000);
    } else if (addr >= UINT64_C(0xFFFFFFFFA0000000) &&
               addr < UINT64_C(0xFFFFFFFFA0000000) + sdram_size) {
        off = addr - UINT64_C(0xFFFFFFFFA0000000);
    } else {
        return false;
    }

    if (off_out)
        *off_out = off;
    return true;
}

/* ------------------------------------------------------------------ */
/* write_pa_u32_all_aliases / read_pa_u32_all_aliases                   */
/* ------------------------------------------------------------------ */

void write_pa_u32_all_aliases(machine_t *m, uint32_t pa, uint32_t val)
{
    if (!m || !m->uc)
        return;

    uint64_t off = (uint64_t)pa;
    const uint64_t aliases[] = {
        off,
        UINT64_C(0x0000000080000000) + off,
        UINT64_C(0x00000000A0000000) + off,
        UINT64_C(0xFFFFFFFF80000000) + off,
        UINT64_C(0xFFFFFFFFA0000000) + off,
    };

    for (unsigned i = 0; i < (sizeof(aliases) / sizeof(aliases[0])); i++) {
        uc_mem_write(m->uc, aliases[i], &val, sizeof(val));
    }
}

bool read_pa_u32_all_aliases(machine_t *m, uint32_t pa, uint32_t *out)
{
    if (!m || !m->uc || !out)
        return false;

    uint64_t off = (uint64_t)pa;
    const uint64_t aliases[] = {
        off,
        UINT64_C(0x0000000080000000) + off,
        UINT64_C(0x00000000A0000000) + off,
        UINT64_C(0xFFFFFFFF80000000) + off,
        UINT64_C(0xFFFFFFFFA0000000) + off,
    };

    for (unsigned i = 0; i < (sizeof(aliases) / sizeof(aliases[0])); i++) {
        uint32_t val = 0;
        if (uc_mem_read(m->uc, aliases[i], &val, sizeof(val)) == UC_ERR_OK) {
            *out = val;
            return true;
        }
    }

    return false;
}

/* ------------------------------------------------------------------ */
/* alias_coherence_probe                                               */
/* ------------------------------------------------------------------ */

void alias_coherence_probe(machine_t *m)
{
    if (m->shared_alias_active) {
        if (m->cfg.sdram_size < 0x200u) {
            fprintf(stderr, "[ALIAS_COHERENCE] fail mode=shared reason=sdram_too_small\n");
            return;
        }

        uint32_t probe_pa32 = (m->cfg.sdram_size - 0x100u) & ~UINT32_C(0x3);
        uint64_t pa = probe_pa32;
        uint64_t kseg0 = UINT64_C(0x0000000080000000) + probe_pa32;
        uint64_t kseg1 = UINT64_C(0x00000000A0000000) + probe_pa32;
        uint32_t orig = 0, pa_val = 0, kseg0_val = 0, kseg1_val = 0;
        const uint32_t pattern = UINT32_C(0xA55A3CC3);

        uc_err e_orig = uc_mem_read(m->uc, pa, &orig, sizeof(orig));
        uc_err e_w = uc_mem_write(m->uc, kseg1, &pattern, sizeof(pattern));
        uc_err e_pa = uc_mem_read(m->uc, pa, &pa_val, sizeof(pa_val));
        uc_err e_k0 = uc_mem_read(m->uc, kseg0, &kseg0_val, sizeof(kseg0_val));
        uc_err e_k1 = uc_mem_read(m->uc, kseg1, &kseg1_val, sizeof(kseg1_val));
        if (e_orig == UC_ERR_OK) {
            uc_mem_write(m->uc, pa, &orig, sizeof(orig));
            uc_mem_write(m->uc, kseg0, &orig, sizeof(orig));
            uc_mem_write(m->uc, kseg1, &orig, sizeof(orig));
        }

        bool pass = (e_orig == UC_ERR_OK &&
                     e_w == UC_ERR_OK &&
                     e_pa == UC_ERR_OK &&
                     e_k0 == UC_ERR_OK &&
                     e_k1 == UC_ERR_OK &&
                     pa_val == pattern &&
                     kseg0_val == pattern &&
                     kseg1_val == pattern);
        fprintf(stderr,
                "[ALIAS_COHERENCE] %s mode=shared probe_pa=0x%08X"
                " pa=0x%08X k0=0x%08X k1=0x%08X"
                " errs=(orig:%d wr:%d pa:%d k0:%d k1:%d)\n",
                pass ? "pass" : "fail",
                probe_pa32, pa_val, kseg0_val, kseg1_val,
                (int)e_orig, (int)e_w, (int)e_pa, (int)e_k0, (int)e_k1);
        return;
    }

    if (m->alias_fallback_sync_active) {
        fprintf(stderr,
                "[ALIAS_COHERENCE] fallback-write-sync active"
                " mode=fallback shared=0\n");
        return;
    }

    fprintf(stderr, "[ALIAS_COHERENCE] fail mode=none reason=no_alias_strategy\n");
}

/* ------------------------------------------------------------------ */
/* map_kseg_alias_block                                                */
/* ------------------------------------------------------------------ */

bool map_kseg_alias_block(machine_t *m, uint64_t map_base, uint64_t pa_base)
{
    if (m->shared_alias_active &&
        m->sdram_backing != NULL &&
        pa_base < (uint64_t)m->cfg.sdram_size &&
        pa_base + UINT64_C(0x100000) <= (uint64_t)m->cfg.sdram_size) {
        uc_err pe = uc_mem_map_ptr(m->uc, map_base, 0x100000, UC_PROT_ALL,
                                   m->sdram_backing + (size_t)pa_base);
        if (pe == UC_ERR_OK || pe == UC_ERR_MAP)
            return true;
        static uint32_t shared_alias_map_fail = 0;
        if (shared_alias_map_fail < 16) {
            fprintf(stderr,
                    "[ALIAS_MODE] shared map_kseg_alias_block failed"
                    " map=0x%08" PRIX64 " pa=0x%08" PRIX64 " err=%s\n",
                    map_base, pa_base, uc_strerror(pe));
            shared_alias_map_fail++;
        }
    }

    uc_err me = uc_mem_map(m->uc, map_base, 0x100000, UC_PROT_ALL);
    if (me != UC_ERR_OK && me != UC_ERR_MAP)
        return false;

    if (me == UC_ERR_OK) {
        uint8_t buf[4096];
        for (uint64_t off = 0; off < 0x100000; off += sizeof(buf)) {
            if (uc_mem_read(m->uc, pa_base + off, buf, sizeof(buf)) != UC_ERR_OK)
                memset(buf, 0, sizeof(buf));
            if (uc_mem_write(m->uc, map_base + off, buf, sizeof(buf)) != UC_ERR_OK)
                break;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* map_kseg_mirror_block                                               */
/* ------------------------------------------------------------------ */

bool map_kseg_mirror_block(machine_t *m, uint64_t va_block)
{
    uint32_t va32 = (uint32_t)va_block;
    if (va32 < 0x80000000u || va32 > 0xBFFFFFFFu)
        return false;

    uint64_t map_base = (uint64_t)(int64_t)(int32_t)va32;
    uint64_t map_base_zero = (uint64_t)va32;
    uint64_t pa_base = (uint64_t)(va32 & 0x1FFFFFFFu);
    bool mapped_any = false;

    if (map_kseg_alias_block(m, map_base, pa_base))
        mapped_any = true;

    /*
     * Some Unicorn paths surface zero-extended 0x8000xxxx VAs in MIPS64 mode.
     * Mirror both aliases so exception-vector fetches can resolve either form.
     */
    if (map_base_zero != map_base && map_kseg_alias_block(m, map_base_zero, pa_base))
        mapped_any = true;

    if (!mapped_any)
        return false;
    return true;
}

/* ------------------------------------------------------------------ */
/* tlb_map_kuseg_page                                                  */
/* ------------------------------------------------------------------ */

/*
 * tlb_map_kuseg_page — directly map a physical SDRAM page into kuseg virtual
 * address space in Unicorn's host region table.
 *
 * Background: Unicorn's MIPS64 softmmu TLB may not honour TLBWI for kuseg
 * addresses, causing an infinite TLB-store-miss storm at PC=0x800015B4.
 * Two root causes are addressed together:
 *
 *  (a) Stale softmmu TLB entry: the softmmu cached a read-only (D=0) entry
 *      that isn't evicted when handle_tlbs upgrades the PTE to D=1 + TLBWI.
 *      Fixed by uc_ctl_flush_tlb() after TLBWI/TLBWR (see pending_tlb_flush).
 *
 *  (b) Unicorn kuseg bypass: some Unicorn builds skip the MIPS hardware TLB
 *      for kuseg and use the flat Unicorn region table keyed by VA directly.
 *      Fixed by pre-mapping the faulting VA block in Unicorn's region table
 *      with a copy of the corresponding SDRAM content.
 *
 * Both fixes are applied together; whichever is relevant for the current
 * Unicorn build will unblock execution.
 */
void tlb_map_kuseg_page(machine_t *m, uint64_t kuseg_va, uint64_t pa,
                        uint64_t page_bytes)
{
    if (page_bytes < 0x1000u)
        page_bytes = 0x1000u;
    if ((page_bytes & 0xFFFu) != 0u)
        page_bytes = (page_bytes + 0xFFFu) & ~(uint64_t)0xFFFu;

    if ((uint32_t)kuseg_va >= 0x80000000u)
        return;  /* safety: only handle user-space addresses */

    uint64_t va_pg = kuseg_va & ~(page_bytes - 1u);
    uint64_t pa_pg = pa       & ~(page_bytes - 1u);

    uc_err me = uc_mem_map(m->uc, va_pg, page_bytes, UC_PROT_ALL);
    bool premap = (me == UC_ERR_MAP);
    if (premap) {
        /* Pre-mapped VA window: still refresh it from SDRAM PA content. */
        me = UC_ERR_OK;
        uc_err pe = uc_mem_protect(m->uc, va_pg, page_bytes, UC_PROT_ALL);
        if (pe != UC_ERR_OK) {
            static uint32_t prot_err_log = 0;
            if (prot_err_log++ < 16)
                fprintf(stderr,
                        "[TLB_MAP_PROT_FAIL] VA_pg=0x%08" PRIX64
                        " bytes=0x%08" PRIX64 " err=%s\n",
                        va_pg, page_bytes, uc_strerror(pe));
        }
    }
    if (me != UC_ERR_OK) {
        static uint32_t err_log = 0;
        if (err_log++ < 16)
            fprintf(stderr,
                    "[TLB_MAP_FAIL] VA_pg=0x%08" PRIX64
                    " bytes=0x%08" PRIX64 " err=%s\n",
                    va_pg, page_bytes, uc_strerror(me));
        return;
    }

    /* Populate exactly the translated TLB window.
     * Read from kseg0 alias (0x80000000 + PA) first, because kernel writes
     * (ramdisk load, page cache I/O) go to kseg0 alias regions which are
     * separate Unicorn memory from the PA region at 0x00000000.
     * Fall back to raw PA if the kseg0 alias block isn't mapped yet.
     *
     * We write to THREE destinations:
     *   1. The kuseg VA flat region (for direct softmmu hits before TLBWI)
     *   2. The raw PA region (for softmmu hits after TLB-translated access)
     *   3. The kseg0 alias is already authoritative — no write needed.
     * This ensures correct data regardless of whether the softmmu resolves
     * via the flat VA region or through the MIPS hardware TLB to PA. */
    uint8_t buf[4096];
    for (uint64_t off = 0; off < page_bytes; off += sizeof(buf)) {
        uint64_t pa_off = pa_pg + off;
        bool got_data = false;

        /* Try kseg0 alias first (has kernel runtime writes) */
        if (pa_off < (uint64_t)m->cfg.sdram_size) {
            uint64_t kseg0_addr = 0x80000000u + pa_off;
            if (uc_mem_read(m->uc, kseg0_addr, buf, sizeof(buf)) == UC_ERR_OK)
                got_data = true;
        }
        /* Fallback to raw PA (has ELF loader content) */
        if (!got_data && pa_off < (uint64_t)m->cfg.sdram_size) {
            if (uc_mem_read(m->uc, pa_off, buf, sizeof(buf)) == UC_ERR_OK)
                got_data = true;
        }
        if (!got_data)
            memset(buf, 0, sizeof(buf));

        /* Write to kuseg VA flat region */
        uc_mem_write(m->uc, va_pg + off, buf, sizeof(buf));

        /* Also sync raw PA so TLB-translated accesses see correct data */
        if (got_data && pa_off < (uint64_t)m->cfg.sdram_size)
            uc_mem_write(m->uc, pa_off, buf, sizeof(buf));
    }

    static uint32_t map_log = 0;
    if (map_log++ < 64)
        fprintf(stderr,
                "[TLB_MAP] kuseg VA=0x%08" PRIX64
                " <- SDRAM PA=0x%08" PRIX64
                " bytes=0x%08" PRIX64 " premap=%u (via kseg0)\n",
                va_pg, pa_pg, page_bytes, premap ? 1u : 0u);

    if (m->cfg.trace_user_handoff &&
        m->execve_user_handoff_active &&
        m->execve_user_handoff_state == EXECVE_HANDOFF_STATE_ARMED) {
        static uint32_t mapcheck_log = 0;
        uint32_t handoff_pc = (uint32_t)m->execve_user_handoff_pc;
        uint64_t map_end = va_pg + page_bytes;
        if (mapcheck_log < 256 &&
            (uint64_t)handoff_pc >= va_pg &&
            (uint64_t)handoff_pc + 16u <= map_end) {
            uint64_t off = (uint64_t)handoff_pc - va_pg;
            uint64_t pa_target = pa_pg + off;
            uint8_t va_bytes[16] = {0};
            uint8_t pa_bytes[16] = {0};
            uc_err va_err = uc_mem_read(m->uc, (uint64_t)handoff_pc, va_bytes, sizeof(va_bytes));
            uc_err pa_err = uc_mem_read(m->uc, pa_target, pa_bytes, sizeof(pa_bytes));
            char va_hex[16 * 3 + 1];
            char pa_hex[16 * 3 + 1];
            format_hex_bytes(va_bytes, sizeof(va_bytes), va_hex, sizeof(va_hex));
            format_hex_bytes(pa_bytes, sizeof(pa_bytes), pa_hex, sizeof(pa_hex));
            bool same = (va_err == UC_ERR_OK && pa_err == UC_ERR_OK &&
                         memcmp(va_bytes, pa_bytes, sizeof(va_bytes)) == 0);
            fprintf(stderr,
                    "[USER_HANDOFF_MAPCHECK] pc=0x%08X va_page=0x%08" PRIX64
                    " pa_page=0x%08" PRIX64 " off=0x%04" PRIX64
                    " va_err=%d pa_err=%d same=%u va=[%s] pa=[%s]\n",
                    handoff_pc, va_pg, pa_pg, off,
                    va_err, pa_err, same ? 1u : 0u, va_hex, pa_hex);
            mapcheck_log++;
        }
    }

    /* Ensure any stale softmmu negative translation for this VA is dropped
     * before retrying user-mode fetch/load. */
    uc_ctl_flush_tlb(m->uc);
}
