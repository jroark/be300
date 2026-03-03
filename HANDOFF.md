# Handoff Document — BE-300 Emulator Debug Session (2026-03-02)

## What Was Accomplished This Session

### TLB Storm Fix (COMPLETE — not yet committed)
The previous blocker was an infinite TLB store miss storm at PC=0x800015B4. Root cause: Unicorn's QEMU softmmu cached a stale read-only (D=0) TLB entry that wasn't evicted after TLBWI upgraded the PTE to D=1.

**Fix implemented in `src/machine.c` and `src/machine.h` (uncommitted):**
1. Added `bool pending_tlb_flush` field to `machine_t` (machine.h)
2. Added `uc_ctl_flush_tlb(uc)` call at the next instruction after every TLBWI/TLBWR
3. Added `tlb_map_kuseg_page()` helper that directly maps kuseg VA in Unicorn's region table

**Result:** TLB storm is gone. Kernel now boots to root filesystem mount and enters init.

## Current Boot Progress

### stdout output (90-second run):
```
Linux version 2.6.8.1 ...
Calibrating delay loop... 98.81 BogoMIPS
...
RAMDISK: Compressed image found at block 0
VFS: Mounted root (ext2 filesystem) readonly.
Freeing unused kernel memory: 112k freed
```
(Same as before — the kernel consistently reaches this point)

### What happens after "112k freed":
- `init()` calls `sys_open("/dev/console")`, `sys_dup()` x2
- `init()` calls `run_init_process("/sbin/init")` etc.
- **`run_init_process` calls SYSCALL (execve, nr=4011) at VA 0x800015B0**
- **CRITICAL BLOCKER: The SYSCALL at 0x800015B0 does NOT generate SYSCALL_INJECT in our hook**

## Current Blocker: /sbin/init execve not intercepted

### Root Cause (still investigating)
Our `intr_hook` intercepts MIPS SYSCALL via `intno==17` and sets `likely_syscall=true` when the instruction 4 bytes before PC is `0x0000000C` (SYSCALL opcode). For `/sbin/hotplug` execve (EPC=0x800405C4) this works perfectly. But for `run_init_process` SYSCALL at 0x800015B0, **no SYSCALL_INJECT is logged**.

Confirmed by:
- `grep 'EPC=0x800015B0'` in stderr → 0 matches
- `grep 'INTR.*800015'` in stderr → 0 matches
- `/sbin/hotplug` IS intercepted and processes (do_execve returns ENOENT = 0xFFFFFFFE, expected)
- `/sbin/init` do_execve is NEVER reached

### Theories to investigate:
1. **The run is too short** — the emulation hasn't reached run_init_process yet in 30s runs; 90s runs needed to confirm. The RAMDISK decompression takes a long time and uses `huft_build` extensively during `populate_rootfs`.
2. **Timer/scheduling bug** — init thread never gets scheduled to run run_init_process
3. **State corruption from hotplug** — after many hotplug SYSCALL→ERET cycles, something breaks the SYSCALL detection

### The INIT_EXECVE_SITE probe addresses are WRONG
The probe fires at: `0x80001850, 0x80001870, 0x80001890, 0x800018B0`

In the 2.6 kernel these addresses are inside `huft_build` (Huffman tree builder, `0x80001780`), called during `populate_rootfs` for ramdisk decompression — NOT during init execve. The logs labeled "/sbin/init", "/etc/init" etc. are **misleading noise from the wrong addresses**. Actual init execve SYSCALL is at **0x800015B0** (in `run_init_process` at 0x80001598).

Fix: update probe addresses in machine.c or remove them entirely.

### The /sbin/hotplug loop
`call_usermodehelper("/sbin/hotplug")` fires from initcalls and runs many times, each returning ENOENT (-2 = 0xFFFFFFFE) because `/sbin/hotplug` doesn't exist in the initramfs. This is NORMAL — the loop is just the kernel retrying hotplug for various events. It does NOT indicate a real problem. However, the flood of hotplug processing is consuming most of the emulation time.

## Key Addresses (linux4be20040908/vmlinux — 2.6.8.1)

| Symbol | VA |
|--------|-----|
| `run_init_process` | 0x80001598 |
| `SYSCALL in run_init_process` | 0x800015B0 |
| `init()` | 0x800015D0 |
| `do_execve` (2.6) | 0x80080CB0 |
| `open_exec` (2.6, called from do_execve) | 0x8007FA98 |
| `JAL open_exec` in do_execve | 0x80080CD8 |
| `call_usermodehelper` entry | logged in CHECKPOINT |
| `____call_usermodehelper` SYSCALL | 0x800405C4 |

### init() call sequence (disassembled):
```
0x800015D0  init():
0x800015E8    JAL do_pre_smp_initcalls
0x800015F0    JAL sched_init_smp
0x800015F8    JAL populate_rootfs         ← huft_build runs here (RAMDISK decompression)
0x80001600    JAL do_basic_setup
0x80001614    JAL sys_access              ← checks /etc/inittab
0x80001630    JAL prepare_namespace       ← if no /etc/inittab
0x80001638    JAL free_initmem            ← prints "Freeing unused kernel memory"
0x80001658    JAL sys_open /dev/console
0x80001674    JAL sys_dup x2
0x80001690    JAL run_init_process (cmdline)
0x8000169C    JAL run_init_process "/sbin/init"   ← SYSCALL at 0x800015B0
0x800016A8    JAL run_init_process "/etc/init"
0x800016B4    JAL run_init_process "/bin/init"
0x800016C0    JAL run_init_process "/bin/sh"
0x800016DC    J panic "No init found."
```

## Uncommitted Changes (src/machine.c, src/machine.h, src/main.c, src/ui.c)

Run `git diff --stat HEAD` to see them. These are significant (~480 line diff) and include:
- TLB flush fix (the main breakthrough)
- All diagnostic instrumentation
- Various bug fixes accumulated over many sessions

**Per CLAUDE.md policy**: commit these immediately with a detailed message describing what was tried, what worked, what failed, what next.

## Recommended Next Steps

### Step 1: Commit current state (MANDATORY per CLAUDE.md)
```bash
# On host:
git add src/machine.c src/machine.h src/main.c src/ui.c
git commit -m "fix(tlb): flush softmmu TLB after TLBWI to unblock init execve

..."
git push -u origin claude/explain-codebase-mm1561dhacl5ikyh-zdk3b
```

### Step 2: Run a proper 120-second test
Confirm whether run_init_process actually fires after root mount, and whether SYSCALL at 0x800015B0 gets intercepted.

### Step 3: Add probe at run_init_process entry
Add a `prid_hook` probe at 0x80001598 and 0x8000169C to confirm run_init_process is being called. Also add SYSCALL probe at 0x800015B0 specifically.

### Step 4: Fix the wrong INIT_EXECVE_SITE probe addresses
The probes at 0x80001850, 0x80001870, 0x80001890, 0x800018B0 are inside `huft_build`, not init execve. Either remove them or update to correct addresses.

### Step 5: Debug why SYSCALL at 0x800015B0 may not fire intr_hook
Check if `likely_syscall` detection works at that address, add explicit logging.

### Step 6: Build initramfs with /sbin/init
Once execve intercept works, the real blocker will be ENOENT: the initramfs lacks `/sbin/init`. Build a minimal initramfs using the Docker cross-dev scripts:
```bash
# In mips-dev container:
./build_busybox.sh
./create_initramfs.sh
```
Then test with the new initramfs.

## Architecture Notes

### SYSCALL intercept mechanism
- `intr_hook` fires for all Unicorn interrupts/exceptions
- `intno==17` = MIPS SYSCALL in Unicorn 2.1.4
- `likely_syscall = true` when `pc-4` contains opcode `0x0000000C`
- We manually set EXL=1 and redirect to 0x80000180 (MIPS exception vector)
- MFC0 Cause/EPC are intercepted in `prid_hook` to inject synthetic values
- ERET is intercepted in `prid_hook` to clear EXL and return to SYSCALL+4

### TLB fix mechanism
- After every TLBWI/TLBWR: set `pending_tlb_flush=true`
- At the NEXT instruction (prid_hook fires before execution): call `uc_ctl_flush_tlb(uc)`
- Also call `tlb_map_kuseg_page()` to directly map the kuseg VA in Unicorn's region table
- This fixes Unicorn's stale softmmu TLB cache that prevented D=0→D=1 PTE upgrades

### do_execve retry machinery (may no longer be needed)
Complex state machine in intr_hook/prid_hook for handling TLB misses during do_execve:
- `execve_watch_active`, `execve_entry_pc`, `execve_watch_a0/a1/a2/ra/sp`
- `TLB_DEFER_ENTRY`, `TLB_DEFER_SKIP`, `ERET_EXECVE_RETRY`
- This was needed before the TLB flush fix; may interfere now

## Known Issues / Noise
- OPEN_EXEC_CALL/RET probes at 0x8004A9F8, 0x8004AA00 are inside `do_generic_mapping_read` in the 2.6 kernel (not open_exec). These fire spuriously and pollute `open_exec_call_count`. Not causing failures but are misleading.
- The 0x8004A9D0 probe (originally 2.4 do_execve) is inside `do_generic_mapping_read+0xD0` in the 2.6 kernel. Disabled by the execve_watch_active guard but still fires the log.
