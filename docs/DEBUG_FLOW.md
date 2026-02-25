# BE-300 Kernel Boot Debug Flow

This document captures the practical debug loop used to move boot forward on this project.

## Scope

- Target: boot `linux4be20040908/vmlinux` further in `be300`.
- Emulator core: `src/machine.c`, `src/bus.c`, `src/hw/*`.
- Reference implementations/docs:
  - `linux4be/linux/arch/mips/vr41xx/common/*`
  - `linux4be/linux/include/asm-mips/vr41xx/vr41xx.h`
  - `../gxe/src/devices/dev_vr41xx.cc`

## Baseline Commands

Build:

```bash
PKG_CONFIG_PATH=/opt/homebrew/opt/unicorn/lib/pkgconfig make -j4
```

Run (normal):

```bash
./be300 --kernel linux4be20040908/vmlinux > /tmp/be300.out 2> /tmp/be300.err
```

Run (trace):

```bash
./be300 --kernel linux4be20040908/vmlinux --trace > /tmp/be300.trace.out 2> /tmp/be300.trace.err
```

Symbol lookup:

```bash
nm -n linux4be20040908/vmlinux | rg "symbol_name"
```

## Iteration Pattern

1. Run boot without trace.
2. Identify the latest printed kernel line in `/tmp/be300.out`.
3. If crash/hang:
   - Check `/tmp/be300.err` for Unicorn error (`UC_ERR_*`) and PC.
   - If PC known, map it using `nm`.
4. Use short trace runs only when needed to find tight loops or last executed instructions.
5. Cross-check expected hardware behavior in `linux4be` driver code and `../gxe`.
6. Patch the smallest hardware behavior needed.
7. Rebuild and rerun.
8. Repeat.

## What Worked

### 1) Clock/timer bring-up and jiffies progress

- Fixed early blocking at `Calibrating delay loop...` by aligning RTC/ICU behavior:
  - RTC compare/interrupt status semantics.
  - Corrected ICU mask/status interpretation and register offsets.
  - Corrected jiffies physical address used by temporary shim.

### 2) VR41xx register map fixes

- RTC window size increased to include accesses at `0x0F00013E`.
- SYSINT2 mask register offsets corrected (`ICU_MSYSINT2REG`, `ICU_MGIUINTHREG`, `ICU_MFIRINTREG`).

### 3) CPU state/introspection robustness

- Improved register width handling for MIPS64-mode Unicorn interactions.
- Added better fault-state reporting for quicker diagnosis.

## What Did Not Work (and should be avoided/retried carefully)

- Naive syscall bypass shims (patching kernel text or generic syscall skipping) produced regressions:
  - `PC=0` unmapped read crashes, or
  - repeated interrupt floods (`intno=26`) in some cmdline paths.

If syscall path changes are retried, keep them highly targeted and verify against the existing `intr_hook` design in `src/machine.c`.

## Current Debug Focus

- Boot now advances well past delay calibration and deep into init/driver setup.
- Remaining issue appears post-early init: either a real stall or silent progress without more visible console output.
- Next debugging should be checkpoint-style instrumentation around:
  - `do_initcalls`
  - `do_basic_setup`
  - `prepare_namespace`
  - `run_init_process`

Use symbol addresses from `nm` and minimal one-shot logging to avoid perturbing control flow.

## Practical Guardrails

- Keep changes minimal and reversible.
- Prefer hardware-model fixes over kernel-text patching.
- Use trace sparingly (short windows); rely on normal logs for progress checks.
- Revert failed experiments immediately to known-good baseline before next attempt.
