# BE-300 Emulator Boot Status and Next Steps

## Current Status

The emulator now boots significantly further than the original `Calibrating delay loop...` stop.

### Reached Output (current baseline)

Boot progresses through:

- kernel banner and CPU/clock detection
- memory/zones/cache setup
- `Calibrating delay loop... 99.84 BogoMIPS`
- initrd detection/free
- framebuffer init (`fb0: Casio BE-x00 frame buffer device`)
- RAMDISK/PPP/NFTL init
- `NET: Registered protocol family 2`

This is a major improvement from the prior hard stop at delay calibration.

## Key Fixes Already Landed

- RTC/ICU behavior aligned with VR41xx Linux expectations:
  - status/mask semantics
  - compare/interrupt handling
  - register offset corrections
- RTC MMIO range expanded to include required addresses.
- Early jiffies progression workaround corrected (`jiffies` PA fix).
- Additional MIPS64 robustness and diagnostics in CPU/machine paths.

## Known Unstable Area

- The syscall/interrupt handling path in `src/machine.c` is sensitive.
- Broad syscall bypass attempts caused regressions (`PC=0` faults or `intno=26` floods).
- Command line forcing (`console=ttyS0,115200 init=/linuxrc`) currently exposes this instability; baseline no-cmdline path is more reliable for now.

## Most Likely Remaining Blocker

Post-early init progression is not yet clearly visible:

- Could be running silently with no additional console output.
- Could be stalled later in init/userspace handoff.

Need precise execution checkpoints around late init calls and `run_init_process`.

## Recommended Next Steps (Priority Order)

1. Add one-shot checkpoint logging (non-trace) for:
   - `do_initcalls`
   - `do_basic_setup`
   - `prepare_namespace`
   - `run_init_process`
   - `do_execve`
2. Run baseline boot and confirm which checkpoints are hit.
3. If `run_init_process` is reached:
   - inspect initrd userspace expectations (`/linuxrc`, `/sbin/init`, etc.).
4. If checkpoints stop before userspace handoff:
   - capture short trace around final checkpoint and map PCs via `nm`.
5. Only then consider targeted syscall/exception-path adjustments; avoid generic bypasses.

## Suggested Verification Command Set

```bash
PKG_CONFIG_PATH=/opt/homebrew/opt/unicorn/lib/pkgconfig make -j4
./be300 --kernel linux4be20040908/vmlinux > /tmp/be300.out 2> /tmp/be300.err
tail -n 200 /tmp/be300.out
tail -n 200 /tmp/be300.err
```

For focused trace:

```bash
./be300 --kernel linux4be20040908/vmlinux --trace > /tmp/be300.trace.out 2> /tmp/be300.trace.err
tail -n 300 /tmp/be300.trace.err
```

## Handoff Notes

- Prefer continuing from current baseline behavior without cmdline overrides.
- Keep experiments isolated and reversible.
- If a test destabilizes boot, revert immediately before new experiments.
