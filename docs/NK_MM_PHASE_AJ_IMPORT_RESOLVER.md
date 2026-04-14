# Phase AJ: import resolver path is the proximate trigger

Date: 2026-04-13

Continues `docs/NK_MM_PHASE_AI_LOADER_PATH.md`. Decompiled the
remaining loader functions; the proximate failure pattern is now
named.

## `FUN_800907C8` — recursive import resolver

```c
int FUN_800907c8(int module) {
    int base = *(module + 0x54);                   // load base VA
    if (*(module + 0x98) == 0) return 1;            // no imports
    int *imports = (int *)(base + *(module + 0x94));
    if (*imports == 0) return 1;                    // empty
    
    int name_off = imports[3];
    while (true) {
        FUN_8008fdb4(buf, base + name_off, 0x20);    // copy name string
        int dep = FUN_8008fd3c(buf);                  // find-or-fail dependency
        if (dep == 0) break;                          // → return 0 (failure)
        
        if ((dep->c0 & visit_bit) == 0) {
            dep->c0 |= visit_bit;
            if (dep->per_proc_refcount == 0) {
                int ok = FUN_800905b0(dep);          // attach to current process
                if (ok == 0) return 0;                // → unload chain
            }
            // resolve symbols, recurse on dep's deps
        }
        ...
    }
    return 0;  // last fallthrough is failure
}
```

Returns 0 if ANY import fails to find a dependency, OR if any
attach fails. Caller in FUN_800927CC:

```c
if (FUN_800907c8(coredll) == 0) {
    FUN_800903bc(coredll, 0);    // unload first
    *(error_field) = 0xE;          // THEN set NOT_ENOUGH_MEMORY
}
```

## `FUN_8008FD3C` — find-already-loaded module

```c
int FUN_8008fd3c(ushort *name) {
    short hash = FUN_8008e844(*name);
    int mod = *(_DAT_FFFFDB24);          // ★ MODULE LIST HEAD
    if (mod == 0) return 0;               // ★ empty list → fail
    while (true) {
        ushort *p = *(mod + 8);
        if (*p == hash && string_match(p, name) == 0)
            return mod;                   // found
        mod = *(mod + 4);                  // next
        if (mod == 0) return 0;
    }
}
```

This **only finds existing modules**; it does NOT load missing ones.
If `_DAT_FFFFDB24` is null or doesn't contain a module coredll
imports from, FUN_8008FD3C returns 0, FUN_800907C8 returns 0,
FUN_800927CC unloads coredll, walker fires.

## `FUN_800905B0` — per-process attach

```c
int FUN_800905b0(int module) {
    int slot_offset = cur_thread->c - _DAT_806698ec;
    int ok = FUN_80097844(module->54 + slot_offset, module->80, 0x1002000, 1);
    if (ok == 0) return 0;
    
    for (each section in module->bc) {
        if (section needs install) {
            int ok2 = FUN_80097844(section_addr, section->size, 0x1000, 0x40);
            if (ok2 != 0) FUN_800a68b0(section_addr, section->target, section->size);
            int ok3 = FUN_80098180(section_addr, section->target, section->size, section->flags);
            if (ok3 == 0) {
                // ROLLBACK pair - matches FUN_8008FE8C pattern
                FUN_80097f44(module->54 + slot_offset, module->80, 0x4000);
                FUN_80097f44(module->54 + slot_offset, 0, 0x8000);
                return 0;
            }
        }
    }
    FUN_8007ea74(2);
    return 1;
}
```

This is "attach module to current process slot": maps the main
range, then walks sections installing each. On any section install
failure, calls the FUN_80097F44 tear-down pair (same pattern as
FUN_8008FE8C) and returns 0.

## Key insight

There are TWO independent code paths that produce the FUN_80097F44
tear-down pair pattern we have been chasing:

1. `FUN_8008FE8C(module)` — called from the loader's main rollback
   chain (`FUN_800927CC → FUN_800903BC → FUN_80090144 →
   FUN_8008FE8C`). This is the path our stack walk pinned via
   saved_ra=`0x8009040C` inside `FUN_800903BC`.

2. `FUN_800905B0(dep)` rollback — called inline when a per-process
   attach fails. This is a DIFFERENT call stack that happens to
   produce the same teardown pattern.

Both paths funnel into `FUN_80097F44` and ultimately the walker.
The runtime stack walk via saved_ra confirms we are in path #1
for our cold-boot run, NOT path #2.

## The single-word check that resolves Phase AC vs alternatives

The three candidate triggers in FUN_800927CC are now precisely:

1. **`FUN_800907C8(coredll) == 0`** — an import lookup failed. The
   most likely sub-cause is `_DAT_FFFFDB24 == 0` (empty module list
   when coredll's imports resolve). Sets error `0x0E` AFTER the
   unload returns.

2. **`(*SUB_ffff9bf6)(...) == 0`** — kernel callback hook returns 0.
   Does NOT set any error code. CLAUDE.md does not document this
   address; Ghidra reports "no function found".

3. **`FUN_8008FF00(coredll, 1, 0) == 0`** — DllMain returns 0. Sets
   error `0x45A` AFTER the unload returns.

Phase AK can resolve which one fires with **one runtime read**:

- Read `*(0xFFFFDB24)` at walker entry time. If it's `0`, candidate
  #1 (FUN_800907C8 import failure) is confirmed AND the sub-cause
  is "module list empty".
- If `*(0xFFFFDB24) != 0`, walk the list and dump module names. If
  none of them match what coredll imports, candidate #1 is still
  the trigger but the sub-cause is "specific module missing".
- If the list looks complete, the trigger is candidate #2 or #3
  and we need to read `*(0xFFFF9BF6 - 4)` (the actual call target
  the trampoline jumps to) to see what's at the kernel hook.

## Phase AK proposal

**One runtime read**: `load_va_word(m, 0xFFFFDB24, &mod_list_head)`
inside the existing walker probe. Print `mod_list_head` and, if
non-zero, walk up to 16 entries via `*(mod + 4)` (next pointer)
and dump each module's name string at `*(mod + 8)`.

That single piece of evidence will resolve which of the three
candidates fired. From there the fix target is at most one more
phase away.

## Status after Phase AJ

- The unload chain is fully attributed to FUN_800927CC's loader
  rollback path
- The 3-way candidate is reduced to a single concrete probe
- Both runtime probing AND Ghidra static analysis are converged
- The fix is one targeted runtime read away from being identified
