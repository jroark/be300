# CLAUDE.md — Working Rules for This Repository

## Reference & Documentation
I have access to real be300 hardware and a Virtual machine with eMbedded Visual C++ 3.0 (with the be300 SDK).
I also have another VM with Platform Builder 3.0.
- docs/Vr4131-um_200203.pdf - NEC vr4131 SOC Users Manual
- docs/U14579EJ2V0UM00.pdf - NEC vrc4173 Companion Chip Users Manual
- docs/hardware.txt - notes from Linux4be project developers

## Source Code Layout

**Core emulation (src/)**
- `machine_be300.c` — Machine setup (GXemul framework init, VR4131 CPU, device registration, kernel loading) and main emulation loop
- `be300.h` — be300_state_t struct, machine_config_t, physical address map constants
- `loader.c` / `loader.h` — ELF kernel loader, B000FF NAND SPL parser (uses GXemul store_buf)
- `ui.c` / `ui.h` — Display stub (GXemul dev_fb handles framebuffer; SDL2 integration TODO)
- `main.c` — CLI argument parsing, calls be300_create/be300_run/be300_destroy

**GXemul CPU engine (gxemul/)**
- `gxemul/src/cpus/` — VR4131 MIPS CPU: instruction execution, CP0, TLB, dyntrans
- `gxemul/src/core/` — Memory, interrupt, timer, settings, console framework
- `gxemul/src/devices/` — dev_vr41xx (VR4131 ICU/timer), dev_ns16550 (UART), dev_fb, dev_ram
- `gxemul/src/machines/machine_hpcmips.c` — BE-300 machine definition (framebuffer, SIU, bootinfo)
- `gxemul/src/stubs.c` — Minimal stubs for unused GXemul subsystems (debugger, x11, diskimage)
- `gxemul/config.h` — MIPS-only build configuration

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

## Debugging & Reverse Engineering Environment Policy

- mipsel debugging and cross tooling is in the Docker container (`mips-dev`) only.
- Commit and push from the host environment only (not from inside Docker).
- Typical split:
  - Container: `mipsel-linux-gnu-readelf`, `mipsel-pe-nm`, kernel symbol dumping, wince symbol dumping, offset finding (The /tmp dir doesn't persist between container invocations.).
    - The local dir is mounted as /work in the container
    - The container has a mips cross developement toolchain for analyzing the kernels and other mips binaries
  - Host: `git add`, `git commit`, `git push`. (host does not have mipsel cross development)

---

## Project Context

**Target:** Casio BE-300 (NEC VR4131 MIPS little-endian) emulator using GXemul 0.7.0 MIPS CPU core.

**CPU Engine:** GXemul 0.7.0 (copied into gxemul/ subdirectory, MIPS-only build). Provides native CP0, TLB, exception handling, dyntrans JIT, and kseg0/kseg1 address translation. Replaces Unicorn (removed).

**Kernels**
- All kernels have been booted to userspace on real hardware
- All kernels contain ramdisks
- None of the linux kernels had much more than serial and framebuffer support, no NAND, no touchscreen, no compact flash support.
- These kernels are from a project named Linux4be. They are development kernels and may contain mistakes.
- `kernels/vmlinux`            - ELF 32-bit LSB executable, MIPS, MIPS-II version 1 (SYSV), statically linked, not stripped, too many notes (256)
  - cmdline: "console=tty0 console=ttyS0,9600 root=/dev/ram"
- `kernels/vmlinux_sdlregtest` - ELF32 MIPS LE, Linux version 2.4.18-mips (mouse@mouse.office.altlinux.ru) (gcc version 3.0.4) #325   20 14:06:02 MSK 2003
  - cmdline: "console=tty0 console=ttyS0,9600 root=/dev/ram"
- `kernels/vmlinux-mw`         - ELF32 MIPS LE, Linux version 2.4.18-mips (jroark@dhcppc4) (gcc version 3.0.1) #309 Sun May 18 03:01:37 PDT 2003
  - cmdline: "console=tty0 console=ttyS0,9600 root=/dev/ram"
- `kernels/vmlinux-pgui-demo`  - ELF32 MIPS LE, Linux version 2.4.18-mips known good kernel and ramdisk (booted to userspace on real hardware)
  - cmdline: "console=tty0 console=ttyS0,9600 root=/dev/ram"
  - ramdisk: `kernels/ramdisk-pgui-full.gz`
- `kernels/vmlinux-pgui-test1` - ELF32 MIPS LE, Linux version 2.4.18-mips (jroark@dhcppc4) (gcc version 3.0.1) #309 Sun May 18 03:01:37 PDT 2003
  - cmdline: "console=tty0 console=ttyS0,9600 root=/dev/ram"

**Kernel source**
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

---

## Development Workflow & Tooling

1. **Build and test from the host:**
   ```bash
   # Inside the container
   # rebuild and test a 2.4 kernel
   mkdir -p build-host && cd build-host
   cmake ..
   make -j$(nproc)
   gtimeout 20s ./be300 --cmdline "console=tty0 console=ttyS0,9600 root=/dev/ram" --kernel ../kernels/vmlinux-pgui-demo \
     > 2.4_stdout.log 2> 2.4_stderr.log
   ```

2. **MIPS Linux ELF Toolchain (in Docker):**
   # Build/update the container image
   docker compose build mips-dev

   # Drop into the dev shell
   docker compose run --rm mips-dev /bin/bash

   The Docker container includes a specialized `mipsel-linux-gnu` toolchain for analyzing mipsel linux elf Executable files.
   - **Tools:** `mipsel-linux-gnu-objdump`, `mipsel-linux-gnu-nm`, `mipsel-linux-gnu-objcopy`, `mipsel-linux-gnu-ar`, etc.
   - **Target Names:** Supports `linux-elf-mips` and `elf-mips`.

3. **MIPS PE (Windows CE) Toolchain (in Docker):**
   # Build/update the container image
   docker compose build mips-dev

   # Drop into the dev shell
   docker compose run --rm mips-dev /bin/bash

   The Docker container includes a specialized `mipsel-pe` toolchain (Binutils 2.21.1 patched via 7shi/1374792) for analyzing Windows CE Portable Executable (PE) files.
   - **Tools:** `mipsel-pe-objdump`, `mipsel-pe-nm`, `mipsel-pe-objcopy`, `mipsel-pe-ar`, etc.
   - **Usage Example (Disassemble WinCE loader):**
     ```bash
     mipsel-pe-objdump -d ce/loader.exe | head -n 50
     ```
   - **Target Names:** Supports `pe-mips` and `pei-mips`.

4. **Logs & artifacts:**
   - Always capture both stdout and stderr from emulator runs (`*.log`) and screenshot.
   - Serial output goes to stdout; emulator diagnostics go to stderr.
   - The emulator opens an SDL window on macOS regardless of DISPLAY. **Do not run the emulator from automated/non-interactive contexts** (e.g., CI, Bash tool) — it will hang on SDL init. Always ask the user to run emulator tests from their terminal.
   - When redirecting stdout to a file, `console_putchar()` flushes immediately so output survives timeout kills.

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

### Build & Test Commands (on Host)
```bash
# Build
cd /work && mkdir -p build-host && cd build-host
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

# Linux kernel regression tests (They all boot to userspace)
# NOTE: These kernels never terminate on their own — they run until timeout
# kills them (exit code 124). This is expected, NOT a failure.
# Check the screenshot after exit for userspace pico gui running
gtimeout 20s ./be300 --cmdline "console=tty0 console=ttyS0,9600 root=/dev/ram" \
  --kernel ../kernels/vmlinux-pgui-demo \
  > 2.4_stdout.log 2> 2.4_stderr.log
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
- `src/be300.h` — `nand_path`, `nand_data`, `nand_size` fields in config/state
- `src/machine_be300.c` — WinCE boot path in `be300_create()`, NAND flash init
- `src/loader.c` — `loader_load_nand()` B000FF parser
- `gxemul/src/devices/dev_vr41xx.c` — VR4131 ICU, timer, GPIO (GXemul native)
- `gxemul/src/devices/dev_ns16550.c` — VRC4173 SIU UART (GXemul native at 0x0A008680)
- `src/hw/rtc.c` — Auto-advance etime on read (fixes SPL polling loops)
- `src/hw/bcu.c` — Silenced unhandled register reads (SPL probes many)
