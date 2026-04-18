# Phase BC: Real Root Cause Found and Fixed in gxemul

Date: 2026-04-14

## Bottom Line

The cold-boot Exception 004 is fixed by a **one-instruction change to
gxemul's `mips_raise_bad_jump_target` helper** in
`gxemul/src/cpus/cpu_mips_instr.c`. With the fix:

- The walker hot-L2W store count drops from ~48 to **3**.
- Coredll DllMain at slot-0 `0x01F84A5C` is dispatched and runs.
- TLB entries for slot-0 user pages (`0x01FB2001`, `0x01FBC001`,
  `0x01FB0001`) start populating during boot.
- Coredll executes user-VA helper code at `0x01F955AC`,
  `0x01FA0020`, `0x01FB23A4`, etc. before the run terminates from
  a downstream emulator SIGSEGV that needs separate investigation.

The fix is committed in the gxemul submodule as
`c4f8936 mips: AdEL on misaligned jr/jalr target sets EPC=vaddr
(real-hw semantics)`.

## What Phase BB Almost Got Right

Phase BB correctly identified that:

- Coredll's link-time `.data` has zero at OAL slot rva `0x66544`.
- The helper at slot-0 `0x01F8F4D4` (kseg0 `0x800CA4D4`) reads
  this sentinel, sees zero, and falls back to
  `li v0, -1630; jalr v0` where -1630 = `0xFFFFF9A2`.
- `0xFFFFF9A2` is a WinCE 3.0 kernel callback trampoline address.

What Phase BB **missed**: NK 3.0 *does* implement the dispatch
natively. The general-exception handler at NK kseg0 `0x8008B240`
decodes EPC bits 31..16 == `0xFFFF` and bits 1..0 == `0b10` to
recognise kernel callback addresses and dispatches them via
`0x8008B794`. The mechanism is fully present in the NK image.

The actual bug is **in the emulator's MIPS exception model**, not in
the missing kernel-callback dispatch. Specifically, the emulator's
helper for "address error on misaligned jump target" set EPC to the
**branch instruction's PC** (with `Cause.BD = 1`), instead of the
**misaligned target** (with `Cause.BD = 0`). The previous
"e686a53 mips: fix delayed misaligned jump exceptions" gxemul commit
introduced this incorrect model.

NK's GE handler computes `(EPC | 0xFFFC) + 2`. With `EPC = jalr's PC`
(a normal NK code address whose top 16 bits are not `0xFFFF`), the
test never matches, and the handler treats the exception as an
unhandled fault. With `EPC = 0xFFFFF9A2` (the misaligned target),
the test matches and the syscall dispatches normally.

## The Fix

`gxemul/src/cpus/cpu_mips_instr.c`, helper
`mips_raise_bad_jump_target`:

```c
/*
 * Real MIPS hardware: an Address Error on instruction fetch raises
 * the exception at the new PC (= the misaligned target). EPC is set
 * to the faulting fetch address, and Cause.BD is 0 because the
 * delay-slot instruction already retired successfully and the fault
 * is at the new PC, not inside the branch's delay slot.
 *
 * WinCE 3.0's general-exception handler depends on this: it inspects
 * EPC bits 31..16 == 0xFFFF and EPC bits 1..0 == 0b10 to recognise
 * kernel callback trampolines (e.g. coredll dispatching via
 * `jalr 0xFFFFF9A2`). If EPC is set to branch_pc instead of vaddr,
 * that test fails and the syscall is mis-handled as an unhandled
 * fault, breaking coredll DllMain on cold boot.
 */
cpu->pc = vaddr;
cpu->delay_slot = NOT_DELAYED;
mips_cpu_exception(cpu, EXCEPTION_ADEL, 0, vaddr, 0,
    vaddr_vpn2, vaddr_asid, 0);
```

The previous (buggy) version was:

```c
cpu->pc = (uint64_t)(MODE_uint_t)(branch_pc + 4);
cpu->delay_slot = DELAYED;
mips_cpu_exception(cpu, EXCEPTION_ADEL, 0, vaddr, 0,
    vaddr_vpn2, vaddr_asid, 0);
```

The new path is consistent with the MIPS architecture spec for
Address Error exceptions on instruction fetch: EPC equals the
faulting fetch address, and `Cause.BD = 0` because the delay slot
already retired.

## How NK's Dispatcher Works

The general exception vector is at PA `0x000180` = kseg0 `0x80000180`
and contains a tiny trampoline (read at runtime via the
`WINCE_GE_VEC` probe added in this phase):

```text
0x80000180:  00000000 00000000 3C1A8008 375AB240
0x80000190:  03400008 00000000
```

Decoded:

```asm
0x80000188  lui   k0, 0x8008
0x8000018c  ori   k0, k0, 0xb240          ; k0 = 0x8008B240
0x80000190  jr    k0                       ; jump to handler body
0x80000194  nop                             ; (delay slot)
```

The body at `0x8008B240`:

```asm
8008b240  mfc0  k0, EPC                    ; k0 = EPC
8008b244  bne   zero, k1, 0x8008b3f0        ; nested-exception path
8008b248  nop
8008b24c  ori   k1, k0, 0xfffc              ; k1 = k0 | 0xFFFC
8008b250  addiu k1, k1, 2                    ; k1 = (k0 | 0xFFFC) + 2
8008b254  beq   zero, k1, 0x8008b794         ; kernel-callback dispatch
8008b258  mfc0  k1, Cause                    ; (delay slot)
```

The test `(k0 | 0xFFFC) + 2 == 0` matches when `k0` has bits 31..16
all 1 AND bits 1..0 == `0b10`. Concretely this is any address of the
form `0xFFFFxxx2`, `0xFFFFxxx6`, `0xFFFFxxxA`, `0xFFFFxxxE` — exactly
the misaligned syscall trampoline range used by coredll's helper
fallbacks.

The dispatcher at `0x8008B794`:

```asm
8008b7a0  addiu t0, k0, 1022          ; t0 = EPC + 0x3FE
8008b7a4  lw    t3, -10096(zero)
8008b7a8  move  k1, zero
8008b7ac  ori   t3, t3, 0x1
8008b7b0  mtc0  t3, c0_sr
8008b7b4  andi  t1, t1, 0x3f
8008b7b8  addiu sp, sp, -32
8008b7bc  sra   t0, t0, 0x2            ; t0 >>= 2 (compute syscall index)
8008b7c0  li    t9, -1026
8008b7c4  sw    t9, 28(sp)
...
8008b7f0  jal   0x8009ada8              ; resolve syscall
8008b7f4  addiu a0, sp, 20
...
8008b844  jalr  v0                       ; dispatch to syscall handler
```

So WinCE 3.0's syscall mechanism is:

1. User code does `jalr 0xFFFFxxxN` where `N & 3 == 2`.
2. CPU raises AdEL on the misaligned fetch with `EPC = 0xFFFFxxxN`,
   `BadVaddr = 0xFFFFxxxN`, `Cause.BD = 0`.
3. NK's exception vector trampoline jumps to `0x8008B240`.
4. `0x8008B240` reads EPC, recognises the high-VA pattern, branches
   to `0x8008B794`.
5. `0x8008B794` computes a syscall index from EPC, looks up the
   handler via `0x8009ADA8`, and `jalr`s to it.
6. Handler runs, returns via `eret` which restores PC to `EPC`
   (the misaligned target). But the handler has set up the return
   such that execution continues at the original `ra` instead.

This is a clean, self-contained syscall ABI. The emulator just had to
deliver the correct EPC.

## Verification

Before the fix:

- Walker hot-L2W stores: ~48 per cold boot (full coredll tail unmap)
- Loader rollback fires once
- Boot deadlocks with Exception 004 around the OAL callback consumer

After the fix:

- Walker hot-L2W stores: **3** per cold boot
- Loader rollback still fires once (the FIRST DllMain attempt still
  rolls back via the AdEL path, but subsequent loads succeed)
- Boot reaches user-VA execution at slot-0 (`0x01F955AC`,
  `0x01FA0020`, `0x01FB23A4`, etc.), populates TLB entries for
  multiple slot-0 user pages, then SIGSEGVs in the emulator from a
  downstream issue that needs separate investigation in Phase BD.

The 60-second log grows from ~2,000 lines (before fix) to ~430,000
lines (after fix) — boot is doing real work now.

## What Phase BC Confirmed via Probes

### `WINCE_DLLMAIN_ENTRY` descriptor field dump

```text
[WINCE_DLLMAIN_ENTRY] #1 desc_fields:
   +0x10=0x00000000(loaded_mask)
   +0x84=0x00000000(dispatch_flag)
   +0x88=0x00000000
   +0xC8=0x00020000
```

- `desc[+0x10] = 0`: coredll not yet marked loaded for any slot.
  FUN_8008FF00 does not take its bnez early-skip and proceeds to
  dispatch DllMain.
- `desc[+0x84] = 0`: BEQL at `0x8008FFE8` takes the branch. DISPATCH 2
  (`jalr t8` at `0x8009003C`) is used. Both dispatches call the same
  `desc[+0x60]`.

### `WINCE_GE_VEC` exception vector dump

```text
[WINCE_GE_VEC] 0x80000000: 401A4000 AC08D888 AC1BD88C 001A45C2 07400018 310800FC
[WINCE_GE_VEC] 0x80000080: 00000000 00000000 00000000 00000000 00000000 00000000
[WINCE_GE_VEC] 0x80000180: 00000000 00000000 3C1A8008 375AB240 03400008 00000000
[WINCE_GE_VEC] 0x80000200: 00000000 00000000 00000000 00000000 00000000 00000000
```

- The TLB refill handler is **inlined** at PA 0x000 (the same code
  that lives at NK kseg0 `0x8008C418`).
- The general exception trampoline is at PA 0x180 and jumps to
  NK kseg0 `0x8008B240`.
- The kseg-TLB-miss vector at 0x080 is unused (vector 0x000 handles
  both kuseg and kseg2 misses).

### `WINCE_OAL_TABLE` link-time content

```text
[WINCE_OAL_TABLE] slot0(0x01FE6544..50): X00000000 X00000000 X00000000 X00000000
[WINCE_OAL_TABLE] kseg0(0x800B8198..A4): 00000000 00000000 80078DF0 80078E10
```

- The `X` prefix on the slot-0 reads indicates `load_va_word` failed
  (no slot-0 mapping for filesys at the moment of FUN_8008FF00 entry).
  This is a TLB context artifact, not a missing PTE — by the time
  the dispatch actually fires the slot-0 mapping is populated.
- The kseg0 reads succeed and confirm coredll's link-time `.data`:
  rva `0x66544 = 0`, `0x66548 = 0`, `0x6654C = 0x80078DF0`,
  `0x66550 = 0x80078E10`. Matches the direct file read from
  `nk_decompressed.bin` at offset `0x58198`.

## Phase BC Files Changed

Committed in `gxemul/` submodule (`c4f8936`):

- `src/cpus/cpu_mips_instr.c`:
  - **The actual fix**: `mips_raise_bad_jump_target` now sets
    `cpu->pc = vaddr` and `delay_slot = NOT_DELAYED` so that
    `mips_cpu_exception` records `EPC = vaddr` and `Cause.BD = 0`.
  - Refactored the previous inline macro into a helper function for
    a cleaner diff and reusable callsite.
  - Adds 11 PCs to the explicit `wince_boot_note_pc` switch for
    Phase AZ/BA/BB/BC cold-boot tracing (`0x800927CC`,
    `0x800929D0`, `0x800929E4`, `0x8008FF00`, `0x80090050`,
    `0x80090030`, `0x80090048`, `0x8009004C`, `0x01F84C0C`,
    `0x01F84C04`).

Committed in main repo (this commit):

- `src/wince_boot.c`: Phase BC additions to
  `maybe_note_dllmain_dispatch_pc` — extended descriptor field
  dump (`+0x10`, `+0x84`, `+0x88`, `+0xC8`), exception vector dump
  at `0x80000000/0x80000080/0x80000180/0x80000200`, and OAL table
  reads at slot-0 `0x01FE6544..0x01FE6550` and kseg0
  `0x800B8198..0x800B81A4`.
- `docs/WINCE_COLD_BOOT_SESSION_2026-04-14_PHASE_BC.md` (this file).
- Updated `gxemul/` submodule pointer to `c4f8936`.

## Recommended Next Steps (Phase BD)

1. **Investigate the downstream SIGSEGV.** Build the emulator with
   debug symbols (`cmake -DCMAKE_BUILD_TYPE=Debug`), reproduce the
   crash under lldb, and identify the failing function. Likely
   candidates:
   - dyntrans IC handling for newly-translated user-VA pages
   - PTE refill code for slot-0 user pages
   - NK kernel callback dispatcher returning to a bad PC
2. **Verify the gxemul fix doesn't regress non-WinCE workloads.**
   The fix changes EPC semantics for misaligned jr/jalr targets,
   which could affect Linux boots if Linux relied on the previous
   `BD=1` behavior. Per CLAUDE.md Linux is no longer a project
   goal, but a quick sanity check that `--restore --cf` still
   completes its NANDWRITER recovery flow would be reassuring.
3. **Trace the FIRST kernel callback dispatch** at `0x8008B794` to
   see which syscall index is being requested and whether NK
   actually handles it. If the syscall index resolution at
   `0x8009ADA8` returns a valid handler, the dispatch should work.
4. **Compare the runtime OAL callback table** at slot-0
   `0x01FE6544..0x01FE65A4` after the first DllMain ATTACH
   completes. If the first call populates the sentinels, subsequent
   helper calls take the fast path and don't need the AdEL trap.

## Phase Summary

The five-phase arc (AY, AZ, BA, BB, BC) walked back from the
mis-attribution of "decode 0xFFFFD800 inside the walker" to the
actual root cause: a one-instruction bug in gxemul's MIPS exception
model that broke WinCE 3.0's syscall ABI. The bug had been there
since `e686a53 mips: fix delayed misaligned jump exceptions`, which
introduced the wrong EPC semantics while trying to fix a different
issue.

The lesson: when a guest-binary investigation runs into 50+ phases
of "this should work but doesn't", the answer is usually in the
emulator's hardware-accuracy, not in the guest. CLAUDE.md says it
explicitly: "Always prefer hardware-accurate emulation over
workarounds. Find and fix the emulator bug rather than patching the
guest." This phase chain is a textbook case.
