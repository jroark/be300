# WinCE NAND Cold-Boot Session Report

Date: 2026-04-12

## Goal

Continue the BE-300 WinCE 3.0 cold-boot work from a true power-on reset,
using the real ROM, the real SPL, and the real `All_nand_300.bin` image,
with no seeded resume state, no guest patching, and no forced handoff
shortcuts.

This report is a continuation of `docs/WINCE_COLD_BOOT_SESSION_2026-04-08.md`.
Read that document for the earlier cold-boot bring-up, strap fixes, and
low-power-path work. This report covers the later exception-debugging work
after the boot moved into NK's post-init / module / callback machinery.

## Current Status

The old fatal low-vector stop is fixed, but WinCE still does not reach a
stable GUI boot. The current blocker is later and narrower:

- a user callback consumer at `0x01F8F4D4` runs before its slot header is
  fully published,
- only the slot argument word at `0x01FE6550` is populated,
- the slot flag at `0x01FE6544` and slot pointer at `0x01FE6548` stay zero,
- the consumer deliberately falls back to `jalr 0xFFFFF9A2` at `0x01F8F4FC`,
- WinCE then enters its own `Exception 004` path, and later cleanup tears
  down the hot user mappings that had briefly been valid.

The most likely next root cause is not generic TLB refill anymore. It is a
missing or skipped guest-visible callback-slot header publish step upstream
of `0x01F8F4D4`.

## Short Version

If you only need the current handoff in one page:

1. The old delayed misaligned `jalr/jr` exception bug in GXemul is fixed.
2. The current failure is later than that and is reproducible without
   crashing the emulator core.
3. The hot user L2 table at `0x80FFC1C8` is not silently lost by the
   emulator. WinCE publishes the PTEs and later zeroes them itself.
4. The immediate problem is that the callback slot header at
   `0x01FE6544/0x01FE6548` never becomes valid before the consumer at
   `0x01F8F4D4` executes.
5. The next pass should trace who is supposed to write the slot header and
   why that publish step never completes after the earlier user fault at
   `0x80092488` on `0x03FE6558`.

## Key Commits And What They Changed

### Main repo

- `05a3d787` - narrowed the post-exception failure and added targeted WinCE
  diagnostics in `src/wince_boot.c` / `src/wince_boot_types.h`.
- `c73b1857` - added callback-slot and callback-object tracing, extra code
  windows, and focused logs around the failing user callback path.

### `gxemul/`

- `e686a53` - fixed delayed misaligned `jr/jalr` exceptions so a bad target
  in a branch delay slot now raises `ADEL` with correct `BD` / `EPC`
  semantics instead of crashing in the wrong place later.

That GXemul fix is already required for the current WinCE diagnostics to make
sense. Do not back it out while debugging the next blocker.

## What Was Tried And What We Learned

### 1. Fix the old delayed-branch misaligned jump bug

Before `e686a53`, the boot could stop in a misleading low-vector failure
after a misaligned user `jalr/jr`. That is no longer the primary blocker.

What changed:

- `gxemul/src/cpus/cpu_mips_instr.c` now raises `ADEL` for misaligned
  `jr/jalr` targets using correct delay-slot semantics.
- the stale delay-slot flag is cleared after `mips_cpu_exception()`.
- matching CPU-side diagnostics were kept in the MIPS core to make later
  exception analysis easier.

Result:

- the former `0x80000190`-style stop disappeared,
- timed WinCE runs now continue through the user fault and into NK's own
  exception machinery,
- later failures are now guest-driven rather than emulator-core crashes.

### 2. Prove whether the hot user page tables were missing or later destroyed

The next hypothesis was that the repeated WinCE exceptions were caused by
missing or stale L2 tables for a hot user region.

Instrumentation added:

- targeted tracking for the hot user L2 table at:
  - VA `0x80FFC1C8`
  - PA `0x00FFC1C8`
- `WINCE_HOT_L2W` logging for each write into that table,
- `WINCE_HOT_L2` summaries for a few important probe VAs,
- `WINCE_TLB_POST` summaries to show the decoded section entry, L2 pointer,
  and selected `EntryLo` values at each exception.

What this proved:

- WinCE really does publish the hot user PTEs,
- the PTEs later become zero because guest code explicitly zeroes them,
- the teardown path is in guest code around `0x80097000` / `0x800971C0`,
- the publish path is around `0x800984B4` / `0x800984CC`.

Useful concrete evidence from the current repro logs:

- writes `#25` through `#29` publish valid entries:
  - `#25` `off=0x24 val=0x000FFB1E`
  - `#26` `off=0x38 val=0x000FFD1A`
  - `#27` `off=0x30 val=0x000FFA1A`
  - `#28` `off=0x2C val=0x000FF91A`
  - `#29` `off=0x28 val=0x000FF81A`
- writes `#30` through `#39` later zero the same region from
  `pc=0x80097000 ra=0x800971C0`

Conclusion:

- the emulator is not losing these L2 writes,
- the later page-table collapse is a guest cleanup symptom,
- the real root cause is upstream of the cleanup.

### 3. Narrow the failure to a callback-slot publish problem

Once the L2 writes were understood, the debugging focus shifted to the user
callback path that immediately precedes the fatal `Exception 004`.

Relevant addresses:

- callback wrapper entry: `0x01F84A5C`
- wrapper call site of consumer: `0x01F84A7C`
- delay-slot store of callback arg: `0x01F84A80`
- callback consumer entry: `0x01F8F4D4`
- deliberate fallback `jalr`: `0x01F8F4FC`
- first earlier user fault in this chain: `0x80092488` on `0x03FE6558`

What the new logs showed:

- the wrapper receives callback object `0x80FFFEA8`,
- the callback object points at slot `0x03FE6558` and callback function
  `0x01F84A5C`,
- the wrapper stores the callback object pointer to `0x01FE6550` in the
  delay slot of its `jal 0x01F8F4D4`,
- that store faults once on `TLBL`, then succeeds after refill,
- by the time the consumer runs, the slot looks like:
  - flag `0x01FE6544 = 0`
  - ptr  `0x01FE6548 = 0`
  - aux  `0x01FE654C = 0`
  - arg  `0x01FE6550 = 0x80FFFEA8`

The raw slot bytes captured in the log are:

```text
[WINCE_PTR] callback_slot_base va=0x01FE6544 space=user bytes=000000000000000000000000A8FEFF80
```

That is the most important line in the current debug state.

It means:

- the callback slot exists,
- the argument word is present,
- but the header words needed by the consumer are still zero.

### 4. Decode the failing consumer path directly from runtime logs

The added `WINCE_CODE` windows are enough to decode the critical user
callback sequence without pulling the module apart separately.

Consumer window at `0x01F8F4D4`:

```text
0x01F8F4E0  lw   t6, 0x6544(at)   ; slot flag
0x01F8F4E4  beq  t6, zr, +4
0x01F8F4EC  lw   t7, 0x6548(at)   ; slot ptr
0x01F8F4F4  lw   v0, 0x260(t7)    ; callback target
0x01F8F4F8  li   v0, -1630
0x01F8F4FC  jalr v0
```

Interpretation:

- if the slot flag is zero, the consumer never uses the pointer,
- instead it deliberately loads `v0 = 0xFFFFF9A2`,
- and then executes `jalr v0`.

That deliberate fallback is what produces the user-space `Exception 004`
that WinCE later prints on serial.

Wrapper window around `0x01F84A7C`:

```text
0x01F84A7C  jal   0x01F8F4D4
0x01F84A80  sw    t6, 0x6550(at)  ; delay-slot store of arg=0x80FFFEA8
```

Interpretation:

- the wrapper definitely stores the arg word,
- the wrapper does not by itself prove that the slot flag or slot pointer
  were ever published beforehand,
- the missing publish is therefore upstream of the consumer rather than a
  failure of the arg store.

### 5. Correlate the callback failure with the earlier user fault

The earliest important user fault in this window is:

```text
[EXC_ENTRY] exc=3 pc=0x80092488 vaddr=0x03FE6558
[WINCE_TLB_POST] ... fault=0x03FE6558 ... lo0=0x00000000 lo1=0x00000000
```

Later, after guest publication of the hot PTE:

```text
[WINCE_TLB_POST] ... fault=0x03FE6558 ... lo0=0x000FFB1E lo1=0x00000000
```

That ties the callback object / slot setup to the same hot user region.

Current interpretation:

- the callback setup path is already unstable before the consumer runs,
- the setup path faults while working on the object/slot area,
- some later part of the publish sequence never completes,
- the consumer then sees only the arg word and takes the deliberate failure
  path.

## Current Failure Chain

This is the current best end-to-end sequence to keep in mind.

1. NK enters a user / module setup path that eventually reaches
   `0x80092488`.
2. That path faults on user VA `0x03FE6558`.
3. WinCE later publishes the hot user PTEs for the relevant region.
4. The callback wrapper at `0x01F84A5C` runs with object `0x80FFFEA8`.
5. The wrapper calls the consumer at `0x01F8F4D4` and stores only the arg
   word to `0x01FE6550`.
6. The slot header at `0x01FE6544/0x01FE6548` is still zero.
7. The consumer takes the deliberate fallback and executes
   `jalr 0xFFFFF9A2` at `0x01F8F4FC`.
8. GXemul now correctly raises `ADEL` with the right delay-slot semantics.
9. WinCE enters its own `Exception 004` path and prints the serial message.
10. Later guest cleanup at `0x80097000` tears down the hot L2 entries.
11. The boot never reaches a valid GUI-ready steady state.

## How To Reproduce

Build:

```bash
cmake --build build-host -j4
```

Primary repro:

```bash
bash -lc 'cd build-host && gtimeout 45s ./be300 --nand ../ce/restore_images/All_nand_300.bin > repro13.stdout 2> repro13.stderr; echo EXIT:$?'
```

Shorter targeted repro:

```bash
bash -lc 'cd build-host && gtimeout 25s ./be300 --nand ../ce/restore_images/All_nand_300.bin > repro14.stdout 2> repro14.stderr; echo EXIT:$?'
```

Helpful greps:

```bash
rg -n "WINCE_HOT_L2W|WINCE_TLB_POST|WINCE_CB_PC|WINCE_PTR|WINCE_CB_OBJ|EXC_ENTRY" build-host/repro14.stderr
rg -n "0x01FE6550|0x03FE6558|0x01F8F4FC|0x80092488" build-host/repro14.stderr
```

## How To Read The New Logs

### `WINCE_HOT_L2W`

Use this to answer: "Did the guest actually write the hot user L2 table?"

Fields:

- `#N` is just the observed write count to the tracked L2 page.
- `off` is the offset within the tracked L2 page.
- `val` is the new PTE value.
- `pc` / `ra` identify the guest writer.

Practical interpretation:

- writes `#25..#29` are the important publish events,
- writes `#30..#39` are the later guest teardown events,
- this log is stronger than any summary heuristic when deciding whether the
  guest or emulator destroyed the mappings.

### `WINCE_HOT_L2`

Use this as a compact decode of a few hot PTE slots after each tracked L2
write.

It tells you:

- which tracked VA is affected,
- whether `lo0` / `lo1` are valid,
- whether the guest has just made a page present or torn it back down.

### `WINCE_TLB_POST`

This is the best one-line post-exception summary.

Use it to see:

- fault VA,
- exception code,
- section-table entry used,
- L2 pointer chosen,
- `pte_off`,
- decoded `lo0` / `lo1`,
- and whether the selected page was actually present at the moment of the
  exception.

Important caveat:

- the older summary classification at the end of the run can still say
  `refill_or_tlb_install_semantics`, but in the current failure window the
  more detailed callback-slot logs are the stronger evidence.

### `WINCE_CB_PC`

This is the main callback-path trace.

Use it to see:

- the current hot callback PC,
- slot header words (`flag`, `ptr`, `aux`, `arg`),
- whether the slot is armed,
- relevant live registers (`a0`, `a1`, `a2`, `t6`, `v0`, `t7` depending on
  the site),
- and the immediate stack context.

Most useful labels in the current bug:

- `callback_wrapper_entry`
- `callback_init_call`
- `callback_consumer_entry`
- `callback_consumer_jalr`

### `WINCE_PTR callback_slot_base`

This prints the raw 16 bytes at `0x01FE6544`.

For the current failure, the expected "bad" line is:

```text
bytes=000000000000000000000000A8FEFF80
```

That means:

- flag = 0
- ptr = 0
- aux = 0
- arg = `0x80FFFEA8`

If future work finds a run where the first 8 bytes become nonzero before the
consumer reaches `0x01F8F4D4`, that is immediately important.

### `WINCE_CB_OBJ`

This decodes the current callback object at `0x80FFFEA8`.

Key fields already confirmed:

- `slot=03FE6558`
- `cb=01F84A5C`
- `listbc=03FE6570`
- `state=1`
- `flags=1`

If the slot publish bug is fixed, this object is still the first thing to
re-check for consistency.

### `WINCE_CODE`

This is the most useful runtime disassembly aid for short hot windows.

Use it when:

- exact PC watchpoints are unreliable because translated blocks do not always
  re-enter `note_pc()` at the granularity you want,
- or when you need to decode a small user-space sequence quickly from a log.

This was how the `0x01F8F4D4` consumer sequence and the wrapper call at
`0x01F84A7C` were decoded.

### `WINCE_L2_ALLOC`

This was added to watch parts of the suspected L2 allocation / release path.

Current usefulness:

- limited,
- some of the expected sites did not fire through the current `note_pc()`
  path, probably because dyntrans kept the execution inside translated
  blocks,
- the memory-write and callback logs ended up being much more reliable.

Recommendation:

- prefer memory write traces and object/slot dumps over pure PC watchpoints
  for the next pass unless you deliberately add a stronger hook.

## Current Theory

The current theory is:

- the callback consumer failure is a symptom of an earlier incomplete publish
  path,
- the guest callback object exists and is passed correctly,
- the guest callback arg word is stored correctly,
- but the guest never finishes publishing the slot header before the consumer
  runs,
- likely because the earlier fault path around `0x80092488` on `0x03FE6558`
  interrupts or skips a required object/slot initialization step.

More concretely:

- the next real bug is probably not "TLB refill is wrong",
- it is probably "a required guest-visible setup step for the callback slot
  header never completes."

This is why the current best next question is:

- who should write `0x01FE6544` and `0x01FE6548`,
- and why does that writer never run, or never complete, before
  `0x01F8F4D4`?

## Recommended Next Steps

### 1. Trace the missing slot-header writers

The next pass should identify the first writer that is supposed to publish:

- `0x01FE6544` (flag)
- `0x01FE6548` (ptr)

Start from:

- the wrapper sequence around `0x01F84A5C` / `0x01F84A7C`,
- the earlier setup path at `0x80092488`,
- and any function reached from the current object `0x80FFFEA8`.

The main question is whether the writer:

- never runs,
- runs with bad inputs,
- or runs and writes elsewhere.

### 2. Prefer write-based instrumentation over PC-only watchpoints

Because dyntrans can hide some exact PC transitions from `note_pc()`, the
next pass should prefer:

- direct watchpoints / logging around the slot-header VAs,
- direct logging of stores into the callback-object / callback-slot region,
- and small code windows near the confirmed writer if needed.

### 3. Keep the earlier conclusions intact

Do not re-open these unless new evidence demands it:

- the delayed-branch misaligned jump fix in GXemul,
- the conclusion that the hot user L2 PTEs are really written by the guest,
- the conclusion that the later teardown is guest cleanup, not silent memory
  loss in the emulator.

### 4. Treat the run-end summary as secondary evidence

`WINCE_EXC_SUMMARY` is still useful, but in the current window the detailed
callback-slot and hot-L2 logs are more precise than the summary class name.

Use the summary as a tail signal, not as the primary diagnosis.

## Practical Debugging Notes

- `build-host/repro13.stderr` and `build-host/repro14.stderr` are good
  reference logs for the current failure shape.
- If you add new logs, keep them narrow. Broad log floods have repeatedly
  made WinCE behavior harder to interpret.
- For short user-space sequences, the `WINCE_CODE` window is faster than
  switching immediately into external disassembly tooling.
- For questions like "did the guest ever write this?", prefer a direct write
  trace to a derived summary.

## Current Local State Notes

At the time of this report:

- top-level `main` is at `c73b1857`,
- `gxemul` HEAD is `e686a53`,
- there are unrelated local worktree changes in `gxemul/` and
  `src/be300_devices.c`,
- there are also untracked `.tmp_spl/*` analysis files.

Do not assume those unrelated local changes are part of the current callback
slot investigation. Review them before using them, and do not revert them
blindly.

## Bottom Line

The main handoff conclusion is simple:

- the emulator now survives the old delayed-branch exception bug,
- the guest really does build and later tear down the hot user PTEs,
- and the next blocker is a missing callback-slot header publish before
  `0x01F8F4D4`, not a generic page-table black hole.

If the next pass can explain why `0x01FE6544` and `0x01FE6548` stay zero
while `0x01FE6550` is already valid, it is very likely to move the boot past
the current `Exception 004` window.
