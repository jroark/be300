# WinCE NAND Cold-Boot Session Report — Phases AY, AZ, and BA

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

## Phase BA Addendum: Direct DllMain Probes and dyntrans note_pc Limitation

### What was added

Phase BA extended the Phase AZ probes with three more PC hooks:

- `0x01F84A5C` — coredll DllMain entry (already in dyntrans
  allowlist) → `WINCE_DLLMAIN_REAL_ENTRY`
- `0x01F84C04` — `li v0, 1` instruction inside DllMain epilogue
  → `WINCE_DLLMAIN_LI1`
- `0x01F84C0C` — `jr ra` of DllMain (the function epilogue)
  → `WINCE_DLLMAIN_EXIT`
- `0x80090030` / `0x80090048` — PCs immediately after the two
  jalr dispatch sites in FUN_8008FF00
  → `WINCE_DLLMAIN_POST_JALR1` / `WINCE_DLLMAIN_POST_JALR2`
- `0x8009004C` — the supposedly-dead `sw zero, 44(sp)`
  → `WINCE_DLLMAIN_DEAD_ZERO`

All hooks were added to the explicit `wince_boot_note_pc` switch in
`gxemul/src/cpus/cpu_mips_instr.c:4357` (worktree-only, NOT
committed; same caveat as Phase AZ).

### What the probes captured

```text
[WINCE_LOADER_ENTRY]    #1 pc=0x800927CC a0=0x00000000 a1=0x80070000
                           sp=0x0201FD90 ra=0x00001000

[WINCE_DLLMAIN_ENTRY]   #1 pc=0x8008FF00 desc=0x80FFFEA8
                           reason=0x00000001 reserved=0x00000000
                           desc[+0x60]=0x01F84A5C(ok)
                           sp=0x0201FD90 ra=0x800929D0

[WINCE_DLLMAIN_REAL_ENTRY] #1 pc=0x01F84A5C
                              a0=0x80FFFEA8 a1=0x00000001
                              a2=0x00000000 sp=0x0201FD60
                              ra=0x80090044 (DllMain DID run)

[WINCE_DLLMAIN_LI1]     #1 pc=0x01F84C04 v0_before=0x00000000
                           (li v0,1 about to execute)

[WINCE_DLLMAIN_EXIT]    #1 pc=0x01F84C0C v0=0x00000000 v1=0x00000000
                           sp=0x0201FD60 ra=0x80090044

[WINCE_DLLMAIN_POST_JALR2] #1 pc=0x80090048 v0=0x00000000
                              sp=0x0201FD60 (2nd jalr returned)

[WINCE_DLLMAIN_DEAD_ZERO] #1 pc=0x8009004C sp=0x0201FD60
                             (UNEXPECTED HIT)
[WINCE_DLLMAIN_DEAD_ZERO] #2 pc=0x8009004C sp=0x0201FD60
                             (UNEXPECTED HIT)

[WINCE_LOADER_POST_FF00]    #1 pc=0x800929D0 v0=0x03F84A5C
                               (==0 -> rollback) sp=0x0201FD90
[WINCE_LOADER_ROLLBACK_DONE] #1 pc=0x800929E4 v0=0x03F84A5C
                                sp=0x0201FD90
```

### What is now firmly established

- **DllMain at slot-0 `0x01F84A5C` IS executed** with the correct
  arguments (a0 = coredll desc, a1 = 1 = ATTACH, a2 = 0,
  ra = `0x80090044`). The dispatch flows from FUN_8008FF00 via the
  second jalr at `0x8009003C` (DISPATCH 2), which is the
  `desc[+0x84] == 0` path of the BEQL at `0x8008FFE8`.
- **The loader rollback IS taken**. `WINCE_LOADER_ROLLBACK_DONE` at
  `0x800929E4` only fires if the `jal 0x800903bc` at `0x800929DC`
  actually executed.
- **Coredll DllMain reaches its epilogue**. The `WINCE_DLLMAIN_LI1`
  probe at `0x01F84C04` and the `WINCE_DLLMAIN_EXIT` probe at
  `0x01F84C0C` both fire, so the cpu's IC translation for these PCs
  is being touched on this run.
- **`WINCE_DLLMAIN_DEAD_ZERO` fires twice** at `0x8009004C`
  (`sw zero, 44(sp)`). This instruction sits between the
  unconditional `b 0x80090050` at `0x80090044` and its target. In
  normal control flow it should never be executed. The fact that
  the probe fires confirms that `wince_boot_note_pc` is being
  invoked on IC slots that are NOT actually being executed —
  see "Why the probes are unreliable" below.

### What the probes CANNOT tell us reliably

- **Whether coredll DllMain actually returns 0 or 1.** Every v0
  reading in the captured output is consistent with `v0 = 0`, but
  the static disasm of DllMain shows every reachable return path
  setting `v0 = 1` (either via `li v0, 1` at `0x800bfc04` or via
  the BEQL delay slot `li v0, 1` at `0x800bfa88`). One of two
  things is happening:
  1. DllMain IS returning 0 via a code path that is not covered
     by the partial disasm we have, or via an exception/longjmp
     that bypasses the normal epilogue, or via a syscall
     trampoline (`jalr 0xFFFFBB76` at `0x800bfb34` is an
     in-DllMain syscall — it could be the failure point).
  2. The probe's `cpu->cd.mips.gpr[V0]` reads are not architecturally
     meaningful at note_pc fire time, and v0 is actually 1.

### Why the probes are unreliable

The `wince_boot_note_pc` callback is invoked from inside
`X(to_be_translated)` in
`gxemul/src/cpus/cpu_mips_instr.c:4293` (and again from
`cpu_dyntrans.c:2249`). Both call sites fire **only when an IC
slot is being translated for the first time**, BEFORE the decoded
instruction is actually executed.

Three concrete consequences:

1. **note_pc fires before the IC handler runs.** At
   `note_pc(pc=0x01F84C04)`, the `li v0, 1` instruction has NOT
   executed yet. `v0_before` correctly shows the pre-instruction
   value (whatever the previous helper jal returned).
2. **Translation can pre-fetch nearby IC slots.** The
   `WINCE_DLLMAIN_DEAD_ZERO` firing at `0x8009004C` proves that
   note_pc is being invoked for IC slots that are NOT being
   executed on the actual control flow path — they are merely in
   the same IC page as PCs that ARE being executed. Pre-translation
   pollution explains both the dead_zero hits and the `v0 = 0`
   readings at PCs that should logically have `v0 = 1`.
3. **gpr state at note_pc time may not reflect the architectural
   state at pc32.** The PCs that note_pc has touched include slots
   that are pre-translated as part of an IC page sweep, so the
   `cpu->gpr` array at those times is whatever the cpu had when it
   first needed any IC slot in that page — not necessarily what
   the cpu had immediately before pc32.

This means the entire Phase AZ + Phase BA register-based reasoning
about "v0 is 0 → DllMain returns 0" is at best circumstantial.
What IS definitive is the loader's *behavior*: the rollback
`jal 0x800903bc` at `0x800929DC` IS being taken, and the only way
it gets taken is if `v0 == 0` at the bnez at `0x800929D0`. So
either FUN_8008FF00 returns 0 (because DllMain returned 0, or
because of an early-skip path), or the bnez itself is being
mispredicted by the emulator.

### Fully decoded FUN_8008FF00 body

```text
8008ff00  addiu sp,sp,-48        ; prologue
8008ff04  sw    ra,28(sp)
8008ff08  sw    a2,56(sp)         ; spill caller's a2 slot
8008ff0c  li    a3,-10240          ; a3 = NK kernel globals base 0xFFFFD800
8008ff10  li    t6,1
8008ff14  sw    t6,44(sp)          ; sp+0x2C = 1 (initial return value)
8008ff18  lw    v1,704(a3)         ; v1 = *(0xFFFFDAC0) = ctx_ptr
8008ff1c  lw    t7,56(v1)          ; t7 = ctx[+0x38] = old kernel error
8008ff20  sw    t7,40(sp)          ; save old error
8008ff24  lw    t8,96(a0)          ; t8 = desc[+0x60] = DllMain ptr
8008ff28  beql  t8,zero,0x80090074 ; if DllMain ptr == 0, skip dispatch
8008ff2c  lw    t1,40(sp)          ; (delay slot, taken-only)
8008ff30  lhu   t9,200(a0)         ; t9 = (uint16_t)desc[+0xC8]
8008ff34  andi  t0,t9,1
8008ff38  bnel  t0,zero,0x80090070 ; if (desc[+0xC8] & 1) != 0, skip
8008ff3c  lw    t1,40(sp)          ; (delay slot, taken-only)
8008ff40  lhu   t1,0(v1)
8008ff44  li    at,1
8008ff48  sra   t2,t1,0x6
8008ff4c  andi  t3,t2,1
8008ff50  bne   t3,at,0x8008ff70   ; if bit-6 of *(ctx_ptr) != 1, skip helper
8008ff54  sw    t3,36(sp)
8008ff58  sw    a0,48(sp)
8008ff5c  jal   0x800896cc          ; helper
8008ff60  sw    a1,52(sp)
8008ff64  lw    a0,48(sp)
8008ff68  lw    a1,52(sp)
8008ff6c  li    a3,-10240
8008ff70  li    at,1
8008ff74  bne   a1,at,0x8008ffa4    ; if reason != 1 (not ATTACH), branch
8008ff78  nop
;== ATTACH path (a1==1) ==
8008ff7c  lw    t4,708(a3)          ; t4 = *(0xFFFFDAC4) = curproc
8008ff80  lw    v0,16(a0)           ; v0 = desc[+0x10] (per-slot loaded mask)
8008ff84  lbu   t5,0(t4)            ; t5 = curproc.slot_idx
8008ff88  li    t6,1
8008ff8c  sllv  v1,t6,t5            ; v1 = 1 << slot_idx
8008ff90  and   t7,v0,v1
8008ff94  bnez  t7,0x80090050       ; if already loaded for this slot, SKIP
                                     ; (returns sp+0x2C = 1 = success)
8008ff98  or    t8,v0,v1            ; (delay slot)
8008ff9c  b     0x8008ffd4
8008ffa0  sw    t8,16(a0)            ; (delay slot) mark loaded
;== DETACH path (a1!=1) ==
8008ffa4  bnel  a1,zero,0x8008ffdc  ; if a1 != 0 (THREAD detach etc.), skip cleanup
8008ffa8  sw    a0,48(sp)
8008ffac  lw    t9,708(a3)
8008ffb0  lw    v0,16(a0)
8008ffb4  lbu   t0,0(t9)
8008ffb8  li    t1,1
8008ffbc  sllv  v1,t1,t0
8008ffc0  and   t2,v0,v1
8008ffc4  beqz  t2,0x80090050        ; if NOT loaded, SKIP (returns 1)
8008ffc8  nor   t3,v1,zero
8008ffcc  and   t4,v0,t3
8008ffd0  sw    t4,16(a0)             ; mark unloaded
;== Common dispatch setup ==
8008ffd4  sw    a0,48(sp)
8008ffd8  sw    a1,52(sp)
8008ffdc  lw    a0,48(sp)
8008ffe0  lw    a1,52(sp)
8008ffe4  lw    v1,132(a0)            ; v1 = desc[+0x84]
8008ffe8  beql  v1,zero,0x80090038    ; if desc[+0x84] == 0, take DISPATCH 2
8008ffec  lw    t8,96(a0)             ; (delay slot, taken-only) t8 = DllMain
8008fff0  lw    v0,84(a0)             ; v0 = desc[+0x54] = slot base
8008fff4  lui   at,0x1ff
8008fff8  sll   t6,v0,0
8008fffc  bgez  t6,0x8009000c
80090000  ori   at,at,0xffff           ; (delay) at = 0x01FFFFFF
80090004  b     0x80090010
80090008  move  a3,v0                  ; (delay) a3 = slot base
8009000c  and   a3,v0,at                ; a3 = slot offset
80090010  sw    v1,16(sp)
80090014  lw    t5,136(a0)              ; t5 = desc[+0x88]
80090018  lw    a2,56(sp)               ; restore lpvReserved
8009001c  sw    t5,20(sp)
80090020  lw    t7,96(a0)               ; t7 = DllMain
80090024  jalr  t7                       ; *** DISPATCH 1 (ATTACH path) ***
80090028  nop
8009002c  b     0x80090050
80090030  sw    v0,44(sp)                ; (delay) save return value
;== DISPATCH 2 (entered via beql at 8008ffe8 if desc[+0x84]==0) ==
80090034  lw    t8,96(a0)
80090038  lw    a2,56(sp)
8009003c  jalr  t8                       ; *** DISPATCH 2 (DETACH path) ***
80090040  nop
80090044  b     0x80090050
80090048  sw    v0,44(sp)                ; (delay) save return value
8009004c  sw    zero,44(sp)              ; "dead code" - apparently reached on some path
80090050  lw    t9,36(sp)                ; cleanup label
80090054  li    at,1
80090058  bnel  t9,at,0x80090070
8009005c  lw    v1,-9536(zero)           ; (delay, taken-only)
80090060  jal   0x800896cc                ; helper
80090064  nop
80090068  lw    v1,-9536(zero)
8009006c  lw    t1,40(sp)
80090070  sw    t1,56(v1)                  ; restore old kernel error
80090074  lw    ra,28(sp)
80090078  lw    v0,44(sp)                  ; v0 = saved return value
8009007c  jr    ra
80090080  addiu sp,sp,48
```

The crucial observation: **the ATTACH path takes DISPATCH 1
(`jalr t7` at `0x80090024`), not DISPATCH 2 (`jalr t8` at
`0x8009003C`)**. DISPATCH 2 is only reached when `desc[+0x84] == 0`
via the BEQL at `0x8008FFE8`.

But our `WINCE_DLLMAIN_REAL_ENTRY` probe captured `ra = 0x80090044`
at DllMain entry. `ra = 0x80090044` is the post-`jalr t8` PC, which
means the dispatch came via DISPATCH 2 — i.e., `desc[+0x84] == 0`.

So **coredll's `desc[+0x84]` is zero**, which forces FUN_8008FF00
to take DISPATCH 2. This may itself be the bug. The descriptor's
`+0x84` is module-flag-related; if it should be nonzero on real
hardware (set by the loader during `FUN_800927CC`'s setup), then
the loader is initializing this field incorrectly.

The Phase BA probe data is consistent with this: only DISPATCH 2
ever fires, only one DllMain entry was captured, and the dispatch
itself appears to reach DllMain's epilogue on at least the
`note_pc` path.

## Recommended Next Steps (Phase BB)

The Phase BA experience exposes a real instrumentation limitation:
PC-based note_pc probes cannot reliably read register state because
they fire from within `X(to_be_translated)` (translation time), not
from inside the executing IC handler. Some next-step options that
work around this:

1. **Read coredll's `desc[+0x84]` at runtime** via a probe at
   FUN_800927CC's setup of the descriptor, or via a stack walk
   from FUN_8008FF00's frame just after `lw v1, 132(a0)`. The
   value of `desc[+0x84]` is the determining factor for which
   dispatch path FUN_8008FF00 takes. If it differs between
   emulator and real hardware, that may be the true root cause.

2. **Add a memory write trap on coredll's `desc[+0x84]`**. The
   field lives at `0x80FFFEA8 + 0x84 = 0x80FFFF2C`. A targeted
   memory-write watchpoint (similar to the existing
   `WINCE_HOT_L2W` trap) would fire at every store to that VA
   and capture pc/sp/ra at each one. This lets us trace exactly
   when and how `desc[+0x84]` is set.

3. **Run a parallel boot with the existing `--ppsh` flag** to
   compare descriptor state between the GUI-boot path (where the
   dispatch presumably succeeds on real hardware) and the
   emulator's failing path. If the descriptor's
   `+0x84` differs, that's a direct comparison point.

4. **Static analysis of the next portion of FUN_800927CC**, focusing
   on every store to `desc[+0x84]` (offset 132 from descriptor
   pointer). The setup happens between FUN_800927CC entry
   (`0x800927CC`) and the dispatch site (`0x800929C8`). Search
   for `sw rN, 132(...)` instructions in that range.

5. **Add an instruction-handler-level probe** that bypasses the
   translation-time note_pc. The cleanest place is to instrument
   one of the existing X() handler families (e.g., X(jalr) and
   X(jr)) to capture pc/v0 at execution time. This is more
   invasive but provides architecturally accurate state.

Do NOT pursue:

- More note_pc-based register reads. They are fundamentally
  unreliable for pre-instruction state.
- The "decode why DllMain returns 0" angle until we know whether
  DllMain is even being dispatched correctly. The DISPATCH 1 vs
  DISPATCH 2 finding suggests the loader is calling DllMain via
  the wrong path, not that DllMain itself is failing.

## Files Changed

Committed in this session:

- `docs/WINCE_COLD_BOOT_SESSION_2026-04-14.md` (new file).
- `src/wince_boot.c` — added `maybe_note_dllmain_dispatch_pc` and
  wired it into `wince_boot_note_pc`. ~70 lines added,
  no removals.

Worktree-only (NOT committed; intentionally left as worktree change
to avoid touching the unrelated pre-existing `gxemul/` modifications
inherited from earlier sessions):

- `gxemul/src/cpus/cpu_mips_instr.c` — added 11 PCs total across
  Phase AZ + Phase BA: `0x800927CC`, `0x800929D0`, `0x800929E4`,
  `0x8008FF00`, `0x80090050`, `0x80090030`, `0x80090048`,
  `0x8009004C`, `0x01F84C0C`, `0x01F84C04` to the explicit
  `note_pc` switch around line 4472.
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
