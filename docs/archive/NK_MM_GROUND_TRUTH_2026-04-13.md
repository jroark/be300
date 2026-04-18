# NK MM Ground Truth (Phase W, 2026-04-13)

Source: Ghidra decompilation of `build-host/nk_decompressed.bin` via the
restarted Ghidra MCP server. This document supersedes all Phase I..V
causal interpretations.

## The real function names (Ghidra-verified)

| VA           | Ghidra name       | Role                                                            |
|--------------|-------------------|-----------------------------------------------------------------|
| `0x80096CC8` | `FUN_80096CC8`    | Opcode dispatcher. `a0` is opcode, jump-tables at `0x80075714`. |
| `0x80096E88` | `FUN_80096E88`    | **L1 range walker** (install-or-count mode by `param_6 != 0`).  |
| `0x800970A8` | `FUN_800970A8`    | **VA range UNMAP primitive.** Body matches our runtime trace.   |
| `0x80097844` | `FUN_80097844`    | MM op entry: dispatch via `0x80096CC8`, then take lock.         |
| `0x80097CA0` | `FUN_80097CA0`    | **VA range MAP primitive.** Allocates pool-5 L2 groups.         |
| `0x80097F44` | `FUN_80097F44`    | Thin stub. Dispatches `0x8009E11C` or `FUN_800998C0`.           |
| `0x80098180` | `FUN_80098180`    | MM query/verify primitive. 44 kernel callers.                   |
| `0x800998C0` | `FUN_800998C0`    | **Recursive lock acquire** on `0x806697A0`.                     |
| `0x80099924` | `FUN_80099924`    | **Recursive lock release** on `0x806697A0` (symmetric).         |
| `0x800A1134` | `FUN_800A1134`    | Pool allocator (20-entry pool descriptor table at `0x806600B8`).|
| `0x8008D8D0` | `FUN_8008D8D0`    | "Try allocate VA range" — reserve-then-commit pattern.          |

## `FUN_800970A8` (walker) — confirmed behavior

```c
void FUN_800970a8(int a0_L1_base, int a1_target_idx, int a2_offset_in_group,
                  int a3_count, undefined4 param_5, int param_6_force)
{
    FUN_8007ea74(1);                                   // lock/sync level 1
    if (a3_count > 0) {
        piVar4 = (int *)(a0_L1_base + a1_target_idx * 4);
        do {
            int L1_slot = *piVar4;
            if (L1_slot == 1) {
                // already reserved — skip by advancing count
                a3_count += a2_offset_in_group - 0x10;
            }
            else if (/* complicated condition including
                        refcount < 2, partial range, etc. */) {
                if (param_6_force == 0 && *(short *)(L1_slot + 10) != 0) {
                    // skip: some lifetime protection active
                    a3_count += a2_offset_in_group - 0x10;
                }
                else {
                    // ★ THE PTE ZERO + PFN FREE LOOP
                    iVar3 = L1_slot + a2_offset_in_group * 4;
                    uVar2 = *(uint *)(iVar3 + 0xc);
                    _DAT_806694cc = L1_slot;
                    while (1) {
                        if (uVar2 != 0 && uVar2 != 0xffffffc0) {
                            *(iVar3 + 0xc) = 0;         // ★★ matches our
                                                        //    observed zero-store
                            FUN_800a3244(uVar2 & 0x3fffffc0);  // free PFN
                        }
                        a3_count--; a2_offset_in_group++;
                        if (a3_count == 0 || a2_offset_in_group > 0xf) break;
                        uVar2 = *(iVar3 + 0x10);
                        iVar3 += 4;
                    }
                    _DAT_806694cc = 0;
                }
            }
            else {
                // normal unmap: mark slot as "reserved" and decrement refcount
                *piVar4 = 1;                           // ★★★ writes "1" into L1 slot
                a3_count -= 0x10;
                *(char *)(L1_slot + 4) -= 1;
            }
            a2_offset_in_group = 0;
            piVar4++;
        } while (a3_count > 0);
    }
    FUN_8008c564();                                    // TLB/cache flush
    return;
}
```

**This is a legitimate VA-range unmap primitive.** It frees PFNs,
zeros PTE slots, and marks L1 slots as "reserved=1". The `"1"`
markers we saw in L1 slots `0..503` and `511` are written BY this
function during prior teardowns.

## `FUN_80096E88` (L1 range walker) — confirmed behavior

Two modes, selected by `param_6` (NULL vs non-NULL).

### Install mode (`param_6 == NULL`)
For each L1 slot in range:
- If slot == 1 (reserved): call `FUN_800A1134(5)` to allocate a
  fresh 76-byte pool-5 L2 group. If alloc fails, set kernel error 8
  and return `-1`. On success, initialize the group by copying
  a template from `piVar7 = &L1[param_2 * 4]`, mark it allocated,
  and store the new pointer into the L1 slot.
- If slot is a real L2 group: walk and count entries.

**The allocator call IS real** and IS in this function. My Phase R
objdump read at `0x80096F0C` showed `0x0018cac0 = sll` — that was
my **`--adjust-vma` base being wrong**. Ghidra's view is
authoritative.

### Count mode (`param_6 != NULL`)
Walks PTE entries in matching L2 groups and writes the match count
to `*param_6`.

## `FUN_80097CA0` (VA range map primitive) — confirmed behavior

```c
void FUN_80097ca0(int param_1) {
    // Initialize an L2 group's PTE slots to 0xFFFFFFC0 (sentinel)
    ...
    iVar2 = FUN_80096E88(..., uVar3);                  // L1 walker, count mode
    if (iVar2 != -1) {
        iVar2 = FUN_800a2f88(iVar2, 0);
        if (iVar2 != 0) {
            // Allocate PFNs and install PTEs via FUN_800A2F34
            // with retry + 100ms sleeps
            // PTE = allocated_pfn | unaff_s6 (flags)
            FUN_8008C564();                            // TLB flush
            FUN_80099924(0x806697a0);                  // RELEASE LOCK (tail)
        }
        *(kernel_state + 0x38) = 8;                    // set error
    }
    ...
    FUN_80099924(0x806697a0);                          // RELEASE LOCK
}
```

**This is the real map primitive**, called from `0x801D3FA0` inside
`FUN_801D3B44` — a high-level VM allocator.

## What Phase H..V got wrong

1. **The supposed jal at `0x80098100` → `0x80096E88` does not exist
   as code.** Ghidra reports `0x80098100` and `0x8009813C` as "No
   function found". Those bytes are data (probably jump-table or
   reloc info) that happened to decode as valid MIPS instructions.

2. **`FUN_80097F44` is a 3-line stub**, not a 320-byte coordinator.
   Its 44 callers all use it as a thin "dispatch + take lock"
   wrapper. Our "0x80097F80..0x80098200 coordinator body" was
   looking at bytes past the stub.

3. **`FUN_80097CA0` is the real mapper**, but its only caller is
   `0x801D3FA0` in `FUN_801D3B44`. It does not take arguments in
   the shape I thought.

4. **The walker is reached via function-pointer dispatch.** The
   addresses `0x800B9FA8`/`0x800B9FB8` hold the function pointer
   to `FUN_800970A8` but Ghidra sees no reader, so the invocation
   path goes through code Ghidra's static analysis can't follow
   (likely a computed `jalr` from a register loaded by an earlier
   unresolved path).

5. **The `v0=0` we captured at walker entry is NOT from a `jal`
   return.** `FUN_80096E88` always returns 1 on its reachable
   paths. The walker's incoming `v0` is whatever `$v0` happened to
   hold in the caller's context immediately before the indirect
   jalr. `Status EXL=1` at walker entry (Phase D) strongly suggests
   exception-handler context, and `$v0` in that context is not
   derived from the MM chain.

## `FUN_8008D8D0` — the real "coordinator-like" function

8 callers from the `0x801C7xxx..0x801CCxxx` range (process manager /
module loader). It implements a two-phase "reserve then commit" VA
allocation:

```c
iVar1 = FUN_80097844(_DAT_806698ec, param_2, param_3, 1);  // reserve range 1
iVar2 = iVar1 + (stack_arg & 0xffff);
iVar1 = FUN_80097844(iVar2, 0x1000, 0x1000, 0x40);          // reserve range 2
if (iVar1 == 0) {
    FUN_80097f44(iVar1_low, 0, 0x8000);                     // rollback: unreserve
    ...
}
FUN_8007ea74(2);                                            // lock level 2
iVar1 = FUN_80098180(range_ptr, iVar2, 0x1000, descriptor); // install
if (iVar1 != 0) { stack_result = 1; }
FUN_80097f44(iVar2, 0x1000, 0x4000);                        // commit range 2
FUN_80097f44(iVar1_low, 0, 0x8000);                         // release range 1
```

The `a2 = 0x4000` / `a2 = 0x8000` flag dichotomy we observed in
Phase K is now clear: **`0x4000 = commit/normal` and
`0x8000 = rollback/release`**. Phase L's "multi-range coordinator
body" interpretation was wrong because those bytes aren't code.

## What does hold up from Phase A..V

- The walker's observable store `sw $zero, 0x0C($s0)` at PC
  `0x800971BC` is real. Its PC and instruction word are correct.
- The pool-5 object layout (76 bytes, template PTE pattern
  `0x4000xx1A` with high bit set) is real.
- The L1 table at `0x80668CC0` with "1" markers and pool-5 pointers
  is real and its layout is as we dumped in Phase U/V.
- The opcode jump table at `0x80075714` is real — it's
  `FUN_80096CC8`'s dispatch.
- `0x800A1134` really is the pool allocator (Ghidra confirms the
  `FUN_80096E88` call site).
- The cold-boot timeout symptom and the `Exception 004` at
  `0x01F8F4FC` are real.

## Where the investigation actually stands

**We still don't know why the walker runs during cold boot for the
L2 group covering `0x01FE0000..0x01FEFFFF`.** Everything we thought
we knew about the caller chain was misread. The right next steps:

1. Find the real call path: since `FUN_800970A8` is dispatched
   indirectly, someone either loads the function pointer from
   `0x800B9FA8`/`0x800B9FB8` or from a computed table index.
   Static analysis of `FUN_8007EA74` (the first call at walker
   entry) may reveal the wrapper.

2. The true sequence: walk callers of `FUN_8007EA74(1)` — the
   walker's first call. That's a distinct ID and its callers narrow
   the context. There should be a small set (level-1 lock clients).

3. Confirm exception-handler origin: Phase D saw `Status EXL=1`
   at walker entry. If the walker runs inside an exception handler,
   the "coordinator" is really an exception dispatcher, and the
   `v0=0` we captured is incoming from some other CPU state. Look
   at NK's TLB refill handler and general exception vector to see
   whether they ever load function pointers like `0x800B9FA8`.

4. Do NOT add runtime PC probes for this — they all hit the
   dyntrans translation-time cache wall. Use Ghidra static xref
   walking instead.
