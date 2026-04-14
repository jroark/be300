# Phase BD: Fix Probe-Driven Stack Overflow Unblocked By Phase BC

Date: 2026-04-14

## Bottom Line

The downstream SIGSEGV that Phase BC flagged as "needs separate
investigation" was a stack overflow inside the emulator's WinCE
cold-boot probe path, unblocked by Phase BC's AdEL EPC fix. After
the fix lets boot reach many more NK PCs, the per-instruction probe
`maybe_note_callback_slot_pc` burned 6 unconditional guest-memory
loads plus a stack-return walk for every NK instruction in the broad
`0x00060000..0x00100000` PA filter. The cost compounds until the
dyntrans recursion depth (delay-slot-across-page handling uses a
direct `instr(to_be_translated)` call) hits the host stack limit
and aborts with SIGSEGV.

The fix is a one-function early-return in
`src/wince_boot.c:maybe_note_callback_slot_pc`: if `pc32` isn't in
the function's own switch allowlist, return before doing any loads.
No probe coverage is lost — the loads only matter for PCs the switch
cases actually format, and those PCs still take the full path.

Verified: both Release and ASAN/Debug builds now run cold boot for
60s / 90s respectively with exit 124 (gtimeout) instead of 139
(SIGSEGV). Screenshot-on-shutdown fires. No regression on
`--restore --cf` (also exits 124 cleanly, ~4.7B instructions).

## Fingerprint (ASAN, Debug build)

```text
==90762==ERROR: AddressSanitizer: stack-overflow on address 0x00016b367fc0
    #0 wince_boot_note_ram_access wince_boot.c:11738
    #1 mips_memory_rw memory_rw.c:564
    #2 wince_boot_note_pc wince_boot.c:11302         (maybe_note_callback_slot_pc)
    #3 mips_instr_to_be_translated cpu_dyntrans.c:2249
    #4..#254 mips_instr_to_be_translated cpu_dyntrans.c (recursive)
SUMMARY: AddressSanitizer: stack-overflow wince_boot.c:11738
    in wince_boot_note_ram_access
```

Frame #3 is the call site `wince_boot_note_pc(cpu, exec_pc32)` inside
the dyntrans-filter block added in Phase AZ/BA/BB/BC. Frames #4..#254
are dyntrans re-entries via `X(end_of_page)`'s
`instr(to_be_translated)(cpu, cpu->cd.mips.next_ic)` path, which
recurses one level per delay-slot page crossing. Each NK instruction
that runs `maybe_note_callback_slot_pc` drives that recursion a little
deeper because the probe does its own memory reads, each of which
re-enters `mips_memory_rw` and eventually `wince_boot_note_ram_access`.
Eventually the chain exceeds the host stack limit and crashes.

Before Phase BC the cold boot was stuck in the walker hot loop inside
a handful of pages, never reaching the kind of breadth-first NK
execution that makes the compounding probe cost matter. Phase BC
unblocked it and immediately exposed this pre-existing cost bug.

## The Fix

`src/wince_boot.c:maybe_note_callback_slot_pc` previously ran
`load_pa_word` (when `callback_slot_watch_armed`) and five
`load_va_word` calls plus `collect_stack_return_sites` before the
`switch (pc32)` that decides whether anything gets logged. Every one
of those loads re-enters `cpu->memory_rw`. Added an early-return
`switch` at the top of the function that lists the exact PCs the real
formatting switch handles; all other PCs return immediately.

The PC list matches the cases in the body's switch verbatim:

```text
0x01F84A5C, 0x800BFA5C
0x01F84A7C, 0x800BFA7C
0x01F84AA8, 0x800BFAA8
0x8008FF00, 0x80090024, 0x80090044
0x80092488, 0x8009248C, 0x80097000, 0x800971C0
0x80098144, 0x800A3244, 0x80096E88, 0x800A1134
0x800970A8, 0x80098108, 0x80097FC4, 0x800971B4
0x800984CC, 0x800984B4, 0x80096F40, 0x80092798
0x01F8F4D4, 0x01F8F4FC, 0x01FFA93C
0x8013593C, 0x02070418, 0x801AB218
```

No other changes. The dyntrans filter in
`gxemul/src/cpus/cpu_dyntrans.c` (worktree-only from earlier phases)
is intentionally left alone.

## Verification

### Debug + ASAN run

```bash
cd build-host-asan
ASAN_OPTIONS=abort_on_error=0:halt_on_error=1:print_stacktrace=1 \
  gtimeout 90s ./be300 --nand ../ce/restore_images/All_nand_300.bin \
  > /tmp/asan2_stdout.log 2> /tmp/asan2_stderr.log
```

Result:

- Exit 124 (gtimeout), was 1 (SIGSEGV + ASAN abort).
- 341,695,471 instructions executed.
- 1,204,232 stderr lines.
- `[UI] Screenshot saved: screenshot_20260413_233649.bmp`.
- `[PPSH_SUMMARY]`, `[WINCE_TYPE4_SUMMARY]`, `[WINCE_EXC_SUMMARY]`
  all printed during shutdown.

### Release run

```bash
cd build-host
gtimeout 60s ./be300 --nand ../ce/restore_images/All_nand_300.bin \
  > cold_stdout.log 2> repro_phase_bd_release.stderr
```

Result:

- Exit 124, was 139 (SIGSEGV).
- 341,513,566 instructions executed.
- Screenshot saved at `screenshot_20260413_233820.bmp` — 2 colors
  (black/white), so the framebuffer has content but has not yet
  reached the `Initializing...` splash. Expected for Phase BD; the
  next progress milestone (progress bar / splash text) is downstream.
- `WINCE_HOT_L2W` count is 25 stores per boot — the probe is doing
  real walker work without recursion. Phase BC reported 3; the new
  count reflects more NK execution reaching the walker post-fix,
  not a regression.

### `--restore --cf` regression check

```bash
gtimeout 30s ./be300 --restore --cf ../ce/restore_images/All_nand_300.bin
```

Exit 124, 4,786,131,438 instructions, screenshot saved. No crash.

## Recommended Next Steps (Phase BE)

The cold boot now runs for the full `gtimeout` window without
crashing but still hasn't drawn the `Initializing...` splash. The
`[WINCE_EXC_SUMMARY]` at end of run reports
`class=refill_or_tlb_install_semantics reason=page_present_but_faulting`,
which names the next investigation target: TLB refill / PTE install
semantics for slot-0 user pages that appear present but still fault.

Likely starting points:

1. Add a targeted probe on the page-present-yet-faulting exception
   path to capture EPC, BadVaddr, ASID, curproc slot, and the TLB
   entry that should have matched. The existing
   `[WINCE_TLB] match ...` framework in `src/wince_boot.c` has most
   of the scaffolding.
2. Cross-check `gxemul/src/cpus/memory_mips_v2p.c` (in the
   pre-existing worktree diff) against the MIPS v2p rules for
   slot-0 ASIDs — the earlier worktree edits there are load-bearing
   but may be incomplete for the pages the post-BC boot is now
   touching.
3. Audit dyntrans `is_userpage` bookkeeping in `cpu_dyntrans.c` for
   slot-0 pages that transition from kseg0 to user-VA aliases.

Do NOT revert the pre-existing worktree changes in `gxemul/` or
`src/be300_devices.c`. They are load-bearing for walker, TLB, and
device behavior per the Phase AY..BC handoffs.

## Files Changed

- `src/wince_boot.c`: 45 lines added, 0 removed. Single early-return
  switch in `maybe_note_callback_slot_pc`.
- `docs/WINCE_COLD_BOOT_SESSION_2026-04-14_PHASE_BD.md` (this file).

The uncommitted pre-existing worktree changes in `gxemul/` submodule
and `src/be300_devices.c` are intentionally not touched.

Reference logs for this phase (not committed):

- `build-host/repro_phase_bd_release.stderr` — Release reference run.
- `/tmp/asan2_stderr.log` — Debug + ASAN reference run.
- `build-host/screenshot_20260413_233820.bmp` — Release end-of-run
  screenshot.
