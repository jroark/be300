# NK MM Phase X: walker call path and Phase D correction

Date: 2026-04-13

Continues `docs/NK_MM_GROUND_TRUTH_2026-04-13.md`. Same Ghidra session,
deeper analysis of the walker's caller chain.

## The walker is a normal function

`FUN_800970A8` body (from Ghidra disasm) is a standard kernel VM
function with a full prologue/epilogue:

```
800970a8: addiu sp, sp, -0x40
800970ac: sw s3, 0x24(sp)
800970b0: sw s2, 0x20(sp)
800970b4: sw s1, 0x1c(sp)
800970b8: sw s0, 0x18(sp)
800970bc: or  s0, a0, zero        ; s0 = L1 base
800970c0: or  s1, a3, zero        ; s1 = count
800970c4: or  s2, a2, zero        ; s2 = offset_in_group
800970c8: or  s3, a1, zero        ; s3 = target_idx
800970cc: sw  ra, 0x3c(sp)
...
800970e4: jal 0x8007ea74           ; TLB/cache sync
800970e8: _li a0, 0x1              ; delay: level 1
...
(main walker body through 0x800971f8)
...
80097200: jal 0x8008c564           ; TLB flush
80097204: _nop
...                                ; epilogue
80097230: jr ra
80097234: _addiu sp, sp, 0x40
```

The `sw zero, 0xc(s0)` at `0x800971BC` and the `jal 0x800A3244` at
`0x800971B8` are *exactly* what we saw in Phase F via `dump_code_window`
on live memory. Ground truth confirmed.

## Phase D's `EXL=1` claim was wrong

Phase D captured `Status=0x00008401` at walker-entry time and I
misread it as "EXL=1, exception context". Recomputing:

```
0x00008401 = 0x8000 | 0x0400 | 0x0001
           = bit 15 (IM7) | bit 10 (IM2) | bit 0 (IE)
```

- Bit 0  = IE  (Interrupt Enable) = 1
- Bit 1  = EXL                    = **0** ← not in exception
- Bit 2  = ERL                    = 0
- Bits 3..4 = KSU                 = 0 (kernel)
- Bits 8..15 = IM0..IM7           = IM2, IM7 set
- Bit 22 = BEV                    = **0** ← using NK vectors, not ROM

So the walker runs in **normal kernel mode with interrupts enabled**,
not in an exception or BEV context. The "exception handler origin"
hypothesis from Phase D/W is discarded.

## Walker callers — indirect dispatch through FUN_80097040 family

Ghidra reports `FUN_800970A8` has only DATA xrefs from
`0x800B9FA8` / `0x800B9FB8` (and `FUN_80097238` has xrefs from
`0x800B9FBC` / `0x800B9FCC`). These are **function-pointer table
entries**, 16 bytes apart — a dispatch table with multiple fields
per entry.

**`FUN_80097040` (sibling / alternate entry shared with FUN_80096E88
and FUN_800970A8's cluster) has 6 direct callers**, all in the
high-level VM module:

```
From 801d25a4 in FUN_801d24d8
From 801d2944 in FUN_801d28c8
From 801d2a68 in FUN_801d2a24
From 801d2fb8 in FUN_801d2ca0
From 801d33c0 in FUN_801d3170
From 801d39e0 in FUN_801d3420
```

## High-level VM dispatcher uses computed jumps

`FUN_801D3170` and `FUN_801D3420` both dispatch operations via
computed jumps into function-pointer tables:

```c
// FUN_801D3170 at 0x801D3240:
(**(code **)(uVar2 * 4 + 0x154f0))();  // opcode table at offset 0x154F0

// FUN_801D3420 at 0x801D34D0:
(**(code **)((param_2 - 0x21) * 4 + 0x15554))();  // table at offset 0x15554
```

These offsets (`0x154F0` and `0x15554`) are relative to a base that
Ghidra couldn't statically resolve ("WARNING: Could not emulate
address calculation"). The real table addresses are likely
`0x800751F0`/`0x80075554` or similar — the rdata section has
function-pointer tables for VM operation dispatch.

## Real call chain (as far as static analysis goes)

```
<something high-level>            ; e.g. VirtualAlloc-equivalent
    │
    ▼
FUN_801D3170(ctx, opcode)          ; process-op dispatcher
    │
    ├── opcode < 0x20 → (*table[opcode])()
    │       │
    │       ▼
    │   indirect jal to one of the handlers in
    │   0x80097040..0x80097484 family, which may
    │   reach FUN_800970A8 via further computed jumps
    │
    └── opcode >= 0x20 → FUN_80097244 then FUN_80097040
                                              │
                                              ▼
                                      L1 walker (install mode)
```

## Stable facts from Phase A..V that survive

- The walker body at `0x800970A8..0x80097237` matches runtime
  observation exactly.
- `sw zero, 0xc(s0)` at `0x800971BC` is the PTE zero store.
- `jal 0x800A3244` at `0x800971B8` frees the PFN.
- `s4 = 0x3FFFFFC0` PFN mask is loaded at `0x800970F8`.
- `s7 = 1` (reserved marker), `s3 = -0x40` (PTE sentinel), `s8 = 1`
  are all set before the main loop.
- Pool-5 object layout (76 bytes, with 16 PTEs at offset 0x0C..0x48
  and header flags at +0x00..+0x0B) is correct as the L2 group struct.
- The L1 table at `0x80668CC0` indexed by `(va >> 14) & 0x7FC` is a
  valid L1 page directory for process address space.
- `FUN_800998C0` is the recursive lock acquire, `FUN_80099924` the
  release, operating on `0x806697A0`.

## Facts from Phase H..V that are provably WRONG

- "jal 0x80096E88 at 0x80098100" (Phase H) — `0x80098100` is not in
  any function per Ghidra. The bytes were misinterpreted as code.
- "Coordinator body at 0x80097F80..0x80098200" (Phases L..V) — also
  not a function. Ghidra reports "No function found" for every
  address I probed in that range.
- "FUN_80096E88 bails with v0=0 on pool-5 failure" (Phase R) — the
  function always returns 1 on reachable paths.
- "Guard beq v0, a3 determines walker invocation" (Phase Q) — no such
  guard exists as code; I was reading data.
- "EXL=1 exception-handler origin" (Phase D/W) — Status=0x00008401 has
  EXL=0. Misread the status register decoding.

## Investigation status

**We still do not know which opcode value causes the high-level VM
dispatcher to reach `FUN_800970A8`, nor the operation context in
which this happens during cold boot.**

The unresolved question is which `opcode - 0x21` value the dispatcher
at `FUN_801D3420:0x801D34D0` evaluates to before reaching the walker.
Static analysis can't fully answer this because:
1. The table base `0x15554` is computed relative to an unresolved
   register value.
2. `FUN_801D3170` and `FUN_801D3420` themselves have no direct code
   xrefs in Ghidra — they're reached via yet another layer of
   indirection.

## Forward direction that would actually work

Instead of tracing more xrefs in Ghidra, **insert one instruction-
level breakpoint in gxemul** at `0x800970A8` using the existing
`single_step_breakpoint` mechanism. When it fires, print the *real*
call chain from the CPU's stack frame (ra, saved-ra slots on stack)
unmodified by any dyntrans cache effect. That gives us the true
caller PC without depending on PC-translation-time hooks.

This is the first thing Phase Y should do. Until then, we're stuck
reading bytes.

## Meta

This session (Phases H through X) spent 27 runtime probes and one
static-disasm pass on a call-chain reconstruction that turned out
to be based on reading data as code. The decisive corrective was
**running Ghidra once**. The lesson for the next cold-boot bug:
start with Ghidra, not runtime traces.
