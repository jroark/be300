# BE-300 Emulator Boot Status and Next Steps

## Current Status (2026-02-25)

The emulator boots through early kernel init up to and including
`NET: Registered protocol family 2` (inet_init).  Checkpoint
instrumentation has now been added to `src/machine.c` to precisely
pinpoint where execution goes silent after inet_init.

### Baseline Boot Output (last confirmed)

```
kernel banner / CPU / clock detection
memory / zones / cache setup
Calibrating delay loop... 99.84 BogoMIPS
initrd detection / free
fb0: Casio BE-x00 frame buffer device
RAMDISK / PPP / NFTL init
NET: Registered protocol family 2
```

---

## What Was Done in This Session

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

1. **Build and run** with the new checkpoint instrumentation:
   ```bash
   make -j4
   ./be300 --kernel linux4be20040908/vmlinux > /tmp/be300.out 2> /tmp/be300.err
   grep -E '(CHECKPOINT|INITCALL|PROGRESS)' /tmp/be300.err | head -80
   tail -n 50 /tmp/be300.out
   ```

2. **Analyse checkpoint output**:
   - If `do_basic_setup` / `do_initcalls` appear → early init is running; map
     `[INITCALL]` function pointers via `nm linux4be20040908/vmlinux` to see
     which initcall stalls.
   - If `prepare_namespace` appears → VFS mount path is reached; check for
     mount errors in stdout.
   - If `run_init_process` appears → note the printed string (which `/init`
     path is tried); investigate initrd layout.
   - If `do_execve` appears → kernel is handing off to userspace; look for
     ELF load / segment-fault issues.
   - If no checkpoints fire after `NET: Registered protocol family 2` →
     execution is looping or stalling inside inet_init sub-functions; check
     `[PROGRESS]` PC samples against `nm` output.

3. **Fix the identified blocker** based on checkpoint evidence.

4. **If stall is inside an initcall sub-function**:
   - Narrow with a short `--trace` run around the stall PC.
   - Map PC range to source via `nm` / objdump.
   - Only then consider targeted exception/syscall-path adjustments.

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
