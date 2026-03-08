# Fix: User-space code missing at kuseg target VA after do_execve

## Context

After the previous fix (ERET_MANUAL_KUSEG), PC successfully reaches `0x2AAA8A00` (ld.so entry point). But `insn@target=0x00000000` — the memory at the kuseg VA is mapped but contains zeros. The emulator loops executing NOPs at the target address.

**Root cause:** During do_execve, the kernel's ELF loader stores user-space code/data to kuseg VAs (e.g., `0x2AAA8A00`) via `__copy_user`. These stores go through Unicorn's softmmu TLB, which translates the kuseg VA to a physical address (PA) in SDRAM. The data lands in the correct PA.

However, when we later try to execute at the kuseg VA, Unicorn's flat memory region table doesn't have a mapping for that VA. The `mem_unmapped_hook` or `machine_run` recovery creates a fresh empty 1MB block at the VA without copying the SDRAM content that was written through the TLB.

The existing `tlb_map_kuseg_page()` function (line 337) does exactly what's needed: maps a kuseg VA block and populates it from SDRAM at the corresponding PA. It's called from the TLBWI handler (line 1604). But diagnostics show `TLB_MAP` never fires, meaning `shadow_cp0_badvaddr` is zero or kseg at TLBWI time.

**Two problems to fix:**
1. `tlb_map_kuseg_page` is not being called during do_execve's TLB operations (shadow state issue)
2. Even if it were, the 1MB granularity mapping may not cover all pages — we need a fallback at ERET time

## Plan

### Step 1: Add diagnostic logging to TLBWI kuseg mapping path

**File:** `src/machine.c`, line ~1596

Add temporary logging (not gated on `tlb_trace_window`) to understand why the kuseg mapping path is being skipped:

```c
if (insn == 0x42000002u || insn == 0x42000006u) {
    m->pending_tlb_flush = true;

    uint32_t badvaddr = (uint32_t)m->shadow_cp0_badvaddr;
    /* DIAGNOSTIC: always log TLBWI state */
    static uint32_t tlbwi_diag_log = 0;
    if (tlbwi_diag_log < 128) {
        fprintf(stderr,
                "[TLBWI_DIAG] badvaddr=0x%08X lo0=0x%08" PRIX64
                " lo1=0x%08" PRIX64 " entryhi=0x%08" PRIX64
                " PC=0x%08" PRIX64 "\n",
                badvaddr,
                (uint64_t)(uint32_t)m->shadow_cp0_entrylo0,
                (uint64_t)(uint32_t)m->shadow_cp0_entrylo1,
                (uint64_t)(uint32_t)m->shadow_cp0_entryhi,
                (uint64_t)(uint32_t)address);
        tlbwi_diag_log++;
    }
    // ... existing code
}
```

### Step 2: Use EntryHi for candidate VA, but only map on high-confidence shadow state

**File:** `src/machine.c`, line ~1596

The current code uses `shadow_cp0_badvaddr` to determine the kuseg VA. But `BadVAddr` might be stale if the readback didn't fire correctly. The kernel's TLB handler writes `EntryHi` (which contains VPN2+ASID) before doing TLBWI. `EntryHi` is written via MTC0 (captured by `cp0_shadow_write` at line 1490, always active), so it is a better source for candidate VA.

Do not map from `EntryHi` blindly. Require confidence checks before calling `tlb_map_kuseg_page`:
- candidate VPN2 is kuseg (`< 0x80000000`)
- `shadow_cp0_pagemask == 0` (4KB pages only for this path)
- selected EntryLo page has `V=1`
- ASID confidence is high: either global pair (`G` set for both Lo0/Lo1) or EntryHi ASID matches current ASID

If confidence is low, log and skip population (fail closed instead of mapping wrong PA).

Replace the BadVAddr check with guarded EntryHi logic:

```c
uint32_t badvaddr = (uint32_t)m->shadow_cp0_badvaddr;
uint32_t entryhi = (uint32_t)m->shadow_cp0_entryhi;
uint32_t entryhi_vpn2 = entryhi & 0xFFFFE000u; /* VPN2 */
uint8_t entryhi_asid = (uint8_t)(entryhi & 0xFFu);
uint8_t current_asid = get_current_asid(m);     /* live CP0 EntryHi ASID */
uint32_t lo0 = (uint32_t)m->shadow_cp0_entrylo0;
uint32_t lo1 = (uint32_t)m->shadow_cp0_entrylo1;
bool global_pair = ((lo0 & 0x1u) != 0) && ((lo1 & 0x1u) != 0);

/* Prefer EntryHi (written by MTC0, always captured) over BadVAddr
 * (captured via MFC0 readback, may be stale). */
uint32_t kuseg_va = (entryhi_vpn2 != 0 && entryhi_vpn2 < 0x80000000u)
                  ? entryhi_vpn2 : badvaddr;

if (kuseg_va != 0u && kuseg_va < 0x80000000u) {
    bool asid_ok = global_pair || (entryhi_asid == current_asid);
    bool mask_ok = ((uint32_t)m->shadow_cp0_pagemask == 0u);
    if (asid_ok && mask_ok) {
        uint32_t even_va = kuseg_va & ~0x1FFFu;
        // ... existing EntryLo0/Lo1 extraction and tlb_map_kuseg_page calls
    } else {
        fprintf(stderr,
                "[TLBWI_POPULATE_SKIP] low confidence asid_ok=%d mask_ok=%d "
                "entryhi=0x%08X badvaddr=0x%08X\n",
                asid_ok, mask_ok, entryhi, badvaddr);
    }
}
```

### Step 3: Add a shared `shadow_tlb_lookup` helper and use it at ERET_MANUAL_KUSEG

**File:** `src/machine.c`, ERET_MANUAL_KUSEG block

After the existing probe, if instruction at target VA is zero, attempt population only through a shared lookup helper that returns:
- `hit` (VA matched)
- `confident` (ASID/global/mask checks passed)
- resolved `pa` and `va_page`
- `reason` string for diagnostics when not confident

Use that helper instead of directly trusting one shadow snapshot:

```c
if (probe == 0) {
    tlb_lookup_result r = shadow_tlb_lookup(m, (uint32_t)epc);
    if (r.hit && r.confident) {
        tlb_map_kuseg_page(m, r.va_page, r.pa_page);
        maybe_map_pair_page(m, r);
    } else {
        fprintf(stderr,
                "[ERET_MANUAL_KUSEG] skip populate hit=%d confident=%d reason=%s "
                "epc=0x%08X\n",
                r.hit, r.confident, r.reason, (uint32_t)epc);
    }

    /* Re-probe after population attempt */
    uc_mem_read(uc, (uint32_t)epc, &probe, 4);
    fprintf(stderr, "[ERET_MANUAL_KUSEG] after TLB populate: insn@target=0x%08X\n", probe);
}
```

### Step 4: Add kuseg SDRAM population to mem_unmapped_hook / machine_run fallback

**File:** `src/machine.c`, machine_run EXECVE_HANDOFF_FIX block

When the belt-and-suspenders fix fires and maps the kuseg VA, also try to populate from SDRAM using the same `shadow_tlb_lookup` helper from Step 3.

In the `EXECVE_HANDOFF_FIX` block, after restoring PC/SP/status:
1. Call `shadow_tlb_lookup(m, restored_pc)`
2. Populate only when `hit && confident`
3. Otherwise keep existing empty map behavior and emit low-confidence diagnostic

### Step 5: Optional, debug-gated broader fix in STORE_EMU/LOAD_EMU

**File:** `src/machine.c`, `emulate_store_on_write_unmapped` (line 511) and `emulate_load_at_pc` (line 432)

This is invasive in hot paths, so keep it disabled by default. Add a runtime flag (for example, `--kuseg-hotpath-populate`) and only run this behavior when explicitly enabled for experiments.

For kuseg addresses and with the flag enabled, try `shadow_tlb_lookup` first and only populate when `hit && confident`; otherwise preserve current empty-block fallback.

```c
if (m->opt_kuseg_hotpath_populate && addr32 < 0x80000000u) {
    /* kuseg: try to populate from SDRAM via shadow TLB */
    tlb_lookup_result r = shadow_tlb_lookup(m, addr32);
    if (r.hit && r.confident) {
        tlb_map_kuseg_page(m, r.va_page, r.pa_page);
    } else {
        /* Fallback: map empty block */
        uc_mem_map(m->uc, block32, 0x100000, UC_PROT_READ | UC_PROT_WRITE);
        if (block != block32)
            uc_mem_map(m->uc, block, 0x100000, UC_PROT_READ | UC_PROT_WRITE);
    }
}
```

### Step 6: Build and test

```bash
cd /work/build-docker && cmake .. && make -j$(nproc)
timeout 180s ./be300 --cmdline "console=tty0 console=ttyS0,9600 root=/dev/ram" \
  --kernel ../kernels/vmlinux-pgui-demo > docker_2.4_stdout.log 2> docker_2.4_stderr.log

# 2.6 regression check
KERNEL26=../linux4be20040908/vmlinux
[ -f "$KERNEL26" ] || KERNEL26=../kernels/vmlinux-2.6
timeout 180s ./be300 --kernel "$KERNEL26" \
  > docker_2.6_stdout.log 2> docker_2.6_stderr.log
```

## Files to modify

- `src/machine.c` — TLBWI handler (~line 1587), ERET_MANUAL_KUSEG block (~line 1945), STORE_EMU (~line 511), LOAD_EMU (~line 432), machine_run EXECVE_HANDOFF_FIX block

## Verification

- **Success criteria:** `insn@target` shows non-zero MIPS instruction at `0x2AAA8A00`. `[TLB_MAP]` log entries appear showing kuseg VA↔PA mappings with `confident=1`.
- **Safety criteria:** no kuseg population when confidence checks fail (`[..._SKIP]` logs present instead).
- **Expected next blockers:** TLB misses at other kuseg pages, user-space syscalls, missing page content for data segments.
- **2.6 regression:** No change expected — 2.6 never reaches kuseg ERET.

## Implementation order

1. Step 1 (diagnostics) — understand actual shadow state at TLBWI time
2. Step 2 (EntryHi + confidence gating) — fix TLBWI kuseg mapping trigger safely
3. Step 6 (test) — verify whether TLBWI path alone is sufficient
4. Step 3 (ERET populate via shared lookup) — fallback population at handoff time
5. Step 4 (EXECVE_HANDOFF_FIX populate via shared lookup) — secondary fallback
6. Step 6 (test) — verify fallback impact
7. Step 5 (optional hot-path experiment) — only if still blocked, and flag-gated
8. Step 6 (test) and remove Step 1 diagnostics if no longer needed

Steps 3-5 are incremental fallbacks; each should be added only if the prior step fails to produce non-zero code at target VA.
