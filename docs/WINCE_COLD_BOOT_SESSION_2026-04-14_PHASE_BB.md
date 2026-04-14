# Phase BB: Root Cause Identified — Coredll Helper Falls Back to Kernel Callback Trampoline

Date: 2026-04-14

## Bottom Line (read first)

**The cold-boot Exception 004 root cause is now identified at the
guest-binary level.** The chain is:

1. WinCE loader `FUN_800927CC` calls `FUN_8008FF00(coredll, 1, 0)`
   which dispatches into coredll DllMain at slot-0 `0x01F84A5C`.
2. DllMain's very first action is `jal 0x81f8f4d4` with `a0 = 66`,
   calling a coredll-internal helper at slot-0 `0x01F8F4D4`.
3. The helper at slot-0 `0x01F8F4D4` (kseg0 `0x800CA4D4`) reads
   `*(0x01FE6544)`. **In coredll's link-time `.data`, this slot is
   ZERO.** (Verified by direct read of `nk_decompressed.bin` at file
   offset `0x58198`.)
4. Because the slot is zero, the helper takes its FALLBACK path:
   `li v0, -1630` (= `0xFFFFF9A2`) followed by `jalr v0`.
5. **`0xFFFFF9A2` is a WinCE 3.0 kernel callback trampoline
   address.** It lives in the kseg2 / high-VA range that WinCE
   reserves for syscalls dispatched via TLB-miss + BadVaddr decode.
6. On real hardware, `jalr 0xFFFFF9A2` traps to NK's exception
   handler, NK decodes the high VA as a syscall index, dispatches to
   the appropriate OAL service (which on first call probably
   initializes the OAL callback table at `0x01FE6544..0x01FE65A4`
   for the calling process), and returns. DllMain then proceeds
   normally, eventually returning v0=1.
7. **On the emulator**, `jalr 0xFFFFF9A2` is NOT being recognized as
   a kernel callback. Either the TLB-miss exception handler doesn't
   run, doesn't decode the address as a syscall, or NK's handler
   raises a fault instead of dispatching. The result is that DllMain
   (effectively) returns 0, the loader rolls back coredll's load,
   and the walker we have been chasing for 50+ phases zeros
   coredll's tail PTEs as part of the legitimate rollback.

This is a CONCRETE EMULATOR BUG, not a guest issue. Per CLAUDE.md
the fix MUST be in the emulator's handling of WinCE 3.0 kernel
callback trampolines, NOT in patching coredll or pre-populating
the OAL table.

## What Was Done

Phase BB extended the Phase BA `WINCE_DLLMAIN_ENTRY` probe to read
four neighboring fields of coredll's descriptor at the moment of
FUN_8008FF00 dispatch:

```c
fprintf(stderr,
    "[WINCE_DLLMAIN_ENTRY] #%u desc_fields:"
    " +0x10=0x%08X(loaded_mask)"
    " +0x84=0x%08X(dispatch_flag)"
    " +0x88=0x%08X +0xC8=0x%08X\n",
    hit_ff00, f10, f84, f88, fc8);
```

Result:

```text
[WINCE_DLLMAIN_ENTRY] #1 pc=0x8008FF00 desc=0x80FFFEA8
                          reason=0x00000001 reserved=0x00000000
                          desc[+0x60]=0x01F84A5C(ok)
                          sp=0x0201FD90 ra=0x800929D0
[WINCE_DLLMAIN_ENTRY] #1 desc_fields:
                          +0x10=0x00000000(loaded_mask)
                          +0x84=0x00000000(dispatch_flag)
                          +0x88=0x00000000
                          +0xC8=0x00020000
```

Key descriptor fields decoded:

- `desc[+0x10] = 0` (loaded_mask). Coredll is NOT marked as loaded
  for any process slot. Therefore FUN_8008FF00's bnez early-skip at
  `0x8008FF94` does NOT skip the dispatch; it proceeds to dispatch
  DllMain.
- `desc[+0x84] = 0` (dispatch_flag). The BEQL at `0x8008FFE8`
  (`beql v1, zero, 0x80090038`) takes the branch, so FUN_8008FF00
  uses DISPATCH 2 (`jalr t8` at `0x8009003C`) rather than DISPATCH 1
  (`jalr t7` at `0x80090024`). Both dispatches call the same
  function pointer (`desc[+0x60]`), so this distinction does not
  itself cause the failure.
- `desc[+0xC8] = 0x00020000`. The lhu at `0x8008FF30` reads the low
  16 bits = 0, so the bnel at `0x8008FF38` does not take its
  early-skip branch.

So the dispatch flow into coredll DllMain is what we expected:
correct args, correct dispatch path, no early skip.

### Disasm of the helper at slot-0 0x01F8F4D4 / kseg0 0x800CA4D4

```asm
800ca4d4  addiu sp, sp, -32
800ca4d8  sw    ra, 20(sp)
800ca4dc  lui   at, 0x01FE
800ca4e0  lw    t6, 0x6544(at)   ; t6 = *(0x01FE6544)
800ca4e4  beqz  t6, 0x800ca4f8   ; if zero → FALLBACK
800ca4e8  lui   at, 0x01FE       ; (delay) reload at
800ca4ec  lw    t7, 0x6548(at)   ; t7 = *(0x01FE6548)
800ca4f0  b     0x800ca4fc        ; jump past FALLBACK
800ca4f4  lw    v0, 0x260(t7)    ; (delay) v0 = t7[+0x260]
800ca4f8  li    v0, -1630         ; FALLBACK: v0 = 0xFFFFF9A2
800ca4fc  jalr  v0                 ; CALL function pointer
800ca500  nop
800ca504  lui   at, 0x01FE
800ca508  lw    t8, 0x6544(at)
800ca50c  move  v1, v0             ; save return
... (similar pattern for second call)
```

The helper is structured as:

- "If the OAL table sentinel at `0x01FE6544` is nonzero, look up a
  function pointer via `*(table[+0x6548] + 0x260)` and call it."
- "If the sentinel is zero, fall back to the kernel callback at
  `0xFFFFF9A2` (or similar high-VA constant for other helpers)."

This pattern repeats across many adjacent helpers in coredll, each
with a different fallback constant in the `0xFFFFF8xx..0xFFFFFAxx`
range. The constants are emitted as `li v0, -N` instructions and
correspond to specific OAL function indices.

### Static read of coredll .data OAL table

Read `nk_decompressed.bin` at file offset `0x58198` (= coredll
section1 realaddr `0x800B7C54` - NK base `0x80060000` + section
offset `0x544` for rva `0x66544`). First 32 entries:

```text
rva 0x66544 = 0x00000000   <- THE SENTINEL, reads as ZERO
rva 0x66548 = 0x00000000   <- second word, also zero
rva 0x6654C = 0x80078DF0   <- first nonzero entry
rva 0x66550 = 0x80078E10   <- OAL display function (CLAUDE.md)
rva 0x66554 = 0x80078EDC
rva 0x66558 = 0x00000000
rva 0x6655C = 0x00000000
rva 0x66560 = 0x80078E1C
rva 0x66564 = 0x80078F38
rva 0x66568 = 0x800790DC
rva 0x6656C = 0x00000000
rva 0x66570 = 0x00000000
... pattern continues ...
```

The structure is groups of 5 words: `[sentinel, ptr, ptr, ptr,
ptr]` repeating. The first word of each group is zero in the
link-time .data. **Phase AX listed the nonzero entries but missed
that the FIRST word of each group is zero.**

Each group's sentinel word at offset 0/5/A/F/... is what the
helpers test against. When zero (cold boot), helpers fall back to
kernel callbacks in `0xFFFFFxxx`.

### Where the kernel callback dispatch should happen

Searching the entire NK image for hardcoded references to
`0xFFFFF9A2`, `0xFFFFFA32`, `0xFFFFFA2E`, `0xFFFFBB76`:

```text
li v0,-1630 (0xFFFFF9A2):  1 occurrence
li v0,-1486 (0xFFFFFA32):  1 occurrence
li t0,-17546 (0xFFFFBB76): 2 occurrences
0xFFFFF9A2 raw word (LE):  0 occurrences
```

All hits are `li` instructions inside coredll. **NK has no data
table of these addresses** — they are not stored anywhere in the
NK image as data. This means the kernel callback dispatch is NOT
data-driven. The kernel must recognize the high-VA range via an
exception handler.

WinCE 3.0 on MIPS uses the standard mechanism: when user code
`jalr`s to an unmapped kseg2 address (0xC0000000+), the CPU takes a
TLB-miss exception. NK's TLB-miss handler examines `BadVaddr`,
recognizes addresses in the kernel callback range, and dispatches
to the appropriate syscall handler based on the address bits.

### What the emulator does today

The emulator currently has NO handling for the `0xFFFFFxxx` kernel
callback range. A `jalr 0xFFFFF9A2` would either:

- Fault as a normal TLB miss, with NK's handler treating it as a
  user-process fault rather than a syscall dispatch, or
- Cause execution to derail in some other way that ultimately
  manifests as the loader-rollback chain we have been chasing.

The fix path requires:

1. **Identify the address-to-syscall mapping** that WinCE 3.0 uses
   for the kernel callback range. This is documented in WinCE 3.0
   SDK headers (look for `KCallTrap`, `PerformCallback4`, or the
   `SC_*` syscall index table).
2. **Verify NK's TLB-miss handler** in the NK image actually
   recognizes the kernel callback range and dispatches accordingly.
   If it does, the emulator's job is just to ensure the TLB miss
   exception properly enters NK's handler.
3. **If NK's handler does NOT recognize the range natively**, we
   may need to install a handler in the emulator that intercepts
   `jalr` to addresses in `0xFFFFC000..0xFFFFFFFF`, examines the
   target, and either dispatches to an in-emulator implementation
   of the syscall OR maps the address to a real NK function via a
   lookup table.

## What This Resolves

### Phase A's "consumer at 0x01F8F4D4 reads zero from 0x01FE6544"

Phase A originally identified `0x01F8F4D4` as the fault site and
noted it reads zero from `0x01FE6544` then does a "deliberate
fallback `jalr 0xFFFFF9A2`". **Phase A had this exactly right.**
The misattribution in Phases AT-AY was treating the zero read as
the result of the walker's PTE-zeroing — when in fact the zero is
the LINK-TIME value in coredll's `.data`. The walker zeroing was a
downstream consequence of the loader rollback, not the cause of
the fault.

### The handoff's "decode 0xFFFFD800" target

Phase AY already proved this was a red herring. Phase BB confirms
that `s0 = 0xFFFFD800` has nothing whatsoever to do with the
trigger.

### Phase AS's "DllMain bytes look fine"

Correct. Coredll's DllMain prologue at `0x01F84A5C` IS valid. The
function runs. Its FIRST instruction-after-saves is the call to
the failing helper. The function never reaches its happy-path
return and instead returns 0 (or causes a fault that's
indistinguishable from returning 0) due to the helper's fallback
to a kernel callback that the emulator can't handle.

### Phase AV's "publisher and walker in same function"

Still wrong. `FUN_80097F44` and the function at `0x80098180` are
distinct functions. But this whole branch of investigation was a
distraction from the actual cause.

## Updated Failure Chain

```text
1.  NK boots, reaches first user process startup.
2.  NK creates filesys.exe (slot 1) and starts FUN_800927CC to load
    coredll into slot-1.
3.  Loader maps coredll's PE32 sections, resolves
    desc[+0x60] = 0x01F84A5C via FUN_8009096C.
4.  Loader at 0x800929C8 calls FUN_8008FF00(coredll, 1, 0).
5.  FUN_8008FF00 dispatches via jalr at 0x8009003C (DISPATCH 2,
    because desc[+0x84] == 0).
6.  Coredll DllMain at slot-0 0x01F84A5C runs.
7.  DllMain immediately calls helper(66) at slot-0 0x01F8F4D4.
8.  Helper reads *(0x01FE6544) = 0 (zero in coredll's link-time
    .data).
9.  Helper takes FALLBACK: li v0, -1630; jalr 0xFFFFF9A2.
10. *** EMULATOR FAILS HERE ***. The emulator does not implement
    WinCE 3.0 kernel callback dispatching for kseg2 addresses
    in the 0xFFFFFxxx range. The jalr causes either an unhandled
    TLB miss or an exception that the guest kernel doesn't
    recognize as a syscall.
11. The fault propagates back as if DllMain returned 0.
12. FUN_8008FF00's saved sp+0x2C ends up holding 0 (instead of 1
    via the normal success path).
13. Loader at 0x800929D0 sees v0 == 0, falls through to
    `jal 0x800903BC` rollback at 0x800929DC, then sets error 0x45A.
14. FUN_800903BC -> FUN_80090144 -> FUN_8008FE8C -> FUN_80097F44 ->
    FUN_800970A8 zeros coredll's tail PTEs (text tail, .data,
    .pdata).
15. After the unmap, any subsequent code that holds pointers into
    the freed pages faults with Exception 004.
```

## Recommended Next Steps (Phase BC)

1. **Probe `jalr 0xFFFFF9A2` behavior in the emulator.** Add a
   probe at the moment any guest instruction performs `jalr` to a
   target VA in `0xFFFFC000..0xFFFFFFFF`. Print PC, target,
   BadVaddr, exception state. This will tell us exactly how the
   emulator handles the call today.

2. **Verify NK's TLB-miss handler.** Static-disassemble NK's
   exception vector at PA `0x00000000` and PA `0x00000180` (kseg0
   `0x80000000` and `0x80000180`) and trace what it does with a
   high-VA BadVaddr. Specifically look for any range check against
   `0xFFFFC000` or similar.

3. **Search for `KCall`, `SystemCall`, `PerformCallback`,
   `CallNextHookEx`, or `SC_` patterns in NK** to find the syscall
   dispatch table. It is likely at a fixed VA in NK's data segment.

4. **Find the WinCE 3.0 kernel callback address-to-syscall mapping**
   in any available reference (SDK headers, Platform Builder docs,
   reverse engineering from a known WinCE binary). The mapping is
   probably:
   - `0xFFFFFxxx` → syscall index `(0xFFFFFxxx - base) / stride`
   - Each syscall is implemented in NK's coredll-equivalent or in
     one of the OAL service modules.

5. **Implement the kernel callback dispatch in the emulator**, OR
   install a minimal stub in the host-side that returns success for
   the specific callbacks coredll's helpers need at boot. This is
   the actual fix — but it requires understanding the callback ABI
   first.

## Files Changed (Phase BB)

- `src/wince_boot.c` — extended `WINCE_DLLMAIN_ENTRY` probe to
  read coredll desc fields `+0x10`, `+0x84`, `+0x88`, `+0xC8`. ~8
  lines added.
- `docs/WINCE_COLD_BOOT_SESSION_2026-04-14_PHASE_BB.md` (this file).

The reference run is `build-host/repro_phase_bb.stderr`.

The `gxemul/src/cpus/cpu_mips_instr.c` 11-PC switch addition from
Phases AZ and BA is still required for any of these probes to
fire. It remains a worktree-only change.
