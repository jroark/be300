# CLAUDE.md — Working Rules for This Repository

## Commit & Push Policy

**After every attempt — regardless of success or failure — commit and push all changes with a detailed message covering:**
- What was tried
- What the outcome was (success, partial, or failure)
- What was learned or confirmed
- What the next step should be

This applies to: code changes, diagnostic instrumentation, failed experiments, documentation updates, and analysis results.

## Branch
Stay on the current branch, don't create PRs.

Push with: `git push -u origin <current branch>`

## Execution Environment Policy

- Build and test in the Docker container (`mips-dev`) only.
- Commit and push from the host environment only (not from inside Docker).
- Typical split:
  - Container: `cmake`, `make`, emulator/test runs, log generation (The /tmp dir doesn't persist between container invocations.).
    - The local dir is mounted as /work in the container
    - The container has a mips cross developement toolchain for analyzing the kernels and other mips binaries
  - Host: `git add`, `git commit`, `git push`. (host does not have mipsel cross development)

---

## Project Context

**Target:** Casio BE-300 (NEC VR4131 MIPS little-endian) emulator in Unicorn.

**Kernels**
- `linux4be20040908/vmlinux`   — ELF32 MIPS LE, 2.6.8.1, built 2004-09-08.
- `kernels/vmlinux`            - ELF 32-bit LSB executable, MIPS, MIPS-II version 1 (SYSV), statically linked, not stripped, too many notes (256)
  - cmdline: "console=tty0 console=ttyS0,9600 root=/dev/ram"
- `kernels/vmlinux_sdlregtest` - ELF32 MIPS LE, Linux version 2.4.18-mips (mouse@mouse.office.altlinux.ru) (gcc version 3.0.4) #325   20 14:06:02 MSK 2003
  - cmdline: "console=tty0 console=ttyS0,9600 root=/dev/ram"
- `kernels/vmlinux-mw`         - ELF32 MIPS LE, Linux version 2.4.18-mips (jroark@dhcppc4) (gcc version 3.0.1) #309 Sun May 18 03:01:37 PDT 2003
  - cmdline: "console=tty0 console=ttyS0,9600 root=/dev/ram"
- `kernels/vmlinux-pgui-demo`  - ELF32 MIPS LE, Linux version 2.4.18-mips known good kernel and ramdisk (booted to userspace on real hardware)
  - cmdline: "console=tty0 console=ttyS0,9600 root=/dev/ram"
- `kernels/vmlinux-pgui-test1` - ELF32 MIPS LE, Linux version 2.4.18-mips (jroark@dhcppc4) (gcc version 3.0.1) #309 Sun May 18 03:01:37 PDT 2003
  - cmdline: "console=tty0 console=ttyS0,9600 root=/dev/ram"

**Kernel Changes for BE-300**
- `kernels/linux-2.6` - Patch set and overlay for the 2.6 kernel from Dec 21st 2003

**WinCE ELF loader**
- `linux4be20040908/loader.exe` - CyaCE compiled binary
- `linux4be20040908/cyacecfg.txt` - CyaCE config
- `ce/cyace` - source code for CyaCE loader

**Things to note**
- originally the kernels were loaded from a running WinCE (warm start, not cold) - hw may have been initialized by WinCE
- None of the test kernels had full hw support

**References**
- GXemul - git@github.com:bitedits/gxe.git - implements various NEC vr41xx CPUs

---

## Key Symbol Addresses (linux4be20040908/vmlinux)

| Symbol                   | VA         |
|--------------------------|------------|
| `rest_init`              | 0x80001558 |
| `do_basic_setup`         | 0x80272918 |
| `do_initcalls`           | 0x802727d0 |
| `prepare_namespace`      | 0x80273470 |
| `run_init_process`       | 0x80001598 |
| `do_execve`              | 0x80080cb0 |
| `inet_init`              | 0x80285ea0 |
| `inet_register_protosw`  | 0x801974d8 |
| `synchronize_net`        | 0x8014a150 |
| `synchronize_kernel`     | 0x80042a80 |
| `wait_for_completion`    | 0x801ab590 |
| `call_rcu`               | 0x800424a0 |
| `rcu_check_callbacks`    | 0x800428b8 |
| `rcu_process_callbacks`  | 0x80042780 |
| `timer_interrupt`        | 0x8000eb30 |
| `ll_timer_interrupt`     | 0x8000ed88 |
| `do_timer`               | 0x800379a8 |
| `update_process_times`   | 0x80037708 |
| `scheduler_tick`         | 0x80027e08 |
| `jiffies` (PA)           | 0x001cd9e0 |
| `af_unix_init`           | 0x80286440 |
| `packet_init`            | 0x802864d8 |

---

## Verification Commands

```bash
# Build and run tests (HOST)
mkdir -p build-host && cd build-host
cmake .. -DCMAKE_OSX_ARCHITECTURES=arm64  # arm64 for M1/M2/M3 Macs
make -j$(sysctl -n hw.ncpu)
./test_basic

# Run emulator
./be300 --kernel ../linux4be20040908/vmlinux

# Check probe output
grep -E '(RCU_PROBE|CHECKPOINT|INITCALL|PROGRESS)' /tmp/be300.err | head -80

# Symbol lookup
nm linux4be20040908/vmlinux | grep <symbol>

# Disassembly from the container only
mipsel-linux-gnu-objdump -d --start-address=0x<VA> --stop-address=0x<VA+N> linux4be20040908/vmlinux
```

---

## Architecture Notes

- **Interrupt injection:** `inject_hw_irq_if_pending` (machine.c) fires when the
  VR41xx ICU has a pending source. Sets EXL=1, `pending_cause = IP2`, redirects
  PC to 0x80000180. The MFC0 Cause intercept in `prid_hook` returns the injected
  cause so the kernel dispatches correctly.
- **Jiffies hack:** `tick_jiffies_hack` directly writes jiffies PA every batch.
  This kept early boot moving but bypasses `do_timer` / RCU callbacks. May need
  to be removed or rate-limited once the real timer path is confirmed working.
- **SYSCALL handling:** `intr_hook` intercepts intno=17 (SYSCALL) and performs
  manual MIPS exception entry (EPC, EXL, vector redirect).
- **PRId intercept:** `prid_hook` intercepts MFC0 PRId to return VR4131 value
  (0x0C80) so the kernel identifies the platform correctly.

---

## Development Workflow & Tooling

1. **Work in Docker (Linux toolchain + Unicorn build):**
   ```bash
   # Build/update the container image
   docker compose build mips-dev

   # Drop into the dev shell
   docker compose run --rm mips-dev /bin/bash

   # Inside the container
   mkdir -p build-docker && cd build-docker
   cmake ..
   make -j$(nproc)
   timeout 120s ./be300 --kernel ../linux4be20040908/vmlinux \
     > docker_stdout.log 2> docker_stderr.log
   ```
   The image installs clang/meson/ninja plus mipsel cross-compilers and
   libunicorn-dev.
   Base packages now include `gdb`, `gdb-multiarch`, `strace`, and `ltrace` for
   cross-debugging.

2. **Logs & artifacts:**
   - Always capture both stdout and stderr from emulator runs (`docker_*.log`).
   - RCU/TLBS instrumentation prints to stderr; grep for `CHECKPOINT`, `TLB_INJECT`,
     `MTC0_EPC`, etc.

3. **Kernel sources:**
   - `kernels/kernel-2.6/` holds the patched BE-300 kernel tree used to produce
     `linux4be20040908/vmlinux`. Consult it when symbol hunting or adjusting
     platform-specific code (`arch/mips/vr41xx/...`).

4. **Commit etiquette:** every investigation (successful or not) gets its own
   commit summarizing what happened, what was learned, and next steps, then
   push to `claude/explain-codebase-mm1561dhacl5ikyh-zdk3b`.
