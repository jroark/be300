# Phase AH: state of investigation, where we actually are

Date: 2026-04-13

After 33 phases of runtime probing and Ghidra cross-walking, here is
what we have proven, what we have disproven, and the smallest set of
unanswered questions.

## Proven facts

1. The cold-boot deadlock manifests as Exception 004 at user PC
   `0x01F8F4FC` after the consumer at `0x01F8F4D4` reads zero from
   user VA `0x01FE6544..0x01FE6558`.

2. The "callback header at 0x01FE6544" is **the data section of
   `coredll.dll`** in process slot 0. Confirmed via the loaded-
   instance descriptor at kseg0 `0x80FFFEA8`, whose pTOC entry
   pointer at `+0x64` resolves to `"coredll.dll"` via the TOC
   name lookup.

3. The 30-phase-long PTE zero stream we observed at
   PA `0x00FFC1C8..+0x40` is the runtime body of `FUN_800970A8`
   (the WinCE VA range UNMAP primitive), specifically its
   `sw zero, 0xc(s0)` store at `0x800971BC` followed by
   `jal 0x800A3244` PFN free at `0x800971B8`. Ghidra decomp
   matches the runtime trace exactly.

4. `FUN_800970A8` is reached via the chain
   `FUN_80097F44 -> FUN_800970A8` at callsite `0x8009813C`,
   confirmed via the walker's saved-ra at `sp+0x3C = 0x80098144`.
   Phase H's original interpretation was correct; my objdump base
   was wrong by 0x16B50 bytes for 24 phases.

5. `FUN_80097F44` is called from `FUN_8008FE8C` (a tiny tear-down
   pair stub) at callsite `0x8008FEC4`, confirmed via FUN_8008FE8C's
   saved-ra at `sp+0x14 = 0x80090240`.

6. `FUN_8008FE8C` is called from `FUN_80090144` (the FreeLibrary-
   style core unload) at callsite `0x80090238`. Confirmed via the
   stack walk's saved-ra at `sp+0x3C = 0x8009040C`.

7. `FUN_80090144`'s "full unload" branch fires when
   `FUN_80090084(module)` returns 0. `FUN_80090084` decompiles to
   the **per-process refcount decrement** primitive: it decrements
   `*(module + (current_proc * 2) + 0x14)` and returns the new
   value. Returning 0 means "the current process just released its
   last reference to this module".

8. The current process (read via `_DAT_FFFFDAC4`) is in **slot 1**,
   with descriptor at kseg0 `0x806698E0`. Per the TOC enumeration
   at boot, slot 1 corresponds to `filesys.exe` (the second module
   loaded after nk.exe and coredll.dll).

9. The kernel error code at `*(_DAT_FFFFDAC0 + 0x38)` at walker
   entry time is `0`. None of the known error-setting paths in
   `FUN_800927CC` (which set `0x45A`, `0xE`, `0x57`) has fired
   before the walker runs. This decisively disproves the Phase AC
   "DllMain returned FALSE" theory.

## Strongly suspected, not yet proven

10. The unload chain is being driven by `filesys.exe` releasing
    its references to `coredll.dll` as part of its own teardown.
    Either filesys.exe is exiting (possibly via an unhandled
    exception) or its load is being rolled back midway.

11. After the unmap, some other context (NK or another process)
    holds a stale slot-0/slot-1 mirror pointer to coredll's data
    and dereferences it, reading zero, falling into the consumer
    at `0x01F8F4D4`'s failure trampoline.

## Newly opened question (Phase AH)

12. `FUN_80097AB0` is the WinCE VA range MAP primitive (sibling of
    the unmap walker). It allocates pool-5 L2 groups, calls
    `FUN_800A2F34()` to allocate physical pages with retry+sleep
    loop, and installs PTEs. Its address `0x80097AEC` appears at
    `sp+0xBC` in the runtime stack walk above the walker, which
    suggests **a MAP operation may be on the same call stack as
    the UNMAP walker**.

    Two interpretations:
    a. The MAP function and its descendants enter UNMAP via a
       cleanup or retry path inside the MM lock - the same call
       stack contains both, with the unmap as a sub-step of map.
    b. The 0x80097AEC value at sp+0xBC is a stack ghost from an
       earlier frame that has not been overwritten - false positive.

    Static check: Ghidra decomp of FUN_80097AB0 does NOT show any
    direct call to FUN_80097F44 or the unmap walker. So if the
    map is on the same stack, the connection is via an indirect
    path (function pointer, exception handler, etc).

## What to discard

- Phase AC: "coredll's DllMain returned FALSE" — kernel error = 0
  disproves
- Phase Q: "v0=0 verify return triggers walker" — the `beq v0, a3`
  guard in FUN_80097F44's body is real but it does not gate the
  walker; the walker is called unconditionally on the unload path
- Phase H..V causal chain about "verify-then-free count mismatch"
- Phase D: "EXL=1 exception handler context" — Status bit decoding
  was wrong; walker runs in normal kernel mode

## Cleanest forward path

Stop adding runtime probes. The remaining investigation needs
**static analysis of the call graph upward from FUN_80090144**
to find which non-recursive path actually invokes it during cold
boot. Specifically:

1. Decompile `FUN_8008FDF8` - it both calls FUN_80090144 (xref)
   AND is called by FUN_80090144 (via ProcessAttach). Mutual
   recursion is suspicious. Find which call direction matters at
   our cold-boot point.

2. Decompile `FUN_8008E724` (FreeLibraryByName) and its callers.
   It's the simplest invocation path and may be called directly
   by a process tear-down routine.

3. Decompile any function that calls `FUN_8008E56C` (the post-
   unload finalizer that FUN_80090144 calls at its very end).
   Its callers tell us what the "complete unload" final step is
   for.

4. Look for an NK process-exit / process-terminate function that
   loops over the loaded-modules list and calls FUN_80090144 for
   each one. If it exists, its caller gives us the "why
   filesys.exe is exiting" answer.

## Honest assessment

The runtime instrumentation phase is over. We have:

- The exact instruction that zeros the PTEs (FUN_800970A8 at
  0x800971BC)
- The function chain from there to FUN_80090144 (the core
  unload)
- The identity of the failing module (coredll.dll)
- The identity of the process whose cleanup triggers the unload
  (filesys.exe = slot 1)

We do NOT have:

- The reason filesys.exe is being torn down
- Whether the tear-down is legitimate or a side-effect of an
  earlier emulator bug
- The actual "fix" target

Further runtime probes will not narrow these faster than reading
4-6 more Ghidra decomps. Phase AI should be 100% Ghidra reads,
no more emulator code edits, until the cold-boot trigger is
named.

## Commits reference

| Phase | Commit     | Subject                                          |
|-------|------------|--------------------------------------------------|
| F     | 63a87e88   | live disasm probe + wide L2 page watch           |
| Y     | 21a01b46   | objdump base was wrong, walker frame confirms    |
| Z+    | 9746f1c2   | proximate root cause - slot-1 shared module      |
| AA    | c933cc88   | DLL_INIT_FAILED rollback identified              |
| AB    | 12a3ad58   | DLL identified by descriptor                     |
| AC    | 9b7defe1   | DLL confirmed as coredll.dll                     |
| AD    | 50ccff58   | DllMain entry mismatch flagged                   |
| AE    | 31892dc4   | kernel error code is ZERO at walker entry        |
| AF    | dd4a20f3   | reframe - per-process teardown not DLL_INIT_FAIL |
| AG    | 013a60cb   | current process is slot 1 = filesys.exe          |
| AH    | (this doc) | reassessment, FUN_80097AB0 in stack              |
