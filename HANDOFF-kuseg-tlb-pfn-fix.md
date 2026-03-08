# Handoff: Kuseg TLB/PFN Mismatch Fix

## What Was Accomplished (This Session)

### Step 1: kseg0 alias read fix (COMMITTED: 70e78ecc)
**Problem:** `tlb_map_kuseg_page()` read from raw PA (0x00000000+), but kernel writes (ramdisk, page cache) went to kseg0 alias (0x80000000+) which is separate Unicorn memory.

**Fix:** Read from kseg0 alias first, fall back to raw PA. Also sync PA from kseg0 data.

**Result:** User page content at 0x2AAA8A00 now has real MIPS instructions (0x04100001 = `bal` or similar) instead of zeros. CPU executes through the first user page. Handoff completes (DROP_DISARM fires).

### Step 2: VR41xx PFN format mismatch (IN PROGRESS, uncommitted)
**Problem:** After the handoff, user-space execution cannot survive a new `uc_emu_start` batch. Each new batch triggers `UC_ERR_READ_UNMAPPED` at the kuseg VA.

**Root cause discovered:** The VR41xx kernel stores PFN in EntryLo such that `PA = PFN << 10`, but Unicorn's MIPS32 hardware TLB computes `PA = PFN << 12`. When `uc_ctl_flush_tlb()` (softmmu flush) fires after TLBWI, QEMU re-walks the MIPS hardware TLB and gets the WRONG physical address.

Example:
- Kernel writes `EntryLo0 = 0x0001601A` (VR41xx format)
- VR41xx PA: `PFN = 0x580`, `PA = 0x580 << 10 = 0x00160000` (correct)
- MIPS32 PA: `PFN = 0x580`, `PA = 0x580 << 12 = 0x00580000` (WRONG)

**Location of the mismatch in code:**
- `src/machine.c` line ~707: `shadow_tlb_lookup` uses `pfn << 10` (VR41xx, correct for our shadow)
- QEMU/Unicorn MIPS hardware TLB uses `pfn << 12` (standard MIPS32, wrong for VR41xx)

## Current State of Code (Uncommitted)

### Changes in `src/machine.c`:

1. **PFN correction at MTC0 time** (~line 2847): When the kernel does `MTC0 $rt, EntryLo0/EntryLo1`, we:
   - Save original value in shadow (VR41xx format)
   - Compute corrected PFN: `(orig_pfn >> 2) << 6 | flags`
   - Write corrected value to GPR $rt (so native MTC0 picks it up)
   - Arm a pending GPR restore (next instruction restores original GPR value)

2. **GPR restore mechanism** (~line 2468): After MTC0 executes, restore the original GPR value so the kernel's subsequent code using $rt gets the right value.

3. **PA sync in tlb_map_kuseg_page** (~line 513): Write kseg0 data to BOTH the kuseg VA flat region AND the raw PA region, so TLB-translated accesses also see correct data.

4. **Removed uc_emu_stop from kuseg ERET** (~line 3433): Instead of stopping the batch on ERET to kuseg, let the native ERET execute and continue in the same batch. This keeps the softmmu cache warm.

### Changes in `src/machine.h`:
- Added `pending_gpr_restore`, `pending_gpr_reg`, `pending_gpr_val` fields

## Current Theory Being Tested

**Hypothesis:** Removing `uc_emu_stop` from the kuseg ERET path will let the CPU continue executing user-space code within the same `uc_emu_start` batch. The softmmu cache stays warm from the `uc_mem_write` calls in `tlb_map_kuseg_page`, so the flat `uc_mem_map` region is used instead of the (broken) hardware TLB.

**What to look for in the test output:**

1. **SUCCESS indicators:**
   - PC advances beyond 0x2AAA8FFC (crosses page boundary to 0x2AAA9000+)
   - New TLB_MAP entries for other kuseg pages (e.g., stack at 0x7FFF7000)
   - User-space syscalls (e.g., `write()`, `open()`)
   - Different error messages (new page faults for unmapped user pages)

2. **FAILURE indicators:**
   - Same `USER_TLBL_INJECT` loop at 0x2AAA8A00
   - `intno=12` flood at PC=0x80001850
   - Crash/hang with no new log output
   - last_pc stuck at same address

3. **Key grep commands:**
   ```bash
   grep 'DROP_DISARM' docker_2.4_stderr.log
   grep 'USER_TLBL_INJECT' docker_2.4_stderr.log
   grep 'INTR.*intno=12' docker_2.4_stderr.log | tail -5
   tail -30 docker_2.4_stderr.log
   tail -10 docker_2.4_stdout.log
   ```

## Next Steps (If Current Test Fails)

### Option A: Force softmmu warm-up before new uc_emu_start
In the `machine_run` loop, when starting at a kuseg PC:
1. Before `uc_emu_start`, write a dummy byte to the kuseg VA via `uc_mem_write` to warm the softmmu cache
2. This might populate the softmmu TLB with the flat region mapping

### Option B: Handle UC_ERR_READ_UNMAPPED for kuseg WITHOUT TLB exception
Instead of injecting USER_TLBL_INJECT, just `continue` to retry. The `uc_mem_map` region exists; maybe QEMU will find it on retry.

### Option C: Patch Unicorn to expose CP0 EntryLo0/EntryLo1
Add `UC_MIPS_REG_CP0_ENTRYLO0` and `UC_MIPS_REG_CP0_ENTRYLO1` to the Unicorn MIPS register enum. Then we can directly read/write these registers and fix the PFN in the hardware TLB.

### Option D: Build custom Unicorn with VR41xx PFN shift
Modify QEMU's MIPS TLB helper (`r4k_fill_tlb`) to use `pfn << 10` instead of `pfn << 12`. This is the cleanest fix but requires building a custom Unicorn library.

### Option E: Execute a trampoline to fix TLB entries
Write a small MIPS code sequence to scratch memory that:
1. `LUI k0, corrected_pfn_hi`
2. `ORI k0, corrected_pfn_lo`
3. `MTC0 k0, EntryLo0`
4. `TLBWI`
5. `JR ra`
Execute this trampoline via `uc_emu_start` after each kuseg TLBWI.

## Next Steps (If Current Test Succeeds)

1. The CPU should hit page boundaries and need new kuseg pages mapped
2. The `USER_TLBL_INJECT` → kernel TLB handler → `tlb_map_kuseg_page` cycle should handle each new page
3. Watch for:
   - Stack page faults (0x7FFF7000 area)
   - ld.so data page faults
   - BSS/heap pages
   - Syscalls from user space
4. The `intno=12` interrupt flooding needs to be addressed (timer/hardware interrupts while in user space)

## 2.6 Kernel Regression Check

Always run:
```bash
timeout 180s ./be300 --kernel ../kernels/vmlinux-2.6 > docker_2.6_stdout.log 2> docker_2.6_stderr.log
```
The 2.6 kernel doesn't reach user space, so the PFN correction and kuseg changes shouldn't affect it. But the PFN correction applies to ALL MTC0 EntryLo writes, which could theoretically affect kseg0 TLB handling during kernel boot.

## Key Files
- `src/machine.c` — main emulator logic, all changes here
- `src/machine.h` — machine struct with new fields
- `kernels/vmlinux-pgui-demo` — 2.4.18 test kernel
- `kernels/vmlinux-2.6` — 2.6 regression kernel

## Key Functions
- `tlb_map_kuseg_page()` (~line 450) — maps kuseg VA pages, reads from kseg0 alias
- `shadow_tlb_lookup()` (~line 627) — finds TLB entries, uses `pfn << 10` (VR41xx)
- `shadow_tlb_populate()` (~line 840) — calls tlb_map_kuseg_page with shadow TLB data
- `cp0_shadow_write()` (~line 56) — tracks MTC0 writes to CP0 registers
- `prid_hook()` — per-instruction hook, handles ERET, TLBWI, MTC0 intercepts
- `intr_hook()` (~line 3571) — interrupt hook, handles execve handoff state machine
- `machine_run()` — main emulation loop, handles UC_ERR_READ_UNMAPPED

## Architecture Notes
- Unicorn maintains SEPARATE memory regions for PA (0x00000000), kseg0 (0x80000000+), and kuseg VAs
- `uc_mem_map` creates flat Unicorn regions, but MIPS kuseg always goes through hardware TLB
- `uc_ctl_flush_tlb` flushes QEMU softmmu cache, NOT the MIPS hardware TLB
- The softmmu cache appears to be implicitly flushed between `uc_emu_start` calls
- Within a single `uc_emu_start` batch, flat regions DO work for kuseg (softmmu caches them)
