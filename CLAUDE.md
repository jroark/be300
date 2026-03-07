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
- All kernels booted to userspace on real hardware
- `kernels/vmlinux-2.6`        — ELF32 MIPS LE, 2.6.8.1, built 2004-09-08.
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

**Kernel source**
- `kernels/src/linux-2.6` - approximate kernel source of `kernels/vmlinux-2.6`
- `kernels/src/linux-2.4.18` - approximate kernel source of 2.4.18 kernels

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

## Development Workflow & Tooling

1. **Work in Docker (Linux toolchain + Unicorn build):**
   ```bash
   # Build/update the container image
   docker compose build mips-dev

   # Drop into the dev shell
   docker compose run --rm mips-dev /bin/bash

   # Inside the container
   # rebuild and test a 2.6 & 2.4 kernel
   mkdir -p build-docker && cd build-docker
   cmake ..
   make -j$(nproc)
   timeout 180s ./be300 --kernel ../linux4be20040908/vmlinux \
     > docker_2.6_stdout.log 2> docker_2.6_stderr.log
   timeout 180s ./be300 --cmdline "console=tty0 console=ttyS0,9600 root=/dev/ram" --kernel ../kernels/vmlinux-pgui-demo \
     > docker_2.4_stdout.log 2> docker_2.4_stderr.log
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
