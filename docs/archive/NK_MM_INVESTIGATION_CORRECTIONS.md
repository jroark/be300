# Phase W: NK MM Investigation — Corrections from Static Disasm

Date: 2026-04-13

After 24 phases of runtime probes, I used offline `mipsel-linux-gnu-objdump`
on `build-host/nk_decompressed.bin` to verify the Phase R claim that
`0x80096E88` is a "worker that calls 0x800A1134 allocator and bails on
v0=0". **That claim does not hold up to static disassembly.** This
document captures the corrections so later phases don't re-chase dead
leads.

## What `0x80096E88` actually is

Full static disasm of `0x80096E88..0x80096FC8`:

```
80096e88:       addiu   a1, sp, 72
80096e8c:       addiu   a2, sp, 60
80096e90:       jal     0x800a20dc
80096e94:       addiu   a3, sp, 64          (delay)
80096e98:       lw      t5, 60(sp)           ; reload *(a2)
80096e9c:       li      at, 4
80096ea0:       bne     t5, at, +0x44         ; branch if *a2 != 4
80096ea4:       lw      t6, 104(sp)
80096ea8:       beqz    t6, +0x24
...
80096ee8:       jal     0x800a2020            ; free *sp+96
80096eec:       lw      a0, 96(sp)           (delay)
80096ef0:       jal     0x800a2020            ; free *sp+100
80096ef4:       lw      a0, 100(sp)          (delay)
80096ef8:       lw      t7, 88(sp)
80096efc:       bnezl   t7, 0x80096fbc        ; if t7!=0 → v0=1 return
80096f00:       li      v0, 1                 (delay of bnezl)
80096f04:       lw      t8, 23368(zero)      ; load kernel flag
80096f08:       lui     a0, 0x8000
80096f0c:       sll     t9, t8, 0xb
80096f10:       bgez    t9, 0x80096fbc        ; if bit 20 not set → ret 1
80096f14:       ori     a0, a0, 0x2
...
80096f2c:       jal     0x8007d230            ; trace/log call
80096f34:       bnez    v0, 0x80096fbc        ; if returned nonzero, ret 1
...
80096f5c:       jal     0x8007d15c            ; another trace/log
80096f64:       bnez    v0, 0x80096fb4
...
80096fb4:       jal     0x8007d1ec            ; cleanup/unlock
80096fb8:       lw      a0, 56(sp)           (delay)
80096fbc:       li      v0, 1                 ★ SUCCESS RETURN
80096fc0:       lw      ra, 28(sp)
80096fc4:       jr      ra
80096fc8:       addiu   sp, sp, 104           (delay - frame cleanup)
```

### Key corrections

1. **The function has NO path that returns `v0=0`.** Every reachable
   exit writes `li v0, 1` or `bnezl ... li v0, 1`. So `v0` at the
   caller cannot come from this function being our "failing worker".

2. **The function does NOT call `0x800A1134` anywhere.** My Phase R
   reading claimed `0x80096F0C: jal 0x800a1134` with `li a0, 5`. The
   actual word at `0x80096F0C` is `0x0018cac0 = sll t9, t8, 0xb`.
   `0x800A1134` is called from an entirely different call site we
   have not identified.

3. **`0x80096E88` does not start with a prologue.** Its first instruction
   is `addiu a1, sp, 72`, not `addiu sp, sp, -N`. The 104-byte frame
   cleanup at `0x80096FC8` implies the prologue lives elsewhere, so
   either (a) `0x80096E88` is a mid-function alternate entry that a
   preceding prologue jumps into after stack setup, or (b) the dyntrans
   translation-time probes I used in Phase O/Q caught a different
   function overlay at that PC.

4. **Static disasm confirms the function only calls:**
   `0x800A20DC`, `0x800A2020` (twice), `0x8007D230`, `0x8007D15C`,
   `0x8007D1EC`. None of these is `0x800A1134`.

## What this means for the walker-entry `v0=0`

At walker entry (`pc=0x800970A8`), Phase Q captured `v0=0, a3=1`. The
Phase H disasm of `0x80098100..0x8009813C` showed no instruction
between `jal 0x80096E88` and `jal 0x800970A8` that writes `v0`. So our
model was "v0 is preserved from 0x80096E88's return through to walker
entry".

Given static disasm shows `0x80096E88` never returns 0, that model
is wrong. Two remaining candidates for where `v0=0` comes from:

1. **Exception-handler context.** Phase D captured the walker running
   with `Status=0x00008401` (EXL=1), so the walker is executing inside
   an exception context. The `v0=0` can come from the exception
   handler's restored register state, not from the worker's return.
   This is the strongest candidate.

2. **0x80096E88 is entered via a path that skips past the `jr ra` at
   0x80096FC4.** If there is a branch target inside the function that
   a prior caller jumps to, it could return through a different exit.
   But no such exit writing `v0=0` exists in the range we dumped.

## What this invalidates

Phases I, L, M, N, O, P, Q, R, S, T, U, V all built on the "worker
at 0x80096E88 calls 0x800A1134 allocator and bails with v0=0 on
pool-5 failure" chain. The structural parts of those phases still
hold:

- Walker `0x800970A8..0x800971D0` loop form is correct (decoded from
  live memory in Phase F).
- Pool-5 descriptor layout and size 0x4C is correct (decoded from
  live memory in Phase T).
- L1 table at `0x80668CC0` and pool-5 object array at
  `0x80FFC000..0x80FFC1C8+` are correct (decoded in Phase U/V).
- Opcode jump table at `0x80075714` is correct (Phase O).

What's wrong is the *causal chain*: the "verify-then-free" and the
"v0 gates the walker" narratives do not follow from static disasm.

## Next investigation direction

Instead of more runtime probes that all hit the dyntrans
translation-time cache wall, the right move is:

1. **Static call graph from `0x800A1134` backwards** — find who
   actually calls the allocator, and classify whether it's part of the
   teardown path or an unrelated subsystem.

2. **Static call graph to `0x800970A8`** (the walker) — find every
   caller and classify each. Phase H/L captured one caller at
   `0x8009813C` inside a function we've been calling the "coordinator".
   A second caller might exist and be the real guilty path.

3. **Exception vector + dispatch** — find the kernel exception handler
   region and see whether `v0=0` gets clobbered in a save/restore path
   along the route to the walker.

4. **Do NOT add more runtime probes at PCs inside the walker call
   chain.** Every probe we added since Phase P has hit the cached-
   translation limit and captured a misleading snapshot.

## Stable facts after 24 phases

- Cold boot times out with a callback consumer at `0x01F8F4D4` reading
  `flag=0 ptr=0 aux=0 arg=0x80FFFEA8` from VA `0x00FFB544` (consumer
  watch slot).
- The "teardown" that zeros `PA 0x00FFC1C8..+0x40` is the real store at
  `0x800971BC: sw $zero, 0x0C($s0)` in a 16-iteration loop.
- The loop zeros PTE-slot words inside a pool-5 object used as an L2
  PTE group.
- The enclosing function `0x800970A8` is called from inside a
  coordinator function that sits around `0x80097FC0..0x80098200`.
- That coordinator runs during cold boot, not at shutdown.
- The coordinator's `a2=0x1F8` argument *does* trigger the free walk,
  but the mechanism is not yet correctly modelled.

Everything else should be treated as provisional.
