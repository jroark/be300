# Phase AD: coredll DllMain entry mismatch — the descriptor lies, or we read it wrong

Date: 2026-04-13

After Phase AC confirmed the failing module is `coredll.dll` via TOC
name lookup, Phase AD attempted to disassemble its DllMain entry to
find the FALSE-returning check. The result raised more questions
than it answered.

## Setup recap

From the runtime descriptor at `0x80FFFEA8`:
- `+0x54 = 0x03F80000` — slot-1 mirror base
- `+0x60 = 0x01F84A5C` — claimed DllMain entry
- `+0x64 = 0x80655CC8` — TOC entry pointer
- `+0x7C = 0x01F80000` — slot-0 base
- `+0x80 = 0x00080000` — size = 512 KB

User-VA `0x01F84A5C` corresponds to NK kseg0 `0x8033FA5C`
(coredll NK base = `0x8033B000` per TOC, offset `0x4A5C`).

## What Ghidra says

`FUN_8033F968` is auto-detected as a function. `0x8033FA5C` is **inside
its body** at offset `0xF4`, NOT at the entry. Ghidra reports xref:

```
0x80655CC8 (TOC entry):
 +0x10 = 0x80126FF0 → "coredll.dll"
 +0x1C = 0x8033B000 (load offset)

xrefs to 0x8033FA5C:
 From 0x8033FA44 in FUN_8033F968 [CONDITIONAL_JUMP]
```

So `0x8033FA5C` is a `bnez v0, +0x14` jump target inside an existing
function.

## What the bytes say

```
8033fa44: bnez v0, 0x8033fa5c    ; if v0 != 0 (success branch from prior call)
8033fa48: lw   t9, 0x40(sp)      ; (delay) load t9
8033fa4c: jal  0x81d67f98        ; free
8033fa50: lw   a0, 4(t9)         ; (delay)
8033fa54: b    0x8033fa64        ; failure tail
8033fa58: nop
8033fa5c: b    0x8033fa78        ; ★ success tail (target of the bnez)
8033fa60: li   v0, 1             ; (delay) v0 = 1
8033fa64: ...                    ; failure cleanup (free, free, v0=0)
8033fa78: lw   s0, 0x20(sp)      ; epilogue
8033fa7c: lw   ra, 0x24(sp)
8033fa80: jr   ra
8033fa84: addiu sp, sp, 0x40
```

So `0x8033FA5C` is the **success-return label** of `FUN_8033F968`: it
sets `v0 = 1` and falls into the epilogue. It restores `s0` and `ra`
from `sp+0x20` and `sp+0x24` of a 64-byte frame that the function's
own prologue allocated somewhere earlier.

It is NOT a function entry. Calling `0x01F84A5C` directly with a
fresh stack would skip the prologue, leave `s0`/`ra` saving slots
holding garbage, and the `lw ra, 0x24(sp); jr ra` would return to
random memory.

## Three possibilities

1. **`FUN_8033F968` IS coredll's DllMain** and `0x8033FA5C` is its
   success return (but then the descriptor `+0x60` should point at
   the function ENTRY `0x8033F968`, not the success label `0x8033FA5C`).

2. **`descriptor+0x60` is not actually the DllMain function pointer.**
   We inferred it was based on `FUN_800927CC`'s reading of
   `unaff_s2 + 0x60` as a function pointer it calls via
   `(**(code **)(param_1 + 0x60))(...)`. But maybe `unaff_s2` and
   `param_1` refer to different fields in different layouts.

3. **`FUN_8009096C` (the RVA-to-VA resolver) is computing a wrong VA
   because `descriptor+0xBC` (its section table arg) is wrong/stale.**
   We saw `+0xBC = 0x03FE6570` (a slot-1 mirror VA in the unmapped
   range). That value looks suspicious — slot-1 of the unloaded DLL.
   If the section table pointer was already reset to a stale slot-1
   address by the time we snapped the descriptor, the resolver would
   return garbage.

## What this changes

The Phase AC claim "coredll's DllMain returned FALSE → loader rolled
back" is **not confirmed**. We confirmed:
- The descriptor at `0x80FFFEA8` is `coredll.dll`'s loaded-instance
  record (TOC name = "coredll.dll")
- The walker is unloading this descriptor's PTE map
- The unload is triggered through `FUN_800903BC` →
  `FUN_80090144` → `FUN_8008FE8C` → `FUN_80097F44` → walker

We did NOT confirm:
- Whether `FUN_8008FF00(coredll, DLL_PROCESS_ATTACH, NULL)` was called
- Whether it returned 0
- Whether the trigger was the `0x45A` path or one of the other unload
  paths in `FUN_800927CC` (which also include error `0xE = 14 =
  ERROR_NOT_ENOUGH_MEMORY` from `FUN_800907C8` failure or
  `(*&SUB_ffff9bf6)` callback failure)

## Phase AE target

Add a runtime probe that reads the **kernel error code field** at
the moment the walker fires. The kernel state pointer is
`_DAT_FFFFDAC0` (per `FUN_800927CC` decomp) and the error is at
offset `0x38`. We can compute `_DAT_FFFFDAC0` either by reading PA
near `0x18C0` (where CLAUDE.md says `ctx_ptr` lives) or by walking
the kernel state table.

If error == `0x45A` → DllMain path (Phase AC's hypothesis).
If error == `0xE`  → `FUN_800907C8` or related "init alloc failed".
If error == `0x57` ('W') → `FUN_80097844`/`FUN_80096CC8` error path.
If error == 0 → unload was clean / other path.

Once we know the error code, we know which of the four unload paths
in `FUN_800927CC` (or the additional path in `FUN_800929CC`) fired.
That narrows the upstream cause from "DllMain failed" to a specific
loader sub-step.

## Stable claims

- The proximate root cause is "WinCE loader unloads coredll.dll
  during early cold boot via the FreeLibrary chain, which tears
  down its PTEs". This is firmly established.
- The 30-phase chase has been tracking the legitimate loader-
  rollback path. The teardown is not an emulator-side store-
  ordering bug; it is correct guest code.
- The actual fix has to address WHY the loader decides to unload
  coredll. Whatever the upstream check is, the emulator is
  producing a state that fails it.

## Forward direction

Phase AE = read kernel error code at walker entry → branch to the
right loader sub-path → fix the upstream emulator behavior that
makes that check fail.

This is finally a point where a static analysis can target a
specific guest decision. The runtime instrumentation has done its
job — the rest is reverse-engineering the loader's check and
matching it against emulator behavior.
