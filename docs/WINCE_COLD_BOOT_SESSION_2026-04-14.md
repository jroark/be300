# WinCE NAND Cold-Boot Session Report — Phases AY and AZ

Date: 2026-04-14

## Goal

Continue the BE-300 WinCE 3.0 cold-boot work from the 2026-04-13
handoff (`docs/WINCE_COLD_BOOT_SESSION_2026-04-13.md`). Phase AX's
addendum directed the next session toward decoding `s0 = 0xFFFFD800`
inside `FUN_80097F44` and adding one runtime probe at
`FUN_8008FE8C`'s frame.

This report covers two phases:

- **Phase AY** (commit `0369ca97`) — pure static analysis. No emulator
  builds, no runtime probes. Decoded the entire walker caller chain
  from the existing `WINCE_STACK_WALK` dump in
  `build-host/repro_phase_at.stderr`, fully disassembled the relevant
  NK functions, and identified `s0 = 0xFFFFD800` as a red herring.
- **Phase AZ** (this commit) — added a one-shot runtime probe at the
  WinCE DLL loader's DllMain dispatch. Confirmed that DllMain dispatch
  fires with correct arguments AND that the loader takes the rollback
  path AT THE SAME TIME, which is internally contradictory and points
  at either dyntrans `cpu->gpr` staleness or an unidentified second
  invocation.

## Bottom Line

After two phases of work the trigger is still not pinned, BUT the
search space has narrowed dramatically. Specifically:

- The pre-Phase-AS hypothesis (loader rollback after DllMain failure)
  is structurally correct and is the path that fires on cold boot.
- The exact reason DllMain "returns 0" at runtime is the next decode
  target, and it requires either a more careful runtime probe (one
  that does not rely on `cpu->gpr` reads being stable around the
  dispatch) or a static trace of every return path inside coredll's
  DllMain at slot-0 `0x01F84A5C`.

## Phase AY: Static Analysis Findings

### 1. The walker's caller chain (from existing log)

`build-host/repro_phase_at.stderr` already contains a 48-word
`WINCE_STACK_WALK` dump from `caller_sp = 0x0201FD30`. Decoded
against the NK objdump it gives the full chain in one read, no new
probe required:

```text
caller_sp = 0x0201FD30   (FUN_8008FE8C body sp)

sp+0x14 = 0x80090240  FUN_8008FE8C saved_ra → inside FUN_80090144
                      (return PC after `jal 0x8008fe8c` @ 0x80090238)

sp+0x20 = 0x80FFFEA8  first word of FUN_80090144's frame (s1)
                      = coredll module descriptor

sp+0x3C = 0x8009040C  FUN_80090144 saved_ra → inside FUN_800903BC
                      (return PC after `jal 0x80090144` @ 0x80090404)

sp+0x54 = 0x800929E4  FUN_800903BC saved_ra → inside FUN_800927CC
                      (return PC after `jal 0x800903bc` @ 0x800929DC)
```

Full call stack at walker-entry time:

```text
FUN_800970A8  (walker)
FUN_80097F44  (frame -80, ra at sp+0x1C, no s-regs)
FUN_8008FE8C  (frame -32, ra at sp+0x14)
FUN_80090144  (frame -32, ra at sp+0x1C, s0/s1 at sp+0x14/0x18)
FUN_800903BC  (frame -32, recursion-guarded wrapper)
FUN_800927CC  (WinCE DLL loader, callsite 0x800929DC)
```

### 2. FUN_800927CC at 0x800929C0 — the trigger code

```asm
800929bc  beqz s1, 0x800929f0       ; skip if earlier failure cleared s1
800929c0  move a0, s2                 ; (delay) a0 = coredll desc
800929c4  li   a1, 1                  ; a1 = DLL_PROCESS_ATTACH
800929c8  jal  0x8008ff00              ; FUN_8008FF00(coredll, 1, 0)
800929cc  move a2, zero
800929d0  bnez v0, 0x800929f0           ; skip rollback if v0 != 0
800929d4  move a0, s2
800929d8  move a1, zero
800929dc  jal  0x800903bc                ; *** rollback ***
800929e0  move s1, zero
800929e4  lw   t7, 704(s3)
800929e8  li   t9, 0x45A                  ; ERROR_DLL_INIT_FAILED, AFTER rollback
800929ec  sw   t9, 56(t7)
```

This is the WinCE DLL loader's three-failure-path block. Phase AE's
"the kernel error is 0 so DllMain didn't fail" is a false negative
because the error 0x45A is set at `0x800929E8`, AFTER the rollback
returns. Reading the error field at walker-entry time always shows 0
regardless of whether the trigger was DllMain failure.

### 3. s0 = 0xFFFFD800 is NK kernel-globals base, not a walker input

```asm
80090144  addiu sp, sp, -32           ; FUN_80090144 entry
...
80090184  li    s0, -10240             ; *** s0 = 0xFFFFD800 ***
80090188  lw    t0, 708(s0)            ; t0 = *(0xFFFFDAC4) = curproc
8009018c  lbu   t1, 0(t0)              ; t1 = curproc.slot_idx
... uses s0 throughout the body as kernel-globals base ...
```

`s0 = -10240 = 0xFFFFD800` is the base address of NK's per-process
kernel globals. It is loaded by `FUN_80090144` at `0x80090184` and
used as a callee-saved base pointer for `lw rN, off(s0)` accesses
to globals at `0xFFFFD800..0xFFFFDFFF`. None of FUN_80090144,
FUN_8008FE8C, or FUN_80097F44 touches s0 across their `jal` calls,
so the walker (`FUN_800970A8`) inherits and dutifully spills
`s0 = 0xFFFFD800` as part of saving the caller's callee-saved set.
**The walker never reads s0 as a parameter.** Phase AX/AW's
"decode 0xFFFFD800" target is a red herring.

### 4. Phase AV's "publisher and walker in same function" was wrong

`FUN_80097F44` (`0x80097F44`, frame -80) ends at `0x8009817C`
(`jr ra; addiu sp, sp, 80`). It does NOT save any s-registers — only
`ra`. The publisher loop at `0x80098484` lives in a SEPARATE function
whose entry is `0x80098180` (frame -152, saves `s0..s8`). They are
two adjacent but distinct NK routines.

Phase AV's confusion came from disassembling without a function
boundary — both functions live in the same NK code page and decode
adjacently if you read straight through. The boundary is the `jr ra`
at `0x8009817C`.

### 5. Coredll DllMain at slot-0 0x01F84A5C / kseg0 0x800BFA5C

Phase AS's "valid 3-arg prologue" reading is correct. The function
publishes the OAL callback table during DLL_PROCESS_ATTACH:

```asm
800bfa5c  27bdffb8  addiu sp, sp, -72
800bfa60  afbf0014  sw    ra, 0x14(sp)
800bfa64  afa40048  sw    a0, 0x48(sp)    ; save hinstDLL
800bfa68  afa5004c  sw    a1, 0x4C(sp)    ; save fdwReason
800bfa6c  afa60050  sw    a2, 0x50(sp)    ; save lpvReserved
800bfa70  8fae0048  lw    t6, 0x48(sp)
800bfa74  3c0101fe  lui   at, 0x01FE
800bfa78  24040042  li    a0, 66
800bfa7c  0c7e3d35  jal   0x81f8f4d4       ; coredll-internal helper
800bfa80  ac2e6550  sw    t6, 0x6550(at)   ; *(0x01FE6550) = hinstDLL
800bfa84  50400060  beql  v0, zero, 0x800bfc08
800bfa88  24020001  li    v0, 1
800bfa8c  8fa7004c  lw    a3, 0x4C(sp)     ; a3 = fdwReason
800bfa90  24010001  li    at, 1
800bfa94  14e10032  bne   a3, at, 0x800bfb60   ; if not ATTACH, branch
800bfa98  24040003  li    a0, 3
800bfa9c  3c0501fe  lui   a1, 0x01FE
800bfaa0  3c0601fe  lui   a2, 0x01FE
800bfaa4  24c66544  addiu a2, a2, 0x6544    ; a2 = 0x01FE6544
800bfaa8  0c7e36ad  jal   0x81f8dab4         ; "install OAL slot"
800bfaac  24a56548  addiu a1, a1, 0x6548
... continues with table installs at 0x6544/6548/654C/6554/...
```

So coredll's DllMain IS the function that publishes the OAL callback
table that Phase AX read from coredll's `.data`. The "consumer at
`0x01F8F4D4`" Phase A chased is the FIRST helper coredll DllMain
itself calls (the `jal 0x81f8f4d4` at `0x800bfa7c`) — it is invoked
BEFORE any OAL slot is written.

MIPS subtlety: the immediate operand in `jal 0x81f8f4d4` is encoded
as `0x07e3d35 << 2`, and the target's high 4 bits come from the PC
at execution time. So when the same bytes execute from kseg0
(`0x800BFA7C`), the jal targets `0x81F8F4D4`; from slot-0
(`0x01F8FA7C`), it targets `0x01F8F4D4`. This is how WinCE 3.0 runs
one copy of coredll bytes from multiple VAs without relocations and
fully explains Phase A's `0x01F8F4D4`-as-fault-target finding.

## Phase AZ: Runtime Probe at FUN_8008FF00 Dispatch

### Probe design

Added `maybe_note_dllmain_dispatch_pc` in `src/wince_boot.c` and
extended the explicit PC switch in
`gxemul/src/cpus/cpu_mips_instr.c` to invoke `wince_boot_note_pc`
for four basic-block leaders:

- `0x800927CC` — FUN_800927CC entry (loader)
- `0x8008FF00` — FUN_8008FF00 entry (DllMain dispatcher)
- `0x80090050` — FUN_8008FF00 cleanup BB (post-dispatch return path)
- `0x800929D0` — FUN_800927CC bnez v0 just after `jal 0x8008ff00`
- `0x800929E4` — FUN_800927CC PC just after `jal 0x800903bc`
  (rollback)

Critical detail: the dyntrans `cpu_dyntrans.c` `note_pc` hook fires
only on first translation AND only when `wince.active == true` at
that exact moment. Since dyntrans translates instructions early
(potentially before NK's first cold-boot probe activates the WinCE
state), most NK PCs miss the hook. The reliable hook is the explicit
switch in `cpu_mips_instr.c:4400+`, which fires on EVERY execution
of the listed PCs. New probe PCs must be added there.

### Captured output

```text
[WINCE_LOADER_ENTRY] #1 pc=0x800927CC a0=0x00000000 a1=0x80070000
                        sp=0x0201FD90 ra=0x00001000

[WINCE_DLLMAIN_ENTRY] #1 pc=0x8008FF00 desc=0x80FFFEA8
                         reason=0x00000001 reserved=0x00000000
                         desc[+0x60]=0x01F84A5C(ok)
                         sp=0x0201FD90 ra=0x800929D0

[WINCE_DLLMAIN_RET]   #1 pc=0x80090050 v0_live=0x00000000
                         v0_saved@sp+2C=0x00000001
                         sp=0x0201FD60 ra=0x800929D0

[WINCE_DLLMAIN_RET]   #2 pc=0x80090050 v0_live=0x80000002
                         v0_saved@sp+2C=0x00000001
                         sp=0x0201FD60 ra=0x80090044

[WINCE_LOADER_POST_FF00]   #1 pc=0x800929D0 v0=0x03F84A5C
                              (==0 -> rollback) sp=0x0201FD90

[WINCE_LOADER_ROLLBACK_DONE] #1 pc=0x800929E4 v0=0x03F84A5C
                                sp=0x0201FD90
```

### What the probe confirms

- **DllMain dispatch fires with correct args**: `desc =
  0x80FFFEA8` is coredll, `reason = 1` is `DLL_PROCESS_ATTACH`,
  `reserved = 0`, and the resolved DllMain VA at `desc[+0x60] =
  0x01F84A5C` matches `e32_entryrva = 0x4A5C` plus
  `e32_vbase = 0x01F80000`. The dispatcher receives the right
  inputs.
- **The loader rollback IS taken**:
  `WINCE_LOADER_ROLLBACK_DONE` at PC `0x800929E4` only fires if
  the `jal 0x800903bc` at `0x800929DC` actually executed. Since
  the only way to reach `0x800929DC` is through `0x800929D0`'s
  bnez NOT branching, the loader saw `v0 == 0` at the bnez.
- **FUN_800927CC was called only once** (probe cap was 16 and
  saw exactly one entry). The rollback chain fires once per
  cold boot.
- **The walker still fires identically** to the pre-Phase-AY
  runs. `WINCE_HOT_L2W_REGS #25` matches the prior log byte for
  byte. So adding the dispatch probes does not change runtime
  behavior — they are pure observers.

### What the probe contradicts

There is a hard contradiction between two of the probe readings:

- `WINCE_LOADER_ROLLBACK_DONE` firing means the loader at
  `0x800929D0` saw `v0 == 0` and fell through to the rollback.
- `WINCE_DLLMAIN_RET v0_saved@sp+2C = 0x00000001` means the local
  return slot inside `FUN_8008FF00` holds the value 1 (success)
  at the moment execution reaches `0x80090050` (the function's
  cleanup BB before its `lw v0, 44(sp); jr ra` epilogue). The
  function should therefore return 1.

If the function returns 1, the loader's bnez should be taken and
the rollback should NOT fire. Yet both fire. The only way to
reconcile this:

1. **dyntrans `cpu->gpr` staleness.** The `cpu->pc` and `cpu->gpr`
   reads at the moment of `note_pc` invocation are not always in
   sync with the architectural state at that PC, especially for
   PCs deep inside cached IC blocks. The handoff already warned
   about this for store PCs in the walker. The same may apply to
   `v0` reads at `0x800929D0` (where `v0=0x03F84A5C` is shown,
   matching no observable function output in the chain), and may
   also apply to the `v0_saved@sp+2C` read if the load itself was
   completed by an in-flight IC handler that hadn't yet committed
   the prior `sw zero, 44(sp)` from the fallthrough path at
   `0x8009004C`.
2. **Two distinct invocations of FUN_8008FF00 with overlapping
   sp.** If a different code path called `FUN_8008FF00` with the
   same `sp = 0x0201FD60` between our captures, that invocation's
   stack overwrote the visible slot. But the probe only captured
   one `WINCE_DLLMAIN_ENTRY`, so this is unlikely unless the
   second call's entry PC is mapped to a different IC slot than
   ours.
3. **An execution path inside FUN_8008FF00 that sets v0 to 0
   without storing to sp+0x2C.** Looking at the function, the
   normal return path is `lw v0, 44(sp); jr ra`, which reads from
   the slot. If the function takes a path that branches around
   the load, v0 could be whatever the previous instruction left
   it. Re-disassembling the entire function (not just the
   dispatch block) might reveal such a path.

### Important reads the probe DID get right

- `desc[+0x60] = 0x01F84A5C` was read via `load_va_word` at the
  moment of dispatch entry, and matches the static
  `e32_entryrva` calculation. So at this exact moment, the
  descriptor points at coredll's real DllMain VA. There is no
  "stale descriptor pointer" bug.
- `desc = 0x80FFFEA8` is coredll's well-known module descriptor
  per Phase AC's TOC name lookup. The right module is being
  passed.
- `reason = 1` is DLL_PROCESS_ATTACH. The first DllMain call is
  what is firing, not a per-process detach.

## Updated Failure Chain

```text
1. NK boots, reaches first user process startup.
2. NK creates filesys.exe (slot 1) and starts FUN_800927CC to load
   coredll into slot-1.
3. The loader maps coredll's PE32 sections into slot-0 and slot-1
   mirror VAs. Both 0x01F84A5C (slot-0 DllMain VA) and 0x800BFA5C
   (kseg0 DllMain VA) are valid at this point.
4. The loader resolves coredll[+0x60] = 0x01F84A5C via FUN_8009096C.
5. At 0x800929C0..0x800929C8, FUN_800927CC calls
   FUN_8008FF00(coredll, 1, 0). [CONFIRMED by Phase AZ probe]
6. FUN_8008FF00 attempts to dispatch via (*coredll[+0x60])(coredll, 1, 0)
   at 0x80090024 or 0x8009003C. **Whether the dispatch actually
   executes coredll's DllMain bytes or returns early via the flag
   checks at 0x8008FF94 / 0x8008FFC4 is the open question.**
7. FUN_8008FF00 returns 0 to the loader. [INFERRED from rollback
   firing]
8. FUN_800927CC sees v0 == 0 at 0x800929D0, falls through to
   `jal 0x800903BC` at 0x800929DC, then sets error 0x45A at
   0x800929E8. [CONFIRMED by Phase AZ ROLLBACK_DONE probe]
9. FUN_800903BC -> FUN_80090144 -> FUN_8008FE8C -> FUN_80097F44 ->
   FUN_800970A8 zeros coredll's tail PTEs (text tail, .data, .pdata
   — 14 pages, 56 KB). [CONFIRMED by walker probes]
10. After the unmap, the OAL callback table at 0x01FE6544..0x01FE65A4
    reads zero, and the next coredll dispatch through it faults
    with Exception 004.
```

## Recommended Next Steps (Phase BA)

1. **Disassemble the ENTIRE FUN_8008FF00 body**, not just the
   dispatch block, with attention to every store to `sp+0x2C` and
   every branch that reaches `0x80090050` or `0x80090074..0x80090080`
   (the epilogue). The goal is to enumerate every return path and
   what `v0` is at each. If there is a path that returns 0 without
   storing to `sp+0x2C`, that explains the contradiction.

2. **Add a probe at the actual jalr post-PC, not at `0x80090050`.**
   The basic-block leaders `0x80090030` (after first `jalr t7`) and
   `0x80090048` (after second `jalr t8`) are the points where the
   dispatch return value is fresh in `v0`. Add them to the
   `cpu_mips_instr.c` switch, then re-run.

3. **Probe coredll DllMain entry directly.** PC `0x01F84A5C` is
   already in the dyntrans `note_pc` allowlist
   (`cpu_dyntrans.c:2240`). Add a hook in `wince_boot.c` for that
   PC that captures `a0/a1/a2/sp/ra` at entry and at `jr ra` exit.
   This will tell us authoritatively whether DllMain ever runs
   coredll bytes at all.

4. **Probe the early-skip branches inside FUN_8008FF00.** The two
   conditional branches at `0x8008FF94` (`bnez t7, 0x80090050`)
   and `0x8008FFC4` (`beqz t2, 0x80090050`) decide whether the
   dispatch is taken. If the function is taking one of these
   skip branches on cold boot, the dispatch is being skipped
   entirely and `v0_saved@sp+2C = 1` is correct (the function
   returns 1 = success). In that case, the rollback is being
   triggered NOT by a DllMain return value but by something else
   in the loader — possibly an exception or interrupt during the
   skipped dispatch path that the dyntrans probe doesn't see.

5. **Do NOT pursue these dead leads:**
   - the `0xFFFFD800` decoding (it's NK globals base),
   - the publisher loop at `0x80098484` (different function,
     unrelated to the walker that fires on cold boot),
   - the EXCEPTION directory walk (dead since Phase AS),
   - more kernel-error reads (the error is set after rollback).

## Files Changed

Committed in this session:

- `docs/WINCE_COLD_BOOT_SESSION_2026-04-14.md` (new file).
- `src/wince_boot.c` — added `maybe_note_dllmain_dispatch_pc` and
  wired it into `wince_boot_note_pc`. ~70 lines added,
  no removals.

Worktree-only (NOT committed; intentionally left as worktree change
to avoid touching the unrelated pre-existing `gxemul/` modifications
inherited from earlier sessions):

- `gxemul/src/cpus/cpu_mips_instr.c` — added 5 PCs
  (`0x800927CC`, `0x800929D0`, `0x800929E4`, `0x8008FF00`,
  `0x80090050`) to the explicit `note_pc` switch around line 4472.
  Required for the probes added in `src/wince_boot.c` to actually
  fire. Without this gxemul change, the probes are dead code.

  Future sessions: either re-apply this 5-line addition to
  `gxemul/src/cpus/cpu_mips_instr.c` before re-running, or include
  the addition in a focused `gxemul/` commit that does NOT touch
  the unrelated pre-existing worktree changes (they are
  load-bearing for other parts of the cold-boot work and should
  not be reverted).

The reference run for this session is
`build-host/repro_phase_az.stderr`.

The unrelated worktree changes in `gxemul/` (other than the
`cpu_mips_instr.c` switch addition) and `src/be300_devices.c` from
prior sessions are still present and untouched.
