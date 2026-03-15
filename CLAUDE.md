# CLAUDE.md — Working Rules for This Repository

## Reference & Documentation
I have access to real be300 hardware and a Virtual machine with eMbedded Visual C++ 3.0 (with the be300 SDK).
- docs/Vr4131-um_200203.pdf - NEC vr4131 SOC Users Manual
- docs/U14579EJ2V0UM00.pdf - NEC vrc4173 Companion Chip Users Manual
- docs/hardware.txt - notes from Linux4be project developers

## Source Code Layout

**Core emulation (src/)**
- `machine.c` — Main emulation: prid_hook (per-instruction), intr_hook (per-exception), machine_create/run/stop, IRQ injection, execve helpers
- `machine.h` — machine_t struct, machine_config_t, shared inline helpers
- `bus.c` / `bus.h` — VRC4173 companion chip MMIO, VR4131 I/O region dispatch
- `loader.c` / `loader.h` — ELF kernel loader, B000FF NAND SPL parser
- `macc.c` / `macc.h` — MACC (multiply-accumulate) instruction emulation
- `ui.c` / `ui.h` — SDL2 display window (optional, gated on HAVE_SDL2)

**WinCE support (src/)**
- `wince_diag.c` / `wince_diag.h` — Diagnostic logging: DIV stack watch, call tracing, context probing, PA watch hooks, stall dumps (all gated on --log-wince-stall)
- `wince_init.c` / `wince_init.h` — Boot-time seeding: exception vectors, kdata, warm profiles, bootrom window

**Emulation subsystems (src/)**
- `mem_alias.c` / `mem_alias.h` — kseg0/kseg1/kuseg memory aliasing, PA coherence
- `tlb_shadow.c` / `tlb_shadow.h` — Shadow TLB recording, VA→PA translation, user handoff tracing
- `null_call.c` / `null_call.h` — Null function pointer call recovery (Linux & WinCE)
- `probes.c` / `probes.h` — Kernel diagnostic probe hooks (initcall, pgui, IRQ, RCU, page fault, etc.)

**Hardware peripherals (src/hw/)**
- `bcu.c/h` — Bus Control Unit
- `icu.c/h` — Interrupt Control Unit
- `rtc.c/h` — Real-Time Clock
- `siu.c/h` — Serial Interface Unit (UART)
- `nand.c/h` — NAND flash controller
- `gpio.c/h` — GPIO
- `cmu.c/h` — Clock Management Unit
- `pmu.c/h` — Power Management Unit

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
- All kernels have been booted to userspace on real hardware
- All kernels contain ramdisks
- None of the linux kernels had much more than serial and framebuffer support, no NAND, no touchscreen, no compact flash support.
- These kernels are from a project named Linux4be. They are development kernels and may contain mistakes.
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

**WinCE ELF loader**
- `ce/cyace` - source code for CyaCE loader

**WinCE restore images**
- Should be full NAND images including NK.exe
- RESTORE_IMAGES.md, contains details of the NAND images
- `ce/restore_images/All_nand_300.bin` - WinCE 3.0 image
- `ce/restore_images/CE_Net.bin` - WinCE 4.0 image

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
   timeout 180s ./be300 --kernel ../kernels/vmlinux-2.6 \
     > docker_2.6_stdout.log 2> docker_2.6_stderr.log
   timeout 180s ./be300 --cmdline "console=tty0 console=ttyS0,9600 root=/dev/ram" --kernel ../kernels/vmlinux-pgui-demo \
     > docker_2.4_stdout.log 2> docker_2.4_stderr.log
   ```
   The image installs clang/meson/ninja plus mipsel cross-compilers and
   libunicorn-dev.
   Base packages now include `gdb`, `gdb-multiarch`, `strace`, and `ltrace` for
   cross-debugging.

2. **MIPS PE (Windows CE) Toolchain:**
   The Docker container includes a specialized `mipsel-pe` toolchain (Binutils 2.21.1 patched via 7shi/1374792) for analyzing Windows CE Portable Executable (PE) files.
   - **Tools:** `mipsel-pe-objdump`, `mipsel-pe-nm`, `mipsel-pe-objcopy`, `mipsel-pe-ar`, etc.
   - **Usage Example (Disassemble WinCE loader):**
     ```bash
     mipsel-pe-objdump -d linux4be20040908/loader.exe | head -n 50
     ```
   - **Target Names:** Supports `pe-mips` and `pei-mips`.

3. **Logs & artifacts:**
   - Always capture both stdout and stderr from emulator runs (`docker_*.log`).

---

## WinCE NAND Boot: Debugging & Testing

### NAND Image Layout (All_nand_300.bin, 16MB)
- `0x00000` — Partition table / boot metadata (16KB)
- `0x04000` — SPL bootloader (B000FF format, ~49KB, "Kernel loader core - Ver 0.52")
- `0x14000` — NK.exe (compressed, ~6MB, Casio proprietary compression)
- `0x3B5000` — FAT16 filesystem (12MB)

### SPL B000FF Details (confirmed via disassembly)
- **Signature**: `B000FF\n` at NAND offset 0x4000
- **Format**: 7-byte sig + 4-byte image_start + 4-byte image_length, then records [addr(4) + len(4) + cksum(4) + data(len)]
- **Load address**: VA 0x80F00000 = PA 0x00F00000 (kseg0, within 16MB SDRAM)
- **Entry point**: 0x80F00000 → NOP, LUI/ORI/JR → jumps to 0x80F02404
- **Code flow**: 0x2404 writes CP0 Config, switches to kseg1 (uncached 0xA0F0xxxx), calls HW init routines
- **HW access**: VR4131 I/O at 0xAF00xxxx, VRC4173 at 0xAA00xxxx/0xAA01xxxx, framebuffer at 0xAA20xxxx
- **CP0 writes**: Only Config ($16) and TagLo ($28) — does NOT set Status, relies on boot-time value
- **No exception vectors**: SPL does not set BEV=0 or install its own exception handlers

### Build & Test Commands (in Docker)
```bash
# Build
cd /work && mkdir -p build-docker && cd build-docker
cmake .. && make -j$(nproc)

# WinCE NAND boot test (60s timeout)
timeout 60s ./be300 --nand ../ce/restore_images/All_nand_300.bin \
  > wince_stdout.log 2> wince_stderr.log
cat wince_stdout.log                         # serial output
grep -E "Unhandled|STOP|error" wince_stderr.log | sort -u | head -30

# WinCE NAND boot with MMIO logging
timeout 60s ./be300 --nand ../ce/restore_images/All_nand_300.bin --log-mmio \
  > wince_mmio_stdout.log 2> wince_mmio_stderr.log

# WinCE NAND boot with instruction trace (very verbose, short timeout)
timeout 10s ./be300 --nand ../ce/restore_images/All_nand_300.bin --trace \
  > wince_trace_stdout.log 2> wince_trace_stderr.log

# Linux kernel regression tests
# NOTE: These kernels never terminate on their own — they run until timeout
# kills them (exit code 124). This is expected, NOT a failure. To check for
# regressions, inspect stdout for successful boot markers like
# "Freeing unused kernel memory" (2.6) or similar userspace-entry messages.
timeout 180s ./be300 --kernel ../kernels/vmlinux-2.6 \
  > docker_2.6_stdout.log 2> docker_2.6_stderr.log; \
  grep -q "Freeing unused kernel memory" docker_2.6_stdout.log && echo "2.6 OK" || echo "2.6 FAIL"
timeout 180s ./be300 --cmdline "console=tty0 console=ttyS0,9600 root=/dev/ram" \
  --kernel ../kernels/vmlinux-pgui-demo \
  > docker_2.4_stdout.log 2> docker_2.4_stderr.log; \
  grep -q "Freeing unused kernel memory" docker_2.4_stdout.log && echo "2.4 OK" || echo "2.4 FAIL"
```

### SPL Disassembly (in Docker, cross-tools available)
```bash
# Extract and flatten SPL from NAND (use Python B000FF parser)
# Disassemble flat binary
mipsel-linux-gnu-objdump -D -b binary -m mips:3000 -EL spl_flat.bin > spl_disasm.txt
# Find hardware address references
grep -E "lui.*0x(0a|aa|af|bf)" spl_disasm.txt
# Find CP0 accesses (MTC0/MFC0)
grep -E "mtc0|mfc0" spl_disasm.txt
```

### Key Files for WinCE Boot
- `src/main.c` — `--nand` CLI flag
- `src/machine.h` — `nand_path`, `nand_data`, `nand_size` fields in config/state
- `src/machine.c` — WinCE boot path in `machine_create()`, null-call recovery gated on `!nand_path`
- `src/loader.c` — `loader_load_nand()` B000FF parser
- `src/bus.c` — VRC4173 UART stubs (IER, IIR, LCR, MCR, MSR, SCR), companion chip stubs
- `src/hw/rtc.c` — Auto-advance etime on read (fixes SPL polling loops)
- `src/hw/bcu.c` — Silenced unhandled register reads (SPL probes many)

### Known Issues / Blockers
- SPL calls address 0 via null function pointer (JALR with t9=0) — needs investigation
- SPL runs in kseg1 (uncached, 0xA0F0xxxx) — memory must be accessible at PA 0x00F00000
- SPL does not install exception vectors — BEV=1 vectors at 0xBFC00000+ contain zeros
- RTC auto-advance-on-read is a crude approximation; may cause timing issues for Linux kernels
