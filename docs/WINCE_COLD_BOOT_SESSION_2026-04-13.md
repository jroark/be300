# WinCE NAND Cold-Boot Session Report

Date: 2026-04-13

## Goal

Continue the BE-300 WinCE 3.0 cold-boot work from a true power-on reset,
using the real ROM, the real SPL, and the real `All_nand_300.bin` image,
with no seeded resume state, no guest patching, and no forced handoff
shortcuts.

This report is a continuation of `docs/WINCE_COLD_BOOT_SESSION_2026-04-12.md`.
That report ended with the hypothesis that the cold boot was stuck on a
missing callback-slot header publish at `0x01FE6544` / `0x01FE6548`. This
report covers what happens when that hypothesis is followed all the way
to the bottom: the "callback header" turns out not to be a callback header
at all, and the failure chain is a legitimate WinCE loader rollback of
`coredll.dll` for filesys.exe's slot.

## Current Status

The Exception 004 cold-boot deadlock now has a fully attributed mechanism
chain end-to-end. What is NOT yet pinned is the single upstream input that
makes WinCE's loader believe coredll's `DllMain` returned 0.

What is now firmly understood:

- the PTE zero stream we have been chasing is `FUN_800970A8`, the WinCE
  VA-range UNMAP primitive, doing legitimate per-process unmap of
  `coredll.dll` for slot 1 (filesys.exe),
- the "callback header at 0x01FE6544" is not a callback header. It is
  coredll's data section. The "consumer at 0x01F8F4D4" is a slot-0 user VA
  inside coredll. Both stay valid until the loader rollback unmaps coredll
  out from under them,
- the unload chain is `FUN_800927CC -> FUN_800903BC -> FUN_80090144 ->
  FUN_8008FE8C -> FUN_80097F44 -> FUN_800970A8`,
- the trigger inside `FUN_800927CC` is one of three failure checks. Two
  of those are now ruled out by direct runtime read; the third is "DllMain
  returned 0".

What is NOT yet understood:

- coredll's `e32_entryrva` is `0x4A5C`, which resolves to mid-function
  code inside `FUN_8033F968` (a 4-arg internal helper, not a DllMain),
- our static disasm cannot match this with a working calling convention,
- so either Ghidra's auto-detected function boundary is wrong, or coredll
  has an unusual calling convention for DllMain that we do not yet
  recognize.

## Short Version

If you only need the current handoff in one page:

1. The "callback consumer at `0x01F8F4D4` reads zero" symptom from the
   2026-04-12 report is correct, but the cause is not a missing publish.
   It is a legitimate per-process unmap of coredll that runs after the
   loader decides DllMain failed.
2. Phase B's `callback_wrapper_entry` PC case at `0x01F84A5C` is not a
   callback wrapper. It is whatever coredll's PE32 declares as its
   `e32_entryrva`. The bytes there are mid-function code in our static
   disasm.
3. The descriptor at kseg0 `0x80FFFEA8` is coredll's loaded-instance
   record. Slot 1 is the only process referencing it.
4. The consumer's "fallback `jalr 0xFFFFF9A2`" is the artefact of jumping
   to mid-function code that reads garbage from the caller's stack frame
   as if it were a saved-reg area, then `jr ra`s to whatever happens to be
   at that stack offset.
5. The next pass should answer: why does coredll's `e32_entryrva` point
   to mid-function code, and what does WinCE's loader actually call as
   coredll's DllMain on real hardware.

## Key Commits And What They Changed

### Main repo

The investigation produced 50 numbered "phase" commits (B through AR)
on `main`. Most of them are runtime probes added to and removed from
`src/wince_boot.c` plus a few one-shot disasm probes via objdump in the
`mips-dev` Docker container. The key commits:

- `7a68febb` - Phase B (the original Phase 1 probe from the 2026-04-12
  investigation - per-word callback slot publish trace).
- `bab501c3` - Phase A real-faulter capture and consumer rearm probe.
  This is when we learned `cpu->pc` and `EPC` are both stale in dyntrans
  and we cannot trust them blindly.
- `de17a164` - Phase D `HOT_L2W` per-store register snapshot. This was
  the first time we got a full register frame at the moment of each PTE
  zero store.
- `21a01b46` - Phase Y, the **objdump-base correction**. Every objdump in
  Phases F through V was using `--adjust-vma=0x80076b50` (the NK *entry*),
  but NK is loaded at `0x80060000` (the NK *base*). This was off by
  `0x16B50` bytes for 24 phases. Correcting the base re-validated Phase H's
  original interpretation that had been "discarded" between then and Phase
  W as bogus.
- `9746f1c2` - Phase Z+, walked one frame above the walker via the saved
  ra at `sp+0x3C` and identified the real caller chain through
  `FUN_8008FE8C` and `FUN_80090144`.
- `c933cc88` - Phase AA, traced the rollback chain up through
  `FUN_800903BC` to `FUN_800927CC`. First identification of the loader
  path.
- `9b7defe1` - Phase AC, confirmed via TOC name lookup that the module
  being unloaded is `coredll.dll`.
- `7314aed0` - Phase AM, proved that slot-0 and slot-1 share one L2 group
  via `(va >> 14) & 0x7FC`. There is no slot-0 / slot-1 mirror encoding
  bug in the emulator. Phase AL's "slot-0 unmapped" reading was just a
  TLB cache state artefact from `load_va_word(NO_EXCEPTIONS)`.
- `361c94fa` - Phase AN, ruled out candidate #2 (`SUB_ffff9bf6` kernel
  hook) by reading the gate variable `_DAT_80668C80 == 0`.
- `8e837b3c` - Phase AP, validated the e32_lite layout decoding by
  reading nk.exe's e32 and matching its computed entry to the documented
  `0x80076B50` from CLAUDE.md.
- `c55a67e6` - Phase AR (current head), tried to walk coredll's PE32
  EXCEPTION directory at the expected kseg0 location and found code
  instead. Section mapping is more complex than rva = offset_from_load.

### `gxemul/`

No new commits this session. `e686a53` (the delayed misaligned jump fix
from 2026-04-12) is still required and is still in place. There are
unrelated local worktree changes under `gxemul/` and `src/be300_devices.c`
that are not part of this investigation.

## What Was Tried And What We Learned

### 1. Add per-word slot-publish trace and rearm logic (Phases A-C)

Continuation of the 2026-04-12 hypothesis that the slot at `0x01FE6544`
was being published incorrectly.

What the new traces showed:

- the per-word `WINCE_CB_FIRST` events never fired for the flag/ptr
  words because the watch was armed on the wrong physical address,
- adding a dynamic rearm at the consumer entry showed the watch PA
  was `0x00FFB544`, not the address of the consumer's actual data,
- the runtime PTEs in the watched L2 page were valid at first and
  later went to zero,
- but the timing made it clear the publish path never actually ran,
  rather than running and failing.

This re-cast the question from "why does publish skip flag/ptr" to
"why does the entire publish never run for this slot".

### 2. Capture the real faulter via WINCE_HOT_L2W and stack walk (Phases D-G)

Once we had per-store register state at each PTE zero write, the picture
shifted from "publish is skipped" to "the L2 page is being torn down".

What was learned:

- the real PTE zero store is `sw zero, 0x0c(s0)` at `0x800971BC`,
  not `0x80097000` (which is a `nop` in a delay slot),
- `cpu->pc` and `COP0_EPC` are both stale in dyntrans for this kind of
  store - the store comes from a dynamically-translated block whose IC
  page does not always re-enter our `note_pc()` hook,
- the loop is bounded by a count in `s1`, decrementing per iteration,
- each iteration calls `0x800A3244` with the masked PFN, then zeros the
  PTE entry, then advances `s0` by 4.

This is the body of `FUN_800970A8`. It is the WinCE VA-range UNMAP
primitive, not anything specific to callbacks. The "teardown" is correct
WinCE code.

### 3. Find the upstream caller via WINCE_WALKER_FRAME (Phases Y-AA)

The dyntrans `cpu->pc` problem made PC-only watchpoints unreliable. The
fix was to read the saved register frame at `sp+0x18..sp+0x3C` from the
walker's own prologue, before any later stack writes obscured them.

Concretely:

- the walker's prologue at `0x800970A8` saves `s0..s8` and `ra` into the
  64-byte frame at fixed offsets,
- reading `*(sp + 0x3C)` at the moment of the first `WINCE_HOT_L2W`
  inside the walker gives the unmodified caller `ra`,
- that `ra` is `0x80098144`, exactly inside `FUN_80097F44`,
- which means Phase H's original interpretation, that had been demoted as
  "reading data as code", was correct - it was correct code that my
  objdump was reading at the wrong file offset because my
  `--adjust-vma` was wrong by `0x16B50` bytes.

After re-disassembling the entire `0x80097F44` body with the corrected
`--adjust-vma=0x80060000`, the caller chain made sense again:

- `FUN_8008FE8C(module)` calls `FUN_80097F44` twice with different flag
  values (`0x4000` then `0x8000`),
- `FUN_80097F44` takes the MM lock at `0x806697A0` via `FUN_800998C0`,
  runs section-table sanity checks, calls `FUN_80096E88` (a range walker)
  twice, calls `FUN_800970A8` (the unmap walker) twice, and releases the
  lock via `FUN_80099924`,
- `FUN_8008FE8C` is called from `FUN_80090144` (the per-process
  FreeLibrary core),
- `FUN_80090144` is called from `FUN_800903BC` (a recursion-guarded
  wrapper),
- `FUN_800903BC` is called from `FUN_800927CC` (the WinCE DLL loader).

### 4. Identify the module (Phases AB, AC)

With the chain pinned to `FUN_800927CC`, the next question was which
module it was unloading.

What was done:

- read the module descriptor that the chain was operating on
  (`0x80FFFEA8`),
- read its `+0x64` field (TOC entry pointer) and walked the WinCE TOC
  entry layout to find `lpszFileName`,
- read the resulting kseg0 string at `0x80126FF0` byte by byte.

Result:

```text
[WINCE_TOC] name_at_+0x10 ptr=0x80126FF0 str='coredll.dll'
```

The descriptor at `0x80FFFEA8` is coredll's loaded-instance record. The
module size and base also match: `+0x54 = 0x03F80000` (slot-1 mirror
base), `+0x80 = 0x80000` (= 512 KB, matching the TOC entry size of
`0x7DC6A` rounded up to 4 KB).

Also confirmed via the `WINCE_TOC` enumerator that already runs at boot:

```text
mod[0]  attr=0x07     load=0x80060000   name=nk.exe
mod[1]  attr=0x1007   load=0x8033B000   name=coredll.dll
mod[2]  attr=0x07     load=0x800D2000   name=filesys.exe
```

So coredll's NK kseg0 base is `0x8033B000` and its slot-0 process VA is
`0x01F80000`. Our descriptor reads consistent with both of those.

### 5. Misread Phase AC's "DllMain failed" theory (Phase AE)

Phase AC's first interpretation was that `coredll`'s `DllMain` returned
FALSE, the loader set `ERROR_DLL_INIT_FAILED (0x45A)`, and triggered
the unload.

To verify, Phase AE read the kernel error field at `*(_DAT_FFFFDAC0 +
0x38)` at walker entry time. The result was `0`.

This was initially read as "DllMain theory is WRONG" - but on re-reading
`FUN_800927CC` more carefully, the error code in all three failure paths
is set AFTER `FUN_800903BC` returns:

```c
if (FUN_8008ff00(coredll, 1, 0) == 0) {
    iVar1 = 0;
    FUN_800903bc(coredll, 0);          // unload first
    *(error_field) = 0x45a;             // THEN set error
}
```

So at walker-entry time (deep inside `FUN_800903BC`), the error code is
not yet set. Reading `0` is exactly what we would see if any of the three
loader failure paths fired. **Phase AE's reading does not actually
distinguish the candidates.** It only rules out paths that set the error
BEFORE calling the unload, which is none of them.

### 6. Phase AG's "filesys.exe is exiting" was also wrong

Phase AG read `_DAT_FFFFDAC4` (current process pointer) and got
`0x806698E0`, with the first word `0x02000000`. That is slot 1 = filesys.exe.

The conclusion at the time was "filesys.exe is exiting and unloading its
modules". Phase AI reversed this:

- the saved ra at `sp+0x3C = 0x8009040C` lands inside `FUN_800903BC`,
- `FUN_800903BC` is the recursion-guarded wrapper called from
  `FUN_800927CC` (loader) and `FUN_800929CC` (loader cleanup),
- it is NOT called from any process-exit hook,
- so the unload is from the LOADER path, not from process termination,
- "current process is slot 1" is true but only because filesys.exe is the
  process whose loader is currently trying to load coredll.

So filesys.exe is being LOADED, not torn down. It is in the middle of
loading coredll, and the loader is rolling back coredll's load.

### 7. Eliminate two of the three loader failure candidates (Phases AK, AN)

`FUN_800927CC` has three failure paths that lead to `FUN_800903BC`:

1. `FUN_800907C8(coredll) == 0` - import resolution failure
2. `(*SUB_ffff9bf6)(...) == 0` - kernel attach hook returns 0
3. `FUN_8008FF00(coredll, 1, 0) == 0` - DllMain returns 0

Phase AK (module list dump) ruled out #1: coredll's descriptor `+0x98 = 0`
which is the imports table size. `FUN_800907C8` short-circuits to
`return 1` when there is no imports table. It can never return 0 for
coredll.

Phase AN (kernel hook gate) ruled out #2: the kernel-hook code in
`FUN_800927CC` is gated by `_DAT_80668C80 != 0` BEFORE the hook is even
called. Reading `_DAT_80668C80` returned `0`. The hook is therefore never
invoked, and #2 cannot be the trigger.

By elimination, the trigger is #3: `FUN_8008FF00(coredll, 1, 0)` returns
0. That is the WinCE call to `DllMain(coredll, DLL_PROCESS_ATTACH, NULL)`.

### 8. Locate coredll's DllMain pointer (Phases AD, AO, AP)

`FUN_8008FF00` calls `(**(coredll + 0x60))(coredll, 1, 0)`. The
descriptor `+0x60` is the function pointer the loader stores after
resolving coredll's PE32 entry RVA via `FUN_8009096C` (an RVA-to-VA
walker over the section table).

Read result:

- `descriptor +0x60 = 0x01F84A5C`

Then to verify whether this is correct, Phase AO read coredll's
`e32_lite` directly at the kseg0 VA stored in coredll's TOC entry
(`+0x14 = 0x80452F34`):

```text
[WINCE_E32] coredll e32_lite at 0x80452F34:
  +0x00 = 0x210E0004   (objcnt=4 | imageflags=0x210E)
  +0x04 = 0x00004A5C   (e32_entryrva)
  +0x08 = 0x01F80000   (e32_vbase, matches descriptor)
  +0x0C = 0x00000003   (subsys = WINDOWS_CE_GUI)
  +0x10 = 0x00010000   (stackmax = 64KB)
  +0x14 = 0x00080000   (vsize = 512KB, matches)
```

`e32_entryrva = 0x4A5C` resolves to `0x01F80000 + 0x4A5C = 0x01F84A5C`,
exactly matching `descriptor +0x60`. So FUN_8009096C is computing
correctly; the descriptor is NOT corrupt.

Phase AP validated the e32_lite field offsets by reading nk.exe's e32
at `0x80436F4C` (NK's TOC entry +0x14). Result:

```text
+0x04 = 0x00016B50  (e32_entryrva)
+0x08 = 0x80060000  (e32_vbase)
```

Computed entry VA = `0x80060000 + 0x16B50 = 0x80076B50`. This is the
exact NK kernel entry that CLAUDE.md documents. **The e32_lite field
layout is verified.** coredll's `e32_entryrva = 0x4A5C` is genuine.

### 9. The 244-byte mystery (Phases AD, AQ, AR)

Disassembling the bytes at coredll's kseg0 `0x8033FA5C` (= user
`0x01F84A5C`) with `--adjust-vma=0x80060000` decodes to:

```text
8033fa5c: 10000006   b 0x8033fa78    ; success-path branch
8033fa60: 24020001   li v0, 1         ; (delay slot) v0 = 1
8033fa64..8033fa74: failure cleanup that frees buffers and sets v0 = 0
8033fa78: 8fb00020   lw s0, 0x20(sp)  ; epilogue
8033fa7c: 8fbf0024   lw ra, 0x24(sp)
8033fa80: 03e00008   jr ra
8033fa84: 27bd0040   addiu sp, sp, 0x40
```

These bytes are the success-tail of `FUN_8033F968` (per Ghidra's
auto-detected function boundary). The xref to `0x8033FA5C` is from
`bnez v0, 0x8033fa5c` at `0x8033FA44`, which is inside the same function.

The real function entry at `0x8033F968` has a normal MIPS prologue:

```text
8033f968: 27bdffc0   addiu sp, sp, -64    ; allocate 64-byte frame
8033f96c: afbf0024   sw ra, 0x24(sp)      ; save ra
8033f970: afb00020   sw s0, 0x20(sp)      ; save s0
```

So `FUN_8033F968` is a normal 64-byte-frame function, and `0x8033FA5C` is
its mid-function success label. Calling `0x8033FA5C` directly as a
function pointer:

1. Sets `v0 = 1` (delay slot of the branch).
2. Branches to `0x8033FA78`.
3. `lw s0, 0x20(sp)` reads from the CALLER's stack at `sp+0x20`.
4. `lw ra, 0x24(sp)` reads from the CALLER's stack at `sp+0x24`.
5. `jr ra` jumps to whatever was at the caller's `sp+0x24`.
6. `addiu sp, sp, 0x40` corrupts the caller's `sp` by +64 bytes.

`FUN_8008FF00`'s prologue is `addiu sp, sp, -0x30` (48 bytes), so its
locals at `sp+0x20` and `sp+0x24` are unrelated to a 64-byte frame's
saved-reg area. The "DllMain call" reads garbage and `jr`s to garbage.

The kernel detects the resulting fault or treats the strange return as
"DllMain failed", and the loader rolls back via `FUN_800903BC`.

Also, `FUN_8033F968`'s decompilation does not look like a DllMain at all.
It takes 4 args, allocates two buffers via `func_0x81d67f78`, links them,
calls 3 helpers in `0x81d6xxxx`, and returns 1 on success. DllMain would
take 3 args (`hInst`, `fdwReason`, `lpvReserved`) and return BOOL. So
`FUN_8033F968` is NOT coredll's DllMain. Whatever it is, calling it from
`0x4A5C` as a 3-arg function is fundamentally wrong.

### 10. The EXCEPTION directory is not where I expected (Phase AR)

Phase AQ confirmed coredll has a PE32 `EXCEPTION` data directory at
RVA `0x67000` with size `0x896C`. In MIPS PE32, the EXCEPTION directory
holds `RUNTIME_FUNCTION` entries that authoritatively describe every
function's entry and exit. Walking it would tell us whether `0x4A5C` is
a real function entry or genuinely mid-function.

Phase AR tried to walk it at the obvious kseg0 mapping
(`0x8033B000 + 0x67000 = 0x803A2000`). The bytes there are not a function
table - they decode as MIPS code (`li v1, 11001; lhu v1, 10(s0); ...`).

So `EXCEPTION rva = 0x67000` does not translate to kseg0 the way
`load_offset + rva` would. coredll has 4 sections (`e32_objcnt = 4`),
and section 2 or 3 has its own `realaddr` that differs from
`kseg0_base + rva`. Phase AL only captured the first 2 of 4 section
table entries; the EXCEPTION directory's true kseg0 location requires
the full section table.

This is the cleanest stopping point for the runtime probing layer. Phase
AS would need to read all 4 section table entries from the slot-1
mirror, compute the EXCEPTION directory's true kseg0 VA, walk it, and
classify whether `0x4A5C` is a real function entry.

## Current Failure Chain

This is the current best end-to-end sequence to keep in mind.

1. NK boots, ROM and SPL hand off cleanly, NK reaches its first user
   process startup.
2. NK creates filesys.exe (slot 1) and starts its loader path
   `FUN_800927CC` to load coredll into the new process slot.
3. The loader allocates pool-5 L2 PTE groups for coredll's slot-0 and
   slot-1 mirror mappings via `FUN_80097CA0` / `FUN_80097AB0` - both
   succeed.
4. The loader resolves coredll's DllMain via `FUN_8009096C`, walking
   coredll's PE32 section table at slot-1 VA `0x03FE6570`.
5. `FUN_8009096C` correctly returns the resolved VA per the e32_entryrva,
   and the loader stores `0x01F84A5C` into `coredll->+0x60`.
6. The loader calls `FUN_8008FF00(coredll, 1, 0)` which dispatches
   `(*coredll->+0x60)(coredll, 1, 0)`.
7. The CPU executes `b 0x8033FA78; li v0, 1; lw s0, 0x20(sp); lw ra,
   0x24(sp); jr ra; addiu sp, sp, 0x40` against `FUN_8008FF00`'s 48-byte
   frame.
8. `lw ra, 0x24(sp)` reads garbage. `jr ra` jumps to garbage. WinCE's
   exception handler treats this as DllMain failure.
9. `FUN_800927CC` calls `FUN_800903BC` to roll back coredll's load.
10. `FUN_800903BC` -> `FUN_80090144` -> `FUN_8008FE8C` -> `FUN_80097F44`
    -> `FUN_800970A8` (the walker) zeros coredll's slot-0 and slot-1
    PTE entries via the shared L2 group at `0x80FFC1C8`.
11. After the walker finishes, any code that holds a slot-0 or slot-1
    pointer into coredll faults with read-zero.
12. The "consumer at `0x01F8F4D4`" we have been chasing since Phase A is
    actually a coredll function (offset `0xF4D4` from coredll's slot-0
    base) being entered via a stale call from elsewhere. It reads the
    "callback header at 0x01FE6544" (which is actually coredll's data
    section, offset `0x66544` from the base) and gets zero because the
    walker already cleared it.
13. The consumer's "deliberate fallback `jalr 0xFFFFF9A2`" at
    `0x01F8F4FC` runs.
14. WinCE prints `Exception 004` on serial.
15. The boot never reaches a stable GUI.

The 2026-04-12 report's "callback consumer never sees its slot header"
diagnosis is correct as a DESCRIPTION of step 12, but the cause is the
loader rollback in steps 6-10, not a missed publish.

## Why coredll's DllMain entry is broken

This is the open question.

Three ways the puzzle could resolve:

1. **Ghidra is wrong about FUN_8033F968's boundary.** The real
   coredll DllMain might start exactly at `0x8033FA5C` and Ghidra's
   auto-analysis grouped it with the previous function. To verify,
   manually create a function in Ghidra at `0x8033FA5C` and see what
   the decompilation looks like. If it is a sane DllMain (3 args, BOOL
   return), the puzzle is solved.
2. **WinCE 3.0 has a non-standard DLL entry calling convention** that
   pre-allocates a 64-byte frame on behalf of the callee. We have not
   found evidence of such a wrapper in `FUN_8008FF00`, but the call site
   could be using a feature we have not recognized.
3. **Our PE32 section-mapping arithmetic is wrong** for the EXCEPTION
   directory. If the EXCEPTION directory really lives at a different
   kseg0 address than `load_offset + rva`, our entire field-offset story
   could be off. Walking the EXCEPTION table once we have the section 2
   `realaddr` would settle it.

The "smoking gun" test is the EXCEPTION directory walk. The static
disasm and the runtime descriptor read are otherwise consistent with
each other.

## How To Reproduce

Build:

```bash
cmake --build build-host -j4
```

Primary repro:

```bash
bash -lc 'cd build-host && gtimeout 45s ./be300 --nand ../ce/restore_images/All_nand_300.bin > repro_phase_ar.stdout 2> repro_phase_ar.stderr; echo EXIT:$?'
```

Helpful greps once the run is captured:

```bash
rg -n "WINCE_HOT_L2W|WINCE_WALKER_FRAME|WINCE_F44_FRAME|WINCE_MODULE|WINCE_E32" build-host/repro_phase_ar.stderr
rg -n "WINCE_NK_E32|WINCE_E32_UNIT|WINCE_RTFUNC" build-host/repro_phase_ar.stderr
rg -n "WINCE_KERNEL_ERR|WINCE_PROC|WINCE_MODLIST|WINCE_HOOK" build-host/repro_phase_ar.stderr
rg -n "WINCE_SECTAB|WINCE_L1_LOOKUP" build-host/repro_phase_ar.stderr
```

The runtime probes are sticky - each one fires once per boot, adds
~10-100 lines to stderr, and is gated on the first hot L2W in the
walker's window. The whole probe set produces ~300 lines of analysis
output per boot, which is enough to decode the entire chain offline.

## How To Read The New Logs

### `WINCE_HOT_L2W_REGS`

Use this to attribute the `#25..#40` PTE write window. Each record is
3 lines (`a0..a3 + s0..s7 + t0..t9`), one for each of the 16 stores in
the publish/teardown window.

Most useful columns:

- `pc` is the runtime PC at the moment of the store. **Note that this
  can be stale due to dyntrans IC translation caching**, so trust the
  store offset (`off=0x0c..0x38`) over the PC value when assigning
  causality.
- `a0` for the walker stores is a slot-1 mirror VA derived from the
  publish/teardown loop's index register.

### `WINCE_WALKER_FRAME` / `WINCE_F44_FRAME`

These read the saved-register area from the walker's and FUN_80097F44's
prologues at the moment the walker's first PTE store fires. Use them
to recover the real caller chain when `cpu->pc` is unreliable:

- `WINCE_WALKER_FRAME saved_ra` = the PC inside FUN_80097F44 just after
  its `jal FUN_800970A8` at `0x8009813C` (saved_ra = `0x80098144`).
- `WINCE_F44_FRAME saved_ra` = the PC inside FUN_8008FE8C just after
  its `jal FUN_80097F44` at `0x8008FEC4` (saved_ra = `0x8008FECC`).

The saved-frame reads are MUCH more reliable than `cpu->pc` for any
question of the form "who called this".

### `WINCE_MODULE` / `WINCE_MODULE_FIELD`

Decodes the failing module descriptor at kseg0 `0x80FFFEA8` field by
field. Important fields:

- `+0x54 = 0x03F80000` slot-1 mirror base
- `+0x60 = 0x01F84A5C` DllMain VA (the broken one)
- `+0x64 = 0x80655CC8` TOC entry pointer (resolves to "coredll.dll")
- `+0x80 = 0x00080000` module size (512 KB)
- `+0xC0 = 0x00000003` per-process inUse mask (process slots 0 and 1)

### `WINCE_E32` / `WINCE_E32_UNIT`

Reads coredll's e32_lite at `0x80452F34` (TOC ulE32Offset). Confirms the
e32_entryrva is `0x4A5C` and not something we are misreading. The
companion `WINCE_E32_UNIT` dump shows the 6 PE32 data directories;
EXCEPTION at rva `0x67000` is the one we want to walk to settle the
entry-point puzzle.

### `WINCE_NK_E32`

Sanity-check probe for the e32_lite layout. Reads nk.exe's e32 at
`0x80436F4C` and computes the entry as `vbase + entryrva = 0x80076B50`,
which matches the known NK entry from CLAUDE.md. **Use this as a
regression check whenever you change anything about the e32 read code.**

### `WINCE_KERNEL_ERR`

Reads the kernel error field at `*(_DAT_FFFFDAC0 + 0x38)` at walker
entry time. As discussed in section 5 above, this returns `0` for ALL
three FUN_800927CC failure paths because the error is set after the
unload returns. **Do not interpret a 0 here as proof that no failure
happened.**

### `WINCE_MODLIST`

Walks the linked list of loaded modules from `_DAT_FFFFDB24`. At
walker-entry time the list contains exactly one entry (coredll itself),
because coredll's load is being rolled back as we capture this state.

### `WINCE_HOOK`

Reads `_DAT_80668C80` (the gate variable for the kernel attach hook
`SUB_ffff9bf6` in FUN_800927CC) plus the bytes around `0xFFFF9BF6`.
Result is `_DAT_80668C80 = 0` and the hook page is unmapped. This is
how candidate #2 was ruled out.

### `WINCE_SECTAB` / `WINCE_L1_LOOKUP`

`WINCE_SECTAB` was an early test that compared slot-0 and slot-1 reads
of the section table at `0x01FE6570` / `0x03FE6570`. The result was
"slot 0 unmapped, slot 1 fully populated" - which initially looked like
a slot-mirror bug.

`WINCE_L1_LOOKUP` shows that this was misleading. Both slot-0 and
slot-1 indices into the L1 table at `0x80668CC0` resolve to the SAME
L2 group at `0x80FFC1C8` because `(va >> 14) & 0x7FC` masks out the
slot bit. There is no slot-mirror asymmetry. The slot-0 read failure
in WINCE_SECTAB was just a TLB cache state artefact from
`load_va_word(NO_EXCEPTIONS)`.

## Current Theory

The current theory is:

- coredll's `e32_entryrva = 0x4A5C` is exactly what coredll's PE32
  declares,
- the WinCE loader at `FUN_800927CC` reads it correctly, resolves it
  via `FUN_8009096C`, and stores the resulting VA into the descriptor
  at `+0x60`,
- `FUN_8008FF00` then calls `(*+0x60)(coredll, 1, 0)`,
- the bytes at the called VA are mid-function code per Ghidra's auto-
  detected boundary, so the call corrupts the stack and "returns" to
  garbage,
- WinCE treats the strange return as DllMain failure and rolls back
  coredll's load,
- the rollback unmaps coredll's slot-0 and slot-1 mirror PTEs via the
  walker chain we have been chasing for 50 phases,
- after the unmap, the consumer at `0x01F8F4D4` (which is just an
  offset into coredll's text section) is somehow re-entered from
  another context, reads zero from coredll's now-unmapped data, and
  raises Exception 004,
- on real WinCE this whole chain is somehow handled correctly - either
  Ghidra's function boundary is wrong and `0x8033FA5C` IS a valid
  DllMain entry under a calling convention we do not yet recognize, or
  WinCE has a wrapper before `FUN_8008FF00` that pre-allocates the
  expected 64-byte frame.

## Recommended Next Steps

### 1. Read all 4 section table entries from slot-1 mirror

Phase AL only captured the first 2 of coredll's 4 section table entries
(64 bytes). Extend the existing `WINCE_SECTAB` probe to read 4*28 = 112
bytes and decode each section's `realaddr`. With section 2's `realaddr`
known, the EXCEPTION directory's true kseg0 VA is `section2_realaddr +
(EXCEPTION_rva - section2_rva)`.

### 2. Walk the EXCEPTION directory at its real kseg0 VA

Once the kseg0 VA is known, walk `RUNTIME_FUNCTION` entries (8 or 20
bytes each, format depends on the WinCE 3.0 ABI variant) looking for
the entry that covers RVA `0x4A5C`. If `BeginAddress == 0x4A5C`, then
`0x4A5C` IS a real function entry and the puzzle becomes "what
calling convention". If `BeginAddress < 0x4A5C < EndAddress`, then
`0x4A5C` is genuinely mid-function and the puzzle becomes "what
e32 field does WinCE actually use for DllMain".

### 3. Manually create a function in Ghidra at 0x8033FA5C

Ghidra's auto-analysis grouped `0x8033FA5C` into `FUN_8033F968`. If the
real function entry is `0x8033FA5C`, force-create a function there in
Ghidra and re-decompile. The result will either look like a sane
DllMain or like the same 4-arg constructor we already saw, and that
single test settles the question.

### 4. Compare against a WinCE 3.0 SDK header for e32_lite

The Microsoft Platform Builder for WinCE 3.0 ships the actual e32_lite
struct header. Phase AP confirmed our field offsets via NK's known
entry, but the e32_lite might have additional DLL-specific fields we
have not read. If the SDK header lists a separate `e32_dll_entry_rva`
or similar, that is the field WinCE actually uses for DllMain on DLLs.

### 5. Do NOT pursue these dead leads from earlier phases

The following hypotheses were tried and ruled out. Do not re-open them
unless new evidence demands it:

- "DllMain returned FALSE and the kernel error is 0x45A" - the error
  is set AFTER the unload returns, so reading 0 at walker-entry time
  proves nothing.
- "Process exit is unloading filesys.exe's modules" - the saved ra at
  `sp+0x3C` is inside `FUN_800903BC`, not inside any process exit hook.
- "Slot-0 and slot-1 mirror PTEs differ in encoding" - they share the
  same L2 group at `0x80FFC1C8` via `(va >> 14) & 0x7FC` masking the
  slot bit.
- "EXL=1 exception handler context" - Phase D's status reading was
  decoded incorrectly. Status = 0x00008401 has EXL=0; the walker runs
  in normal kernel mode.
- "Coredll has imports that fail" - coredll's imports table size at
  descriptor `+0x98 = 0`. There are no imports.
- "The kernel attach hook `SUB_ffff9bf6` returns 0" - the gate
  `_DAT_80668C80 = 0`, so the hook is never invoked at all.

## Practical Debugging Notes

- The objdump base correction in Phase Y (`--adjust-vma=0x80060000`,
  not `0x80076b50`) is a **load-bearing** detail. Any future static
  disasm of coredll, NK, or any module in the NK image must use the
  correct base. The Phase H / I / L / M / P / Q / R interpretations
  all looked broken until this was fixed.
- `cpu->pc` and `COP0_EPC` are both stale in dyntrans for some store
  PCs. The reliable way to attribute a store is to read the saved
  register frame from the containing function's prologue at the moment
  of the store - the pattern from `WINCE_WALKER_FRAME` / `WINCE_F44_FRAME`.
- Probe gating: every Phase B-AR probe is gated on the first hot L2W
  fire (`hot_user_l2_write_count == 30`) so they only print once per
  boot. If you add new probes, follow the same gating - flooding the
  log makes the chain hard to read.
- For PE32 layout questions, the 6 data directories at e32_lite +0x20
  are `[EXPORT, IMPORT, RESOURCE, EXCEPTION, SECURITY, BASERELOC]` in
  that order, each `(rva, size)` pair = 8 bytes. The IMPORT directory
  for coredll is empty, which is the cleanest disproof of Phase AC's
  original "import resolution failure" theory.

## Current Local State Notes

At the time of this report:

- top-level `main` is at `c55a67e6`.
- `gxemul` HEAD is unchanged from 2026-04-12 (`e686a53` plus the same
  unrelated local worktree changes).
- there are local worktree changes in `gxemul/` and
  `src/be300_devices.c` that are not part of this investigation.
- there are also untracked `.tmp_spl/*` analysis files (the `.gitignore`
  was updated in commit `0753d9cc` to keep them out of accidental commits).

Do not assume those unrelated local changes are part of the current
loader-rollback investigation. Review them before using them, and do
not revert them blindly.

## Bottom Line

The main handoff conclusion is:

- the "Exception 004 cold-boot deadlock" is fully attributed at the
  mechanism layer end-to-end,
- the walker that zeros the L2 PTEs is correct WinCE code doing a
  legitimate per-process unmap of coredll for filesys.exe's slot,
- the trigger is the WinCE loader rolling back coredll's load after
  treating its DllMain call as a failure,
- the loader's `FUN_8008FF00` calls `(*coredll->+0x60)(coredll, 1, 0)`
  using the resolved e32_entryrva, and the bytes at that VA do not
  look like a callable function entry under any normal MIPS calling
  convention,
- the next pass needs to walk coredll's PE32 EXCEPTION directory to
  authoritatively answer whether `0x4A5C` is a real function entry,
- and the runtime instrumentation layer is now mature enough that
  further progress should come from the EXCEPTION directory walk and
  static analysis, not from more PC probes.

If the next pass can prove whether `0x01F84A5C` is a valid coredll
DllMain entry, the cold-boot deadlock either becomes a known coredll
calling-convention problem (fix is in WinCE loader emulation) or a
known PE32 layout misread (fix is in our static analysis tooling). Both
outcomes are concrete and recognizable - the investigation is no longer
groping in the dark.

## Phase AS Addendum (2026-04-13, commit 99061abb)

Phase AS answered the open question and the answer overturns the
"mid-function" hypothesis from Phases AQ-AR. **Read this section
before acting on anything above.**

### Smoking gun

Phase AS added three probes to `src/wince_boot.c`:

1. `WINCE_SECTAB_AS` - read the `o32_lite` section table from `e32_va
   + 0x6C` (not from slot-1 VA `0x03FE6570`, which was a misread - that
   VA is in coredll's data section, not the section table). WinCE 3.0
   `o32_lite` is 24 bytes per entry. Decoded 4 entries:

   ```text
   idx=0 .text   vsize=0x64F3E  rva=0x1000   realaddr=0x800BC000
   idx=1 .data   vsize=0x558    rva=0x66000  realaddr=0x800B7C54
   idx=2 .pdata  vsize=0x896C   rva=0x67000  realaddr=0x805A7000
   idx=3 .rsrc   vsize=0x6000   rva=0x70000  realaddr=0x80121000
   ```

   Note that the e32_unit directory table at `e32_va + 0x20` has
   **9 entries** (EXPORT, IMPORT, RESOURCE, EXCEPTION, SECURITY,
   BASERELOC, DEBUG, COPYRIGHT, GLOBALPTR), not 6. The prior
   `WINCE_E32_UNIT` probe only read the first 6, but the section
   table offset depends on reading all 9 (72 bytes) plus one 4-byte
   trailer, so section table starts at `e32_va + 0x6C`.

2. `WINCE_EXC_VA` - located EXCEPTION directory at kseg0 `0x805A7000`
   (section 2's realaddr, since `exc_rva == sec_rva[2]`). The prior
   Phase AR guess `0x803A2000 = 0x8033B000 + 0x67000` was wrong by
   the .text/kseg0 mismatch described below.

3. `WINCE_DLLMAIN_INSN` - the definitive test. Dumped 16 instructions
   around rva `0x4A5C` via three candidate source VAs:

   - **A)** section 0 realaddr-based: `0x800BC000 + (0x4A5C - 0x1000)
     = 0x800BFA5C`
   - **B)** NK kseg0 base + rva: `0x8033B000 + 0x4A5C = 0x8033FA5C`
     (the Phase AR / Ghidra disasm address)
   - **C)** slot-0 user VA: `0x01F80000 + 0x4A5C = 0x01F84A5C`

   Result: **A and C are byte-identical, B is completely different.**
   At the real `.text` VA (A/C), the target instruction is
   `0x27BDFFB8 = addiu sp, sp, -72` - a clean MIPS function prologue.
   The next four instructions are:

   ```text
   0x800BFA60  AFBF0014  sw ra, 0x14(sp)
   0x800BFA64  AFA40048  sw a0, 0x48(sp)
   0x800BFA68  AFA5004C  sw a1, 0x4C(sp)
   0x800BFA6C  AFA60050  sw a2, 0x50(sp)
   ```

   That is exactly a 3-arg DllMain prologue (`hinstDLL, fdwReason,
   lpvReserved`).

### What this overturns

- **coredll's `.text` does NOT live at `NK_kseg0_base + section_rva`.**
  The `0x8033B000` "coredll load address" reported by `WINCE_TOC` and
  `WINCE_MODLIST` is the address of coredll's **descriptor/header area**
  in the NK image, not the runtime base of its `.text`. The `.text`
  section lives at `section0.realaddr = 0x800BC000`, which is a
  separate region of the NK-loaded RAM entirely.
- **Ghidra's `FUN_8033F968` and its "mid-function" boundary have
  nothing to do with coredll's DllMain.** They describe bytes at
  `0x8033FA5C`, which is **not** coredll code in the runtime image.
  Whatever function Ghidra identified there is some other module's
  text or unused header space.
- **Phase AR's entire RUNTIME_FUNCTION walk was doomed** because its
  `exc_va = 0x803A2000` was derived from the same wrong base. The
  real EXCEPTION directory is at `0x805A7000`, and its contents do
  NOT decode as standard 8- or 20-byte RUNTIME_FUNCTION entries
  (likely WinCE-specific packed or lazy-decompressed format). The
  EXCEPTION walk is therefore a dead end; do not chase it further.
- **The "DllMain returned 0" failure candidate is no longer the
  obvious trigger.** If the bytes at `0x01F84A5C` are a clean
  prologue, dispatch via `(*coredll->+0x60)(coredll, 1, 0)` should
  enter DllMain normally. Either DllMain legitimately returns 0
  for some reason, or `FUN_8008FF00` has a pre-dispatch gate we did
  not read, or the loader rollback is triggered by a 4th path we
  have not enumerated, or the walker is firing from an entirely
  different call chain than Phase Y attributed.

### What still holds

- The walker (`FUN_800970A8`) IS running and IS zeroing PTEs. Phase
  AS's probes fired inside its window, confirming the trigger is
  still active.
- The descriptor at `0x80FFFEA8` really is coredll's (confirmed via
  TOC name lookup in Phase AC).
- The WalkerFrame / F44Frame saved-ra chain pointing to
  `FUN_800903BC` in NK kernel code is still valid, because NK's
  `.text` really is at kseg0 base `0x80060000` (verified against
  NK entry `0x80076B50`). Only **coredll's** `.text` was at a
  different base than assumed.
- The 2026-04-12 report's "Exception 004 callback-consumer fault"
  and "callback header at `0x01FE6544` reads zero" observations
  are still accurate, but their cause is still the loader-rollback
  unmap we have been chasing - just triggered by something other
  than bad DllMain bytes.

### Recommended next steps (supersedes the list above)

1. **Stop pursuing the EXCEPTION directory walk.** The bytes at
   `0x805A7000` don't parse as RUNTIME_FUNCTION entries, and even if
   they did, we now have direct byte-level proof that `0x4A5C` is a
   function entry. No further static validation needed.
2. **Trace `FUN_8008FF00`'s return value and entry state.** Add a
   probe that reads the CPU state right before and right after the
   `jalr` that calls `(*coredll->+0x60)`. Check:
   - Does execution actually reach the dispatch?
   - What `v0` (return value) comes back?
   - Does DllMain enter and run normally, or fault immediately?
3. **Enumerate ALL paths from `FUN_800927CC` to `FUN_800903BC`.**
   The handoff doc listed 3, but the assumption that those are
   exhaustive was based on static analysis done with the wrong
   coredll `.text` base. Re-disassemble `FUN_800927CC` (which IS
   in NK, so base `0x80060000` is correct) and re-enumerate every
   call to `FUN_800903BC` and every conditional that leads to one.
4. **Suspect TLB/PTE timing, not bytes.** The walker unmaps coredll
   PTEs. If kernel dispatch `jalr` to `0x01F84A5C` runs AFTER the
   PTEs have been zeroed, the CPU takes a TLB miss / bus error on
   the first instruction fetch and DllMain "returns" to garbage.
   But a probe via `load_va_word` moments later could still succeed
   if the host-memory fast path bypasses the guest TLB. Verify by
   triggering the probe BEFORE the walker starts (earlier hot count,
   or a different gate like "first write to coredll descriptor
   +0x60") instead of inside the walker's execution window.
5. **Do NOT trust any Ghidra address in the `0x8033xxxx` range as
   coredll code.** Re-run coredll disassembly in the Docker container
   against kseg0 starting at `0x800BC000` (section 0 realaddr), NOT
   `0x8033B000`. The `--adjust-vma` value for coredll objdump is
   `0x800BB000` (so that file offset 0x1000 lands at VA 0x800BC000).

### Module base mapping (authoritative, as of Phase AS)

| Module     | Descriptor VA  | .text realaddr | slot-0 VA base | slot-1 VA base |
|------------|----------------|----------------|----------------|----------------|
| nk.exe     | 0x80060000     | 0x80076B50 (entry) | n/a (kernel) | n/a (kernel) |
| coredll    | 0x8033B000     | 0x800BC000     | 0x01F80000     | 0x03F80000     |
| filesys    | 0x800D2000     | tbd            | tbd            | tbd            |

For any future module-relative address computation: **use o32_lite
section table `realaddr` as the authoritative source, not the TOC/
descriptor `load` field.** The descriptor `load` points at the
module's header area (for inspection), while `realaddr` per section
points at the actual runtime bytes.

## Phase AT/AU/AV Addendum (2026-04-13, commits 7824503c, 6a55d275)

Extending Phase AS, three more phases re-framed the trigger question
from "what makes the loader roll back coredll" to "why is there a
map-then-unmap sequence on coredll's tail at all". **Read this
section before starting any new probe work.**

### Phase AT - the "first hot user L2 write" is early boot, not the walker

A probe gated on `hot_user_l2_write_count == 1` fires in ROM / SPL
early-init code, not inside the walker:
- `sp = 0xA0003800` (kseg1 early stack, not NK kernel stack)
- `coredll_desc +0x60 = 0x00000000` (not yet populated)
- slot-0 VA `0x01F84A5C` and slot-1 VA `0x03F84A5C` are **both
  unmapped** - only kseg0 `0x800BFA5C` reads the valid prologue
- walker saved_ra at `sp+0x3C = 0` (not a real walker frame)

This confirms coredll's raw `.text` is loaded by SPL into kseg0
(`0x800BC000` region) very early, before user paging exists.

The Phase AS reads at `n==30` (deep inside the walker) showed
`coredll_desc +0x60 = 0x01F84A5C` AND slot-0 `0x01F84A5C` still
mapped to the valid prologue. **Combined with Phase AT, this proves
the walker is NOT unmapping coredll's DllMain page.**

### Phase AU - the walker targets coredll's tail (.text tail + .data + .pdata)

Classified the walker's zero targets by decoding existing
`WINCE_HOT_L2W_VA` entries (no new probe needed). The walker at
`pc=0x80097000` (FUN_800970A8) zeros pte_slots 1..7 of the L2 group
at `0x80FFC1C8`, which are paired 4K PTE entries mapping slot-0 VAs:

```text
0x01FE2000 .. 0x01FEF000   (= coredll rva 0x62000 .. 0x6F000, 14 pages, 56 KB)
```

Page-by-page section mapping:

| rva range        | pages | section                                    |
|------------------|-------|--------------------------------------------|
| 0x62000-0x64000  | 3     | last 3 pages of .text (ends at rva 0x64F3E) |
| 0x65000          | 1     | hole between .text and .data               |
| 0x66000          | 1     | .data (rva 0x66000-0x66558)                |
| 0x67000-0x6F000  | 9     | all of .pdata (EXCEPTION, 0x67000-0x6F96C) |

The DllMain page at rva `0x4000` is NOT touched. `.rsrc` at rva
`0x70000+` is NOT touched. The walker zeros only the **tail** of
coredll from end-of-text through end-of-pdata.

**Immediately before the walker (stores #25..#29 in
`WINCE_HOT_L2W`), a DIFFERENT pc (`0x800984B4`) PUBLISHES valid PTEs
into the same L2 group** - specifically pte_slots 4..7 (VAs
`0x01FE8000..0x01FEF000` = coredll rva `0x68000..0x6F000` = the
`.pdata` core). PFNs written are `0xFF8..0xFFD`.

So the sequence is:

1. Some NK routine maps `.pdata` pages (4..7) into the L2 group
2. Immediately after, FUN_800970A8 zeros slots 1..7 of that group
3. The `.data` page at slot 3 (rva 0x66000) and `.text` tail pages
   at slots 1..2 (rva 0x62000-0x64000) get zeroed even though the
   publisher did not touch them - they were already mapped from
   earlier setup

This is a paired **map-then-unmap** operation on coredll's tail,
not a module rollback. The 2026-04-12 "callback consumer at
`0x01FE6544` reads zero" fault is a **stale reference**: some
earlier code captured a pointer into rva `0x66544` (in the .data
page), and after this routine ran the .data page was legitimately
zeroed, so the consumer reads zero.

### Phase AV - publisher and walker live in the same NK function

Disassembled the publisher's region. The inner PTE-publish loop at
`0x80098484..0x800984D8` is:

```text
80098484: (loop top - reached via bnez from 800984d8)
80098490: sw   t0, 12(s1)       ; store PTE (skip path)
80098494: b    0x800984c0
80098498:   addiu s3, s3, 256   ; (delay) PFN += 0x100
8009849c: lw   v0, 12(t1)        ; read old PTE
800984a0: sw   v1, 148(sp)
800984a4: and  s0, v0, s8        ; mask
800984a8: move s3, s0
800984ac: jal  0x800a31bc        ; per-entry helper
800984b0:   move a0, s0
800984b4: lw   v1, 148(sp)       ; <-- dyntrans reports pc here
800984b8: or   t3, s0, s7        ; or PTE flags (s7)
800984bc: sw   t3, 12(s1)        ; publish PTE|flags
800984c0: addiu s5, s5, -1       ; count--
800984c8: addiu s2, s2, 4
800984d0: addiu s1, s1, 4        ; advance L2 entry ptr
800984d4: slti  at, s4, 16       ; i < 16
800984d8: bnez  at, 0x80098484   ; loop
```

The outer scope at `0x80098300..0x800983a4` performs:
- a table lookup `lw a0, -10048(t4)` with `t4 = (v0 & 0x3F) << 2`
  (module-by-index lookup, 64-entry table)
- a call to `FUN_80096E50` at `0x80098348`
- a call to **`FUN_80096E88`** at `0x8009837C`

`FUN_80096E88` is the same range walker the handoff doc attributes
to `FUN_80097F44`. So **the containing function for the publisher
IS `FUN_80097F44` (or its outer wrapper)**. This is not a separate
"publisher" function - it's the same `FUN_80097F44` that the
handoff doc already identified as the caller of the unmap walker.

**Conclusion: a single NK function, under the MM lock, does both
the publish and the unmap.** It is one atomic map-then-unmap
sequence, not a map by one code path followed by an unmap by
another. The handoff doc's attribution of FUN_80097F44 as the
caller of FUN_800970A8 is correct; what the handoff missed is that
FUN_80097F44 ALSO publishes PTEs just before it unmaps them, and
the publish+unmap are in the SAME function invocation.

### Revised trigger hypothesis

This is the current best understanding, superseding all prior
"DllMain failed" / "loader rollback" theories:

1. Something triggers `FUN_80097F44` to run on coredll's tail L2
   group `0x80FFC1C8`.
2. `FUN_80097F44` acquires the MM lock, does section-table sanity
   checks, publishes new PTEs for pte_slots 4..7 via the inner loop
   at `0x80098484`, then unmaps pte_slots 1..7 via
   `FUN_800970A8` (the walker), then releases the MM lock.
3. The zeroed range `0x01FE2000..0x01FEF000` = coredll rva
   `0x62000..0x6F000` covers text tail + data + pdata.
4. AFTER the unmap, a stale consumer holding a pointer into rva
   `0x66544` (the .data page) reads zero and faults with
   Exception 004.

The open questions are now:

1. **Who calls FUN_80097F44?** The handoff doc traced
   `FUN_800903BC -> FUN_80090144 -> FUN_8008FE8C -> FUN_80097F44`.
   If that chain is correct, then some code in the FreeLibrary
   core is calling FUN_80097F44 with coredll's tail L2 group, but
   WHY this happens on cold boot when coredll should not be freed
   is unknown. Re-verify by reading `WINCE_WALKER_FRAME` and
   `WINCE_F44_FRAME` saved_ra values from the current run.
2. **Why does the publisher publish pte_slots 4..7 just before the
   walker unmaps 1..7?** A map-then-immediately-unmap pattern only
   makes sense if the publisher and walker are doing different
   operations that happen to touch the same L2 group - e.g.
   publisher sets up a shared page aliased to something, walker
   detaches a different alias. The map+unmap being in the same
   function under the MM lock suggests this is an atomic
   re-mapping of coredll's tail (probably moving it from one slot
   to another, or copying to a reclaim pool).
3. **Who is the stale consumer at rva 0x66544?** Probably a slot-0
   function that cached a pointer into coredll's .data section
   during an earlier call. To find it, search coredll's .text
   (at `--adjust-vma=0x800BB000`) for any `lw`/`sw` loading from
   rva 0x66544 or a nearby value.

### Recommended next steps (supersedes earlier lists)

1. **Re-verify the `FUN_80097F44` caller chain.** The current run
   still has `WINCE_WALKER_FRAME` and `WINCE_F44_FRAME` probes
   firing at `n==30`. Extract the saved_ra values and confirm
   whether they point at `FUN_800903BC` (loader rollback) or
   somewhere else entirely. The handoff doc's attribution was made
   when the investigation believed in the DllMain-failure chain;
   it may still be right, or it may be another artifact of the
   wrong coredll base.
2. **Disassemble FUN_80097F44 end-to-end** with the correct NK
   base (`--adjust-vma=0x80060000`) to understand what the
   publish-then-unmap sequence is actually doing. The
   publisher loop at `0x80098484` is inside a larger structure -
   decode the outer control flow around `0x80098300..0x800984E0`.
3. **Find the consumer of rva 0x66544** by searching
   `coredll_decompressed` bytes (at section 0 realaddr
   `0x800BC000`) for any instruction that references offset
   `0x65544` (= `0x66544 - 0x1000`). Search the whole `.text`
   (rva `0x1000..0x64F3E`). Use `--adjust-vma=0x800BB000` so file
   offset 0x1000 lands at VA 0x800BC000.
4. **Do NOT add more runtime probes until the caller chain is
   re-verified from the existing run log.** The current
   instrumentation is saturated - more probes without more
   understanding will not narrow the trigger further.

### Current authoritative function identifications (from NK at base 0x80060000)

| Symbol          | VA          | Role                                             |
|-----------------|-------------|--------------------------------------------------|
| FUN_80096E50    | 0x80096E50  | helper called by FUN_80097F44 inner              |
| FUN_80096E88    | 0x80096E88  | range walker (called twice by FUN_80097F44)      |
| FUN_80097F44    | 0x80097F44  | MM-locked map-then-unmap function on L2 groups   |
| FUN_800970A8    | 0x800970A8  | L2 PTE zero walker (called by FUN_80097F44)      |
| FUN_800A31BC    | 0x800A31BC  | per-entry publisher helper (purpose unknown)     |
| publisher loop  | 0x80098484  | inner body of FUN_80097F44, PTE build loop       |
| FUN_8008FE8C    | 0x8008FE8C  | calls FUN_80097F44 (FreeLibrary core? re-verify) |
| FUN_80090144    | 0x80090144  | calls FUN_8008FE8C (re-verify)                   |
| FUN_800903BC    | 0x800903BC  | calls FUN_80090144 (re-verify)                   |
| FUN_800927CC    | 0x800927CC  | WinCE DLL loader (supposedly calls 800903BC)     |
| FUN_8008FF00    | 0x8008FF00  | calls (*coredll+0x60)(coredll,1,0)               |

The top 5 are confirmed by Phase AV disassembly. The bottom 5 are
inherited from the pre-Phase-AS handoff and should be re-verified
now that we know the prior chain attribution was made with the
wrong coredll base.
