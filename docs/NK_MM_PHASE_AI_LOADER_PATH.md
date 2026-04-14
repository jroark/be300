# Phase AI: it's the loader path, not process exit

Date: 2026-04-13

Continues `docs/NK_MM_PHASE_AH_REASSESS.md`. Same Ghidra session,
deeper xref walk on `FUN_8008E56C`, `FUN_80090380`, `FUN_80093F5C`,
`FUN_8008FDF8`, `FUN_8008E724`. The result re-opens Phase AC.

## Functions decoded

### `FUN_8008FDF8` — release dependent imports
```c
void FUN_8008fdf8(int module) {
    int *imports_list = module->54 + module->94;  // imports table
    if (*imports_list != 0) {
        do {
            FUN_8008fdb4(buf, ..., 0x20);
            int dep_module = FUN_8008fd3c(buf);
            if (dep_module != 0)
                FUN_80090144(dep_module, 1);   // dereference each import
        } while (more);
    }
    return 1;
}
```
Walks `module + 0x94` (imports table) and calls `FUN_80090144` on each
imported module to drop the per-process refcount.

### `FUN_8008E724` — `FreeLibraryByName`
```c
void FUN_8008e724(byte *name) {
    int mod = *(_DAT_FFFFDB24);          // module list head
    while (mod != 0) {
        ushort *p = *(mod + 8);           // module name
        if (string_match(name, p)) break;
        mod = *(mod + 4);                 // next
    }
    if (mod) FUN_80090144(mod, 1);
}
```
Linear search through the loaded-modules linked list at
`_DAT_FFFFDB24`, calls `FUN_80090144` on the match.

### `FUN_8008E56C` — per-DLL final unmap
```c
void FUN_8008e56c(int module) {
    FUN_8008e3cc(module);                                        // misc cleanup
    FUN_80097f44(*(module+0x54), *(module+0x80), 0x80004000);    // unmap base+size, flag
    // sanity checks on slot ranges...
    FUN_80097f44(va, 0, 0x8000);                                  // unmap with 0x8000
    FUN_8008e464(module + 100);                                   // free aux
    FUN_800a11b0(module, 1);                                      // pool free
}
```
Called from:
- `FUN_80090380` (the tail of `FUN_80090144`'s full-unload path that
  Ghidra split off because of frame size)
- `FUN_80093F5C` (the forced process-exit clear)

### `FUN_80090380` — full-unload tail of FUN_80090144
```c
while (*(s0 + 0x14) != 0) {
    FUN_8008e724(*(s1 + 0x54) + *(s0 + 0x20));   // FreeLibraryByName each export
    s0 += 0x14;
}
FUN_8008e56c(s1);                                 // final unmap
return 1;
```
This is reached via fall-through from `FUN_80090144`. Ghidra split
the function at `0x80090380` because the body crosses the auto-
analysis frame boundary.

### `FUN_80093F5C` — forced per-process clear (NOT our path)
```c
void FUN_80093f5c(int module, byte *proc_slot) {
    int *refcount_ptr = module + (*proc_slot * 2) + 0x14;
    if (*refcount_ptr != 0) {
        *refcount_ptr = 0;                                 // FORCE refcount to 0
        *(module + 0xC) &= ~(1 << (*proc_slot & 0x1F));    // clear inUse bit
        if (_DAT_80668c80 != 0)
            (*(code *)&SUB_ffff9bf2)(proc_slot, module);   // notification
        if (*(module + 0xC) == 0) {                        // global inUse = 0
            (*_DAT_8066007c)(*(module+0x54)+1, 1);
            FUN_8008e56c(module);                          // full unmap
        }
    }
}
```
Has only DATA xrefs (`0x800B9D78`, `0x800B9D88`) — reached via
function-pointer dispatch. Probably the "process exit" hook NK
installs to clean up modules when a process terminates.

## Why this matters for our investigation

The walker's saved-ra at `sp+0x3C = 0x8009040C` puts us inside
`FUN_800903BC` (right after its `jal FUN_80090144` at `0x80090404`).
`FUN_800903BC` is the recursion-guarded wrapper, called from
`FUN_800927CC` and `FUN_800929CC` — both in the **DLL loader**, NOT
in the process-exit hook.

So **Phase AG's "filesys.exe is exiting" interpretation is wrong**.
Slot 1 is the active process slot during the DLL load, but the
unload is being driven by the loader rolling back a failed load,
not by process termination.

## Phase AC re-opened

Phase AC claimed "coredll's DllMain returned FALSE → DLL_INIT_FAILED
→ rollback". Phase AE killed it because kernel error at walker entry
was `0`. But re-reading `FUN_800927CC`:

```c
if ((iVar4 = FUN_8008ff00(unaff_s2, 1, 0), iVar4 == 0)) {
    iVar1 = 0;
    FUN_800903bc(unaff_s2, 0);                         // ① unload first
    *(unaff_s3->2c0->38) = 0x45a;                       // ② THEN set error
}
```

The error is set AFTER `FUN_800903BC` returns. At walker-entry time
(deep inside `FUN_800903BC`), the error has not been written yet,
so reading `0` is **exactly what we'd see for the DllMain-failed
path**. Phase AE's probe was checking the wrong moment.

**Phase AC's hypothesis is back on the table.**

## Three candidate triggers in FUN_800927CC

All three reach `FUN_800903BC` with `error = 0` at walker-entry time:

1. **`FUN_800907C8(coredll) == 0`** — early init step fails. Sets
   error `0x0E` (NOT_ENOUGH_MEMORY) AFTER unload returns.

2. **`(*SUB_ffff9bf6)(parent_thread, coredll, ...)  == 0`** — the
   kernel hook at kseg3 `0xFFFF9BF6` returns 0. This path does NOT
   set any error code at all. **Most interesting candidate**: if
   our emulator hasn't installed this hook (or installs a stub that
   returns 0), we'd see exactly the observed behavior.

3. **`FUN_8008FF00(coredll, 1, 0) == 0`** — DllMain returns 0. Sets
   error `0x45A` (DLL_INIT_FAILED) AFTER unload returns.

## What the hook at kseg3 0xFFFF9BF6 might be

CLAUDE.md documents the kseg3 region:
```
0xFFFFD000-0xFFFFD8BC   per-process structures
0xFFFFD8C0+             refill table base
0xFFFFDB00-0xFFFFDB1C   db state
0xFFFFDAC0              ctx_ptr
```

`0xFFFF9BF6` is NOT in the documented MM region. It is in the
`0xFFFF8000..0xFFFFBFFF` range, which on WinCE typically holds
**callback trampolines that bridge user-mode callbacks back into
kernel mode**. NK installs these during boot.

The exact callback at `0xFFFF9BF6` is unclear without OAL source.
But the call signature
`(arg1=parent_thread_ctx, arg2=module_desc, arg3=stack_arg)` looks
like a **"process attach notification hook"** — a callback that
NK fires when a module is attached to a process so the OAL or
debugger can intercept it.

## Phase AJ target

Two complementary probes (both runtime, but minimal):

1. **Read the FUN_800927CC frame's `iVar4` value** at the moment
   the walker fires. We need to know which of the 3 candidates
   returned 0. The frame is one level above FUN_800903BC, which
   is one level above FUN_80090144, which is two levels above the
   walker. Walk up the stack one more frame and read the local
   that holds the failure check return.

2. **Read `*(0xFFFF9BF6 - 0)` and the bytes around it.** If the
   hook is null, the call would crash before reaching the
   `iVar4 == 0` branch — so the hook IS installed. But we should
   confirm what's there.

## Stable claims after Phase AI

- The unload is from the LOADER path (FUN_800927CC →
  FUN_800903BC → FUN_80090144), NOT from a process exit hook.
- The DLL being unloaded is coredll.dll.
- The trigger is one of three failures in FUN_800927CC, all of
  which leave kernel error = 0 at walker-entry time.
- The strongest candidate is the `(*SUB_ffff9bf6)` kernel hook at
  kseg3 `0xFFFF9BF6` returning 0, because it is the only one of
  the three that never sets an error code.
- Phase AG's "filesys.exe process exit" interpretation is wrong.

## Honest meta

The investigation started with a single PTE zero stream and ended
40 phases later with a 3-way branch on which loader-failure check
fired. We have a clean static call graph, a clean runtime stack
walk that pins the return path through it, and the actual fix is
within 1-2 phases of being identifiable. The remaining work is
Ghidra-only or one targeted runtime read of `iVar4` from the
loader's stack frame.
