# Phase AF: per-process unload, not DLL load failure

Date: 2026-04-13

Phase AE confirmed kernel error at walker entry is `0`. Phase AF
decompiled `FUN_80090084` (the function we were calling "the
refcount check") and discovered it's the actual per-process
dereference primitive. This unifies the entire 30-phase chase
under a corrected model.

## What `FUN_80090084` actually does

```c
short FUN_80090084(int module_desc) {
    int *refcount_ptr = module_desc + (current_proc_slot * 2) + 0x14;
    short new_refcount = *refcount_ptr - 1;
    *refcount_ptr = new_refcount;
    if (new_refcount == 0) {
        // clear per-process bit in module's inUse mask at +0xC
        *(module_desc + 0xC) &= ~(1 << current_proc_slot);
    }
    // also handles chained dependency at module->+0xCC
    int chain = *(module_desc + 0xCC);
    if (chain != 0 && chain != -1) {
        // recursively decrement the chain's per-process refcount
        ...
    }
    return new_refcount;  // returns NEW per-process refcount
}
```

It is **not** a refcount _check_; it is the per-process _decrement_.
`current_proc_slot` is `*_DAT_FFFFDAC4`, a per-thread/per-process
selector.

## What `FUN_80090144` actually does

```c
short new_refcount = FUN_80090084(module_desc);
if (new_refcount != 0) {
    // partial cleanup path (LAB_80090338)
    return;
}
// full per-process unload:
//   call destructors via FUN_8008FF00(..., 0, 0)  ; DLL_PROCESS_DETACH
//   recursively unload chain at +0xCC
//   call FUN_8008FE8C(module) to tear down PTEs
//   ...
```

So the **full unload path fires when the per-process refcount hits
0 for the current process** — i.e. when this process is detaching
from the module. It does NOT mean the global refcount is 0; it just
means the current process is releasing its mapping.

## Consequence for the entire investigation

1. The walker we have been chasing for 30 phases is **legitimate
   per-process unload code**, NOT a rollback of a failed
   DllMain. There is no error condition; the unload fires because
   the process is releasing its references.

2. Phase AC's "DllMain returned FALSE → DLL_INIT_FAILED" theory
   is fully discredited. Kernel error at walker entry is 0; the
   unload path doesn't set any error.

3. The "callback consumer at 0x01F8F4D4" we observed in Phase A
   reading zero from the data section was NOT a callback into the
   unloaded DLL. It was code (probably in coredll itself, or
   another DLL still loaded) that holds a pointer to coredll's
   user-VA data and dereferences it AFTER the per-process unmap
   has stripped the slot-0/slot-1 mirrors.

4. The 30-phase teardown chain is actually:
   ```
   <some early process is being terminated>
     → dereference all its loaded modules
     → for each module that hits ref 0:
         FreeLibrary-equivalent path
         → FUN_80090084 returns 0 (last ref)
         → FUN_80090144 full unload
         → FUN_8008FE8C tear down per-process PTE map
         → FUN_80097F44 → FUN_800970A8 (the walker)
         → walker zeros L2 PTE entries for that process's mapping
   ```

5. The remaining question: **which early-boot process is exiting?**
   Cold boot should be NK + filesys.exe + a few system processes,
   none of which should exit during init. If a process IS exiting,
   either:
   - It crashed (exception unhandled)
   - It legitimately ran to completion (e.g., a one-shot init helper)
   - Our emulator caused it to terminate via an emulator-side error

6. The "consumer at 0x01F8F4D4" then runs in the parent context
   (NK or another process) and dereferences a pointer that was
   valid in the now-terminated child process's address space. In
   that other context the pages are unmapped, so the read returns
   zero, and the code falls into the failure trampoline.

## What changes for the fix

We are no longer looking for "why does DllMain fail". We are
looking for "why does an early cold-boot process terminate, and
why does another context retain a pointer to its address space
that survives past the termination."

This is much closer to a recognizable WinCE bug pattern: a stale
inter-process pointer, possibly because:
- A callback was registered with a user-VA pointer that should
  have been kseg0
- A thread in another process is holding a coredll-relative
  pointer that points into coredll's per-process slot
- The terminating process's exit handler set a global hook that
  the parent process tries to invoke

## Phase AG target

Identify the terminating process. Two probes:

1. Read the current process pointer at `_DAT_FFFFDAC4` and dump
   its name string (process descriptors typically have a name
   field).

2. Read the next-up frame's saved ra to find FUN_80090144's
   caller — Ghidra showed it has 3 callers, including
   `FUN_800903BC` and recursive self-call. The non-recursive
   caller at the moment of our trace is the `process_exit_cleanup`
   function. Find that, decompile it, and we'll see what triggered
   the process exit.

The investigation is finally past the symptom layer. Phase AG
should give us the actual cause within a few more phases.

## Stable claims

- Walker is legitimate per-process unmap code, not rollback
- The unload fires as part of process termination
- Kernel error at walker entry is 0
- The module being unloaded is coredll.dll
- The "consumer reading zero" is post-unmap dereference from
  a stale pointer in another context

## Discarded claims

- Phase AC: "DllMain returned FALSE" — error code = 0 disproves
- Phase Q: "v0=0 verify return triggers walker" — the walker is
  triggered by per-process refcount drop, not a verify guard
- Phase H..V: "verify-then-free count gates walker" — there is no
  verify-then-free guard; FUN_80097F44 calls FUN_80096E88 and
  FUN_800970A8 unconditionally as part of the unload sequence

## Honest assessment

The runtime probing has done its job: we've gone from "Exception
004 at 0x01F8F4FC after 45 seconds of cold boot" to "WinCE
correctly tears down a per-process module mapping and another
context with a stale pointer dereferences the unmapped page".
Further runtime probing will not narrow the upstream cause faster
than reading FUN_8008E56C (the post-unload finalizer) and walking
its callers via Ghidra static analysis.
