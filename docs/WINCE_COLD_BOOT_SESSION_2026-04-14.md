# WinCE NAND Cold-Boot Session Report — Phase AY

Date: 2026-04-14

## Goal

Continue the BE-300 WinCE 3.0 cold-boot work from the 2026-04-13 handoff
(`docs/WINCE_COLD_BOOT_SESSION_2026-04-13.md`). The handoff's Phase AX
addendum directed the next session to:

1. Disassemble `FUN_80097F44` end-to-end and decode `s0 = 0xFFFFD800`.
2. Find the writer of `0xFFFFD800` in NK.
3. Locate OAL-callback dispatch sites in coredll's `.text`.
4. Add ONE new runtime probe (`WINCE_FE8C_FRAME`).
5. Re-verify the upper caller chain from existing log.

This session is pure static analysis. **No emulator builds or runs were
needed and no new runtime probes were added.** Every answer came from
re-disassembling `build-host/nk_decompressed.bin` and re-reading the
existing `build-host/repro_phase_at.stderr` more carefully than prior
phases did.

## Bottom Line

The Phase AX hypothesis ("walker is incorrectly told to discard
coredll's tail; decode `s0 = 0xFFFFD800` to find out why") was
**fundamentally wrong on three independent points**, and the original
pre-Phase-AS hypothesis ("loader rolls back coredll because DllMain
returns 0") is **right after all**. Specifically:

1. `s0 = 0xFFFFD800` is **not** a walker parameter. It is the NK
   kernel-globals base, set by `li s0, -10240` at `0x80090184` inside
   `FUN_80090144`, and used purely as a callee-saved base pointer for
   later `lw rN, off(s0)` accesses to per-process kernel globals at
   `0xFFFFD800..0xFFFFDFFF`. Walker dutifully spills it because it is
   already in s0 when walker is entered, but walker never reads it as
   a parameter. The Phase AX/AW "decode 0xFFFFD800" target was a red
   herring.
2. Phase AV's claim that "publisher loop at `0x80098484` and walker at
   `0x800970A8` live inside the same NK function `FUN_80097F44`" is
   **wrong**. `FUN_80097F44` (entry `0x80097F44`) is a 80-byte-frame
   function that ends at `0x8009817C` (`jr ra; addiu sp, sp, 80`). It
   does **not** save or use any s-registers — only `ra`. The publisher
   loop at `0x80098484` lives in a separate function whose entry is
   `0x80098180` (frame `-152`, saves `s0..s8`). They are two distinct
   NK routines that happen to be adjacent.
3. **Phase AE's "DllMain didn't fail because the kernel error is 0"
   was a false negative.** The error code `0x45A` is set at
   `0x800929E8` *after* the rollback returns, not before. Reading the
   error field at walker-entry time will always show 0 regardless of
   whether the trigger was DllMain-failure. Phase AE only ruled out
   paths that set the error *before* calling `FUN_800903BC`, which is
   none of them.

The actual cold-boot bug, restated: **coredll's `DllMain(coredll, 1, 0)`
runs and returns 0**. The WinCE loader at `FUN_800927CC + 0x210`
checks the return, decides initialization failed, and rolls back
coredll's load via `FUN_800903BC -> FUN_80090144 -> FUN_8008FE8C ->
FUN_80097F44 -> FUN_800970A8`, which legitimately zeros coredll's tail
PTEs (text tail + data + pdata, 14 pages, the live OAL callback table
included). After the rollback, any code that tries to reach coredll
through the now-zeroed mappings faults with Exception 004.

The next investigation pass should answer: **why does coredll's
DllMain return 0 on emulator?**

## What Was Decoded

### 1. The walker's caller chain (re-verified from existing log)

The existing run `build-host/repro_phase_at.stderr` already captures a
48-word `WINCE_STACK_WALK` dump from `caller_sp = 0x0201FD30`. Decoded
against the NK objdump, this gives the entire chain in one read:

```text
caller_sp = 0x0201FD30    (= FUN_8008FE8C body sp)

sp+0x14 = 0x80090240   FUN_8008FE8C saved_ra → inside FUN_80090144
                       (return PC after `jal 0x8008fe8c` @ 0x80090238)

sp+0x20 = 0x80FFFEA8   first word of FUN_80090144's frame (its s1)
                       = coredll module descriptor pointer

sp+0x3C = 0x8009040C   FUN_80090144 saved_ra → inside FUN_800903BC
                       (return PC after `jal 0x80090144` @ 0x80090404)

sp+0x54 = 0x800929E4   FUN_800903BC saved_ra → inside FUN_800927CC
                       (return PC after `jal 0x800903bc` @ 0x800929DC)
```

So no new runtime probe is needed for the upper chain. The full call
stack at walker-entry time is:

```text
FUN_800970A8  (walker, 0x80097F44 → 0x800970A8 via jal @ 0x8009813C)
FUN_80097F44  (frame -80, ra at sp+0x1C, no s-regs)
FUN_8008FE8C  (frame -32, ra at sp+0x14)
FUN_80090144  (frame -32, ra at sp+0x1C, s0/s1 at sp+0x14/0x18)
FUN_800903BC  (frame -32, ra at sp+0x14, recursion-guarded wrapper)
FUN_800927CC  (WinCE DLL loader, callsite 0x800929DC)
```

### 2. FUN_800927CC at 0x800929C0 — the actual trigger

```asm
800929bc  beqz s1, 0x800929f0       ; skip if s1==0 (early failure flag)
800929c0  move a0, s2                ; (delay) a0 = coredll desc
800929c4  li   a1, 1                  ; a1 = DLL_PROCESS_ATTACH
800929c8  jal  0x8008ff00              ; FUN_8008FF00(coredll, 1, 0)
800929cc  move a2, zero
800929d0  bnez v0, 0x800929f0           ; skip rollback if v0 != 0 (success)
800929d4  move a0, s2
800929d8  move a1, zero
800929dc  jal  0x800903bc                ; *** ROLLBACK CALL ***
800929e0  move s1, zero
800929e4  lw   t7, 704(s3)
800929e8  li   t9, 0x45A                  ; ERROR_DLL_INIT_FAILED, AFTER rollback
800929ec  sw   t9, 56(t7)
```

Two important consequences:

- `v0 == 0` after `FUN_8008FF00(coredll, 1, 0)` is the trigger.
  `FUN_8008FF00` is a thin dispatcher whose return value is just
  whatever `(*coredll[+0x60])(coredll, 1, 0)` returned. So
  `v0 == 0` ⇔ DllMain returned 0.
- The kernel error 0x45A is set at `0x800929E8`, which is reached
  only AFTER `jal 0x800903bc` at `0x800929DC` returns. Phase AE's
  walker-entry-time read of the error field returned 0 because the
  rollback was still in flight. It does **not** disprove the
  DllMain-failure hypothesis.

### 3. FUN_8008FF00 — the DllMain dispatcher

`FUN_8008FF00` (`0x8008FF00`, frame -48) saves `ra/a2`, reads
`desc[+0x60]` (the resolved PE32 entry VA), checks a couple of
descriptor flags, then dispatches via:

```asm
80090020  lw   t7, 96(a0)         ; t7 = desc[+0x60] (DllMain ptr)
80090024  jalr t7                  ; call DllMain
80090028  nop
8009002c  b 0x80090050
80090030  sw   v0, 44(sp)          ; save return value
```

(There is a second `jalr` at `0x8009003C` for the alternate flag
path; both lead to the same `sw v0, 44(sp)` slot.) The function
returns `*(sp+44)` in `v0`, which is exactly DllMain's return value.

### 4. Coredll DllMain at slot-0 0x01F84A5C / kseg0 0x800BFA5C

Phase AS's "valid 3-arg prologue" reading is correct. The **right**
file offset to disassemble coredll `.text` from `nk_decompressed.bin`
is `--adjust-vma=0x80060000 --start-address=0x800BFA5C` (NK base, not
some coredll-specific base). Phase AX's slice reads were done at the
correct offset; my own first attempt with `--adjust-vma=0x800BB000`
landed in the data section and decoded as the `0xF800F800` pattern.

The actual DllMain prologue and start-of-body:

```asm
800bfa5c  27bdffb8  addiu sp, sp, -72
800bfa60  afbf0014  sw    ra, 0x14(sp)
800bfa64  afa40048  sw    a0, 0x48(sp)    ; save hinstDLL
800bfa68  afa5004c  sw    a1, 0x4C(sp)    ; save fdwReason
800bfa6c  afa60050  sw    a2, 0x50(sp)    ; save lpvReserved
800bfa70  8fae0048  lw    t6, 0x48(sp)
800bfa74  3c0101fe  lui   at, 0x01FE
800bfa78  24040042  li    a0, 66           ; selector code
800bfa7c  0c7e3d35  jal   0x81f8f4d4       ; coredll-internal helper
800bfa80  ac2e6550  sw    t6, 0x6550(at)   ; *(0x01FE6550) = hinstDLL
800bfa84  50400060  beql  v0, zero, 0x800bfc08  (see note)
800bfa88  24020001  li    v0, 1
800bfa8c  8fa7004c  lw    a3, 0x4C(sp)     ; a3 = fdwReason
800bfa90  24010001  li    at, 1
800bfa94  14e10032  bne   a3, at, 0x800bfb60   ; if not ATTACH, branch
800bfa98  24040003  li    a0, 3             ; (delay) selector
800bfa9c  3c0501fe  lui   a1, 0x01FE
800bfaa0  3c0601fe  lui   a2, 0x01FE
800bfaa4  24c66544  addiu a2, a2, 0x6544    ; a2 = 0x01FE6544
800bfaa8  0c7e36ad  jal   0x81f8dab4         ; "install OAL slot" helper
800bfaac  24a56548  addiu a1, a1, 0x6548    ; (delay) a1 = 0x01FE6548
800bfab0  3c0501fe  lui   a1, 0x01FE
800bfab4  24a56554  addiu a1, a1, 0x6554    ; a1 = 0x01FE6554
800bfab8  24040004  li    a0, 4
800bfabc  0c7e36ad  jal   0x81f8dab4
800bfac0  00003025  move  a2, zero
... continues with more table installs at 0x6544/6548/654C/6554/...
```

So coredll's DllMain **is exactly the function that publishes the OAL
callback table that Phase AX read from coredll's `.data`**. The
"consumer at `0x01F8F4D4`" that we have been chasing since Phase A is
the FIRST helper DllMain calls (the `jal 0x81f8f4d4` at `0x800bfa7c`)
— it is the same function, not a stale victim. It is invoked **before
any OAL slot has been written**.

Important MIPS subtlety: the immediate operand in `jal 0x81f8f4d4` is
encoded as `0x07e3d35 << 2`, and the target's high 4 bits come from
the PC at execution time. So when DllMain executes from kseg0
(`0x800BFA7C`), the jal targets `0x81F8F4D4` (kseg0). When the same
bytes execute from slot-0 (`0x01F8FA7C`), the same `jal` instruction
targets `0x01F8F4D4` (slot-0 user VA). This is how WinCE 3.0 supports
running one copy of coredll bytes from multiple VAs without
relocations. It also fully explains Phase A's
`0x01F8F4D4`-as-fault-target finding without any "consumer cached a
stale pointer" speculation.

The line at `0x800bfa84` (`beql v0, zero, +0x180`) is BEQL, "branch
likely if equal". If the first helper returns `v0 == 0`, the branch is
taken AND the `li v0, 1` delay slot is executed, and DllMain jumps to
the success exit. If the helper returns nonzero, the branch is not
taken, the delay slot is annulled, and DllMain falls through to the
ATTACH publish path. Both branches of this BEQL look like normal
DllMain control flow on real hardware; nothing here screams "always
returns 0".

### 5. FUN_80090144 — where 0xFFFFD800 actually comes from

```asm
80090144  addiu sp, sp, -32
80090148  sw    s1, 24(sp)
8009014c  move  s1, a0                  ; s1 = caller's a0 = module desc
80090150  sw    ra, 28(sp)
80090154  sw    s0, 20(sp)
80090158  sw    a1, 36(sp)
... per-process refcount checks via desc[+0xC0] ...
80090184  li    s0, -10240               ; *** s0 = 0xFFFFD800 ***
80090188  lw    t0, 708(s0)              ; t0 = *(0xFFFFDAC4) = curproc
8009018c  lbu   t1, 0(t0)                ; t1 = curproc.slot_idx
... uses s0 throughout the body as kernel-globals base ...
800901c0  jal   0x8008ff00                ; **FUN_8008FF00(desc, 0, 0)**
800901c4  move  a2, zero
... more decref / cleanup logic ...
80090238  jal   0x8008fe8c                ; FUN_8008FE8C(desc) → triggers walker
8009023c  move  a0, s1
... continues with FUN_80090084, FUN_8008E724, FUN_80081308, etc.
800903b8  addiu sp, sp, 32                ; epilogue
```

Three things to notice:

- `s0 = -10240 = 0xFFFFD800` is NK's per-process kernel-globals base.
  It is a constant chosen so `lw rN, 0x2C0(s0)` reaches `0xFFFFDAC0`,
  `lw rN, 0x2C4(s0)` reaches `0xFFFFDAC4`, etc. Many NK functions use
  this trick (see also `lw v1, -9536(zero)` at `0x800903F4`, which
  reaches `0xFFFFDAC0` directly via the `zero` register). The `0xD800`
  has zero load-bearing meaning beyond "low 16 bits of a sign-extended
  constant that chains to NK kernel globals".
- `FUN_80090144` calls `FUN_8008FF00(desc, a1, 0)` at `0x800901C0`.
  When the per-process refcount reaches zero on a *different* path
  through `FUN_80090144`, it dispatches DllMain again with reason 0
  (`DLL_PROCESS_DETACH`). This is the per-process unload path, not
  the loader-attach path. The walker fires after this DETACH on the
  rollback chain too, but only if FUN_800927CC's earlier ATTACH
  decided to roll back.
- `FUN_80090144` does NOT touch s0 across its `jal 0x8008fe8c` call,
  so walker inherits whatever value s0 has at the time, which is
  `0xFFFFD800`. Walker spills it as part of saving the caller's
  callee-saved set. **Walker never reads s0 as a parameter.**

### 6. FUN_8008FE8C — the actual call that reaches the walker

`FUN_8008FE8C` (`0x8008FE8C`, frame -32) is a 24-instruction wrapper.
It computes a slot-relative VA from the descriptor's `+0x54` (slot
base) and `+0x80` (size), plus a per-process delta from kernel globals,
and calls `FUN_80097F44` twice with different `a1/a2` flag values.
It does NOT touch any s-register. So the walker's saved
`s0..s3 = (0xFFFFD800, 0x80FFFEA8, 0x80FFFEA8, 0xFFFFD800)` is just
FUN_80090144's saved set spilled by the walker, with `s1/s2 =
coredll_desc` because `FUN_80090144` set `s1` to its `a0` argument.

### 7. FUN_80097F44 vs the publisher at 0x80098484

`FUN_80097F44` (`0x80097F44`, frame -80) is the MM-locked range walker
the handoff already attributed correctly. It:

- takes the MM lock at `0x806697A0` via `FUN_800998C0`,
- derives a target module via the table lookup `lw a0, -10048(t9)`
  with `t9 = (v0 & 0x3F) << 2` (where `v0 = a0_arg >> 0x19`),
- calls `FUN_80096E50` (helper) at `0x80097FF0`,
- calls `FUN_80096E88` (range walker) twice (`0x80098054`, `0x80098100`),
- calls **`FUN_800970A8` (the unmap walker) twice** (`0x80098090`,
  `0x8009813C`),
- releases the lock via `FUN_80099924` and returns.

The function's body ends at `0x8009817C` (`jr ra; addiu sp, sp, 80`).

Phase AV's "publisher loop at `0x80098484` lives in this function" is
wrong. The publisher loop is in a **separate** function whose entry is
at `0x80098180` (frame -152, saves `s0..s8` and `ra`). That function
is the one that calls `FUN_80096E50` at `0x80098348` and
`FUN_80096E88` at `0x8009837C`, with the publish loop at
`0x80098484..0x800984D8`. Whatever it does is unrelated to the
walker that fires on cold boot — `FUN_80097F44` at `0x80097F44` is a
different routine entirely.

Phase AV's confusion came from disassembling without a function
boundary — both functions live in the same NK code page and decode
adjacently if you read straight through. The boundary is the `jr ra`
at `0x8009817C`, not anything Ghidra-derived.

## Updated Failure Chain

```text
1. NK boots, reaches first user process startup.
2. NK creates filesys.exe (slot 1) and starts FUN_800927CC to load
   coredll into slot-1.
3. The loader maps coredll's PE32 sections into slot-0 and slot-1
   mirror VAs. Both `0x01F84A5C` (slot-0 DllMain VA) and `0x800BFA5C`
   (kseg0 DllMain VA) are valid at this point per Phase AS reads.
4. The loader resolves `coredll[+0x60] = 0x01F84A5C` via FUN_8009096C.
5. At `0x800929C0..0x800929C8`, FUN_800927CC calls
   `FUN_8008FF00(coredll, 1, 0)` which dispatches via
   `(*coredll[+0x60])(coredll, 1, 0)` at `0x80090024`, executing
   coredll's DllMain at slot-0 `0x01F84A5C`.
6. DllMain runs. It calls coredll-internal helper `0x01F8F4D4` (a0=66)
   first, then either branches to success-return (BEQL path) or
   continues into the ATTACH publish path that writes the OAL
   callback table at `0x01FE6544..0x01FE65A4`.
7. **DllMain returns 0** for some reason that is not yet known.
8. FUN_8008FF00 propagates `v0 = 0`. FUN_800927CC sees `v0 == 0` at
   `0x800929D0`, falls through to the rollback `jal 0x800903BC` at
   `0x800929DC`, then sets `*(704(s3)+0x38) = 0x45A` at
   `0x800929E8`.
9. `FUN_800903BC -> FUN_80090144 -> FUN_8008FE8C -> FUN_80097F44 ->
   FUN_800970A8` zeros coredll's tail PTEs (text tail at rva
   0x62000-0x64000, .data at 0x66000, .pdata at 0x67000-0x6F000 —
   14 pages, 56 KB).
10. After the unmap, the next code that tries to dispatch through the
    OAL table at `0x01FE6544..0x01FE65A4` reads zero and faults with
    Exception 004 — exactly the "consumer at 0x01F8F4D4" that has
    been chasing since Phase A.
```

## What This Session Confirmed Or Refuted

Confirmed:

- The `WINCE_STACK_WALK` probe (added in some earlier phase) already
  contains the full caller chain. Future sessions should grep
  `WINCE_STACK_WALK` first before adding more probes.
- The walker `FUN_800970A8` is correct NK code. The trigger lives
  upstream.
- The whole upstream chain through the `FUN_800927CC` loader is real,
  per the saved-ra walk.
- Coredll's DllMain at slot-0 `0x01F84A5C` IS a real DllMain that
  publishes the OAL callback table. Phase AS's prologue reading was
  correct.
- The "consumer at `0x01F8F4D4`" is not stale — it is coredll's own
  internal helper called by DllMain at `0x800BFA7C`. The MIPS jal
  PC-region semantics fully explain why the same bytes call
  `0x81F8F4D4` from kseg0 and `0x01F8F4D4` from slot-0 without any
  relocation.

Refuted:

- Phase AV's "publisher and walker live in the same NK function" is
  false. `FUN_80097F44` ends at `0x8009817C`; the publisher at
  `0x80098484` lives in a separate function at `0x80098180`.
- Phase AX's "decode `s0 = 0xFFFFD800` to find the walker's bad input"
  target is a red herring. `0xFFFFD800` is NK kernel-globals base,
  loaded via `li s0, -10240` at `0x80090184` in `FUN_80090144`, and
  walker never reads it as a parameter.
- Phase AE's "DllMain didn't fail because the kernel error is 0" is
  false. The error 0x45A is set at `0x800929E8`, AFTER the rollback,
  so reading it at walker-entry time always shows 0.
- The original pre-Phase-AS hypothesis "DllMain returns 0 → loader
  rollback → walker zeros tail" is the correct chain. Phases
  AT/AU/AV/AW/AX walked away from it in error after Phase AS read
  the DllMain bytes and concluded "the bytes look fine, so the
  trigger must be elsewhere". The bytes ARE fine — what we still
  don't know is why DllMain returns 0 at runtime.

## Recommended Next Steps (Phase AZ)

The remaining single open question is: **why does coredll's DllMain
return 0 on emulator?** Three angles, in order of expected payoff:

1. **Add ONE runtime probe at FUN_8008FF00's jalr.** Either at
   `0x80090024` or at `0x8009003C` (whichever the dispatch actually
   reaches — both lead to the same `sw v0, 44(sp)`). The probe should
   capture:
   - cpu->pc, sp, a0/a1/a2 just before the jalr fires (one-shot, gated
     on a counter that fires the first time pc is in the jalr's basic
     block)
   - cpu->pc, sp, v0 just after the jalr returns
   This single probe authoritatively answers "what return value does
   coredll's DllMain produce, and was it actually entered with the
   right args".
2. **Statically trace coredll's DllMain to find every `move v0, ...`
   and `li v0, ...` within reach of the function epilogue.** The
   epilogue is somewhere after the case dispatcher at
   `0x800bfb60..0x800bfbf4`. Identify each return path and what
   condition leads to `v0 = 0`. Most likely candidates:
   - the BEQL path at `0x800bfa84` if the first helper does NOT
     return zero AND the rest of the ATTACH path falls through to a
     failure return,
   - one of the case branches at `0x800bfb60..0x800bfbdc` for
     fdwReason ≠ 1 returning 0,
   - a deeper helper (`0x81F84EC8`, `0x81F94F44`, etc.) that fails
     for reasons specific to emulator state.
3. **Compare coredll's DllMain runtime entry conditions against
   real-hardware ground truth.** The args at the time of dispatch are
   `a0 = coredll desc = 0x80FFFEA8`, `a1 = 1 = ATTACH`, `a2 = 0`. Of
   those, only `a0` could vary between hardware and emulator — and
   only if our descriptor's `+0x60` resolves to a different VA than
   real hardware does. Phase AS already verified `+0x60 = 0x01F84A5C`
   matches `e32_entryrva = 0x4A5C` plus `e32_vbase = 0x01F80000`, so
   this looks fine, but a runtime read of `desc[+0x60]` at the moment
   of the dispatch would lock it down.

Do NOT pursue any of these dead leads:

- the `0xFFFFD800` decoding (it's just NK globals base),
- the publisher loop at `0x80098484` (it's a different function and
  unrelated to the walker that fires on cold boot),
- the EXCEPTION directory walk (already dead per Phase AS),
- enumerating more `FUN_800927CC` failure paths beyond the three
  already known — Phase AY confirmed callsite `0x800929DC` IS the
  one that fires.

## Files And Commits

This session changed:

- `docs/WINCE_COLD_BOOT_SESSION_2026-04-14.md` (new file).
- No source files. No emulator builds. No new probes.

The reference run for everything in this report is
`build-host/repro_phase_at.stderr` from Phase AT. No new repro is
needed.

The unrelated worktree changes in `gxemul/` and
`src/be300_devices.c` from prior sessions are still present and
untouched.
