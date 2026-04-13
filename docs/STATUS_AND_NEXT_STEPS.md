# BE-300 Emulator Boot Status and Next Steps

This file is historical Linux-kernel status. For the current WinCE 3.0
cold-boot work, start with
`docs/WINCE_COLD_BOOT_SESSION_2026-04-12.md` and then
`docs/WINCE_COLD_BOOT_SESSION_2026-04-08.md`.

## Current Status (2026-02-26)

The emulator now boots far beyond inet initcalls and reaches root mount
plus `/sbin/init` handoff.

### Baseline Boot Output (last confirmed)

```
kernel banner / CPU / clock detection
memory / zones / cache setup
Calibrating delay loop... 99.84 BogoMIPS
initrd detection / free
fb0: Casio BE-x00 frame buffer device
RAMDISK / PPP / NFTL init
NET: Registered protocol family 2
NET: Registered protocol family 17
RAMDISK: Compressed image found at block 0
VFS: Mounted root (ext2 filesystem) readonly.
Freeing unused kernel memory: 112k freed
```

### Current blocker signature

After `run_init_process("/sbin/init")`, Unicorn stops with:

```
[MACHINE] uc_emu_start error at PC=0xFFFFFFFF800A74E0: Invalid memory write (UC_ERR_WRITE_UNMAPPED)
[MACHINE] pending_excode=8 pending_cause=0x00000020 pending_epc=0xFFFFFFFF800015B0
```

Nearest symbol for `0x800A74E0`: `create_elf_tables`.

Additional 2026-02-26 evidence from current cycle:

- With stale synthetic-exception cleanup enabled, `pending_excode` no longer
  remains latched at crash time.
- Failure persists as repeated `UC_ERR_WRITE_UNMAPPED` around:
  - `0x800A74E8`..`0x800A7510` (`create_elf_tables` range),
  - followed by stop at `0x800015B4`.
- Recovery probe mapped candidate blocks:
  - `0x7FF00000` (from `$v0/$a0` around `0x7FFF7F4x`),
  - `0x2AA00000` (from `$t2=0x2AAA8000`),
  but write-unmapped still recurred.
- `UC_HOOK_MEM_*` fault callbacks did not receive this failure, indicating the
  failing path is likely inside Unicorn/QEMU MMU handling rather than our bus
  unmapped callback path.

---

## What Was Done in This Session

### IRQ cause staging fix (critical progress)

- In `src/machine.c` (`prid_hook`), IRQ `Cause` injection was extended to all
  observed reads in the VR41xx interrupt path:
  - `0x80000180` (vector),
  - `0x80007700`,
  - `0x800077A8`.
- `pending_cause_served` now flips true only after `0x800077A8`.
- This made IRQ dispatch reliable (`irq_dispatch`/`do_IRQ`/`do_timer`) and
  unblocked RCU callbacks (`rcu_check_callbacks`/`rcu_process_callbacks`),
  allowing initcalls to complete past inet.

### Additional kernel sweep (`kernels/`)

- Tested:
  - `kernels/vmlinux`
  - `kernels/vmlinux-mw`
  - `kernels/vmlinux-pgui-demo`
  - `kernels/vmlinux-pgui-test1`
  - `kernels/vmlinux_sdlregtest`
- All five currently stall in/near `calibrate_delay` (steady PCs near that
  symbol), including runs with `--cmdline 'lpj=1000000'`.
- Current best debug target remains `linux4be20040908/vmlinux`.

### Checkpoint instrumentation (`src/machine.c`)

Added two new hook types that fire without perturbing execution:

1. **One-shot checkpoint hooks** (`checkpoint_table[]`)
   - 30+ named sites covering:
     - `rest_init`, `do_pre_smp_initcalls`, `do_basic_setup`, `do_initcalls`
     - Fine-grained probes inside `init()` around the `sys_access` / `prepare_namespace` branch
     - `run_init_process` (prints `$a0` string = the init path being exec'd)
     - `do_execve` (prints `$a0` string = executable path)
     - Post-inet_init initcalls: `af_unix_init`, `packet_init`
     - inet_init sub-functions: `ip_init`, `tcp_init`, `arp_init`, `ip_rt_init`,
       `ipfrag_init`, `neigh_table_init`, `alloc_large_system_hash`, `fib_hash_init`
   - Output format: `[CHECKPOINT] >>> <name> <<<`  (with string for `a0` sites)
   - Each fires exactly once; no log flooding.

2. **do_initcalls tracer** at JALR site `0x80272874`
   - Logs the function pointer called for each initcall (up to 64).
   - Output format: `[INITCALL] #NN  fn=0x<ADDR>`

3. **Periodic PC sampler** in `machine_run`
   - Logs `PC` every 100 batches (~10 M instructions) to show where time
     is spent during silent phases.
   - Output format: `[PROGRESS] insns=<N>M  PC=0x<ADDR>`

All addresses are for `linux4be20040908/vmlinux` (verified via `nm`).

---

## Key Fixes Already Landed (prior sessions)

- RTC/ICU behavior aligned with VR41xx Linux expectations
  (status/mask semantics, compare/interrupt handling, register offsets).
- RTC MMIO range expanded to include required addresses.
- Early jiffies progression workaround corrected (`jiffies` PA fix).
- MIPS SYSCALL exception handled via `UC_HOOK_INTR` intercept.
- MIPS64 CPU state robustness and diagnostics improvements.
- `.gitignore` extended to exclude boot log artifacts and kernel tree.

---

## Known Constraints

- The syscall/interrupt path is sensitive; broad bypass attempts caused
  regressions (`PC=0` faults, `intno=26` floods).
- Avoid forcing `init=/linuxrc` or `console=ttyS0` via cmdline until the
  silent-boot cause is confirmed; the no-cmdline baseline is more stable.

---

## Next Steps (Priority Order)

1. **Focus on `/sbin/init` exec path crash** (new top priority):
   ```bash
   make -j4
   ./be300 --kernel linux4be20040908/vmlinux > /tmp/be300.out 2> /tmp/be300.err
   rg -n '(run_init_process|do_execve|uc_emu_start error|pending_excode|SYSCALL_INJECT|IRQ_GATE)' /tmp/be300.err
   ```

2. **Instrument syscall exception lifetime near execve**:
   - Add targeted logs for syscall-context `mfc0 Cause`, `mfc0 EPC`,
     `mtc0 EPC`, and `ERET` events near the crash window.
   - Verify whether nested page-fault exceptions during `create_elf_tables`
     are dispatched with the correct non-syscall `Cause.ExcCode`.

3. **Validate address-space behavior during `create_elf_tables`**:
   - Capture faulting virtual write target (`$a0/$v1` etc.) and map to expected
     user stack setup.
   - Confirm whether kernel page-fault path runs and returns before the write.

4. **Re-test alternate kernels only after syscall-fault path is fixed**:
   - Their current `calibrate_delay` stall likely shares timer/exception
     assumptions and is lower priority than the now-advanced 2.6.8.1 path.

---

## Verification Command Reference

```bash
# Full boot run with checkpoint output
make -j4 && ./be300 --kernel linux4be20040908/vmlinux \
    > /tmp/be300.out 2> /tmp/be300.err
grep -E '(CHECKPOINT|INITCALL|PROGRESS)' /tmp/be300.err
tail -50 /tmp/be300.out

# Map a PC to a symbol
nm linux4be20040908/vmlinux | grep -i '<symbol>'
addr2line -e linux4be20040908/vmlinux 0x<PC>

# Focused trace around a known stall PC
./be300 --kernel linux4be20040908/vmlinux --trace \
    > /tmp/be300.trace.out 2> /tmp/be300.trace.err
grep '0x<PC>' /tmp/be300.trace.err | head -30
```

---

## Handoff Notes

- All checkpoint addresses are for `linux4be20040908/vmlinux`.  If a
  different kernel image is loaded, re-derive addresses with `nm`.
- Keep experiments isolated; revert immediately if a change destabilises boot.
- Prefer continuing from the no-cmdline baseline.
