# CLAUDE.md — Working Rules for This Repository

## Reference & Documentation
I have access to real be300 hardware and a Virtual machine with eMbedded Visual C++ 3.0 (with the be300 SDK).
I also have another VM with Platform Builder 3.0.
- docs/Vr4131-um_200203.pdf - NEC vr4131 SOC Users Manual
- docs/U14579EJ2V0UM00.pdf - NEC vrc4173 Companion Chip Users Manual
- docs/hardware.txt - notes from Linux4be project developers
- `hardware_survey/` - real hardware memory/register dumps from BEDiag tool
- `hardware_survey/BE300BootROM_v1.txt` - full 16KB ROM dump (PA 0x1FC00000, CRC32=0xFA3B5582)
- `hardware_survey/be300_boot_rom.bin` - extracted ROM binary loaded by emulator
- `ce/bediag/` - BEDiag diagnostic tool source and output

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

**CPU Engine:** GXemul 0.7.0 (copied into gxemul/ subdirectory, MIPS-only build). Provides native CP0, TLB, exception handling, dyntrans JIT, and kseg0/kseg1 address translation. Replaces Unicorn (removed). **Does not support MIPS16** — the VR4131 supports MIPS16 (16-bit compressed instruction mode, selected by setting bit 0 of a JALR target), but GXemul cannot decode MIPS16 instructions.

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
- `ce/restore_images/All_nand_300.bin` - WinCE 3.0 image (SPL v0.52)
- `ce/restore_images/org_CE_30.bin` - WinCE 3.0 image (SPL v0.60)
- `ce/restore_images/BE500.bin` - BE-500 model variant (SPL v0.62)
- `ce/restore_images/CE_Net.bin` - WinCE 4.0 image (SPL v0.62)

**Boot ROM**
- 16KB masked ROM at PA 0x1FC00000 (VA 0xBFC00000 kseg1, 0x9FC00000 kseg0)
- Dumped from real hardware: `hardware_survey/be300_boot_rom.bin`
- Reset vector: NOP → LUI/ORI/JR to 0xBFC002F0 (main boot code)
- ROM does: CP0 init, SDRAM timing, clock setup, NAND read, SPL load
- BEV TLB refill vector (+0x200) is all 0xFF in the original ROM (no handler)
- BEV general exception (+0x380) is boot code continuation (section copier + dispatcher), NOT a real exception handler — it just happens to overlap the BEV vector address
- **Emulator patches ROM at load time** with MIPS32 BEV handlers: TLB refill at +0x200, general exception dispatcher at +0x280 (checks ExcCode), EXL check at +0x384 to distinguish exception vs boot flow, boot code relocated to +0x394
- **ROM uses MIPS16 code** — ~5.5KB of MIPS16 at offsets 0xC20-0x219B (34 functions)
- BEV exception handler at +0x380 does JALR to 0x9FC00C85 (bit 0 = MIPS16 mode switch)
- MIPS16 functions use JALX (jump-and-link-exchange) to call back into MIPS32 ROM helpers, creating a cross-mode call graph
- NK.exe is 100% MIPS32 — no MIPS16 anywhere in the 6.2MB kernel
- Full MIPS16 disassembly saved in `build-host/rom_mips16_disasm.txt` (use `-m mips:16` flag with objdump)

**Boot ROM Layout:**
```
0x0000-0x00FF: Reset vector, exception stubs (256 B, MIPS32)
0x0100-0x0C1F: Initialization/setup code (~2.8 KB, MIPS32)
0x0C20-0x219B: MIPS16 function library (~5.5 KB, 34 functions)
0x219C-0x224F: Function metadata + address table (34 entries at 0x21C0)
0x2250-0x3FFF: Unused padding (~7.6 KB, 0xFF fill — available for MIPS32 rewrites)
```

**MIPS32 ROM helpers called from MIPS16 via JALX:**
- 0x9FC00464: unknown helper
- 0x9FC00834: memcpy-like
- 0x9FC00888: memset-like
- 0x9FC00980: context save
- 0x9FC009BC: context helper
- 0x9FC00BC0: another helper
- 0x9FC00C04: trampoline (MIPS32 at 0xC00-0xC1C pops s0,s1,a0 from stack, JR a0)

**MIPS16 workaround approach:**
Rather than adding MIPS16 support to GXemul (would require ~3K LOC of architectural changes to the dyntrans JIT — fixed 4-byte IC entries, no ISA mode tracking, no PC bit 0 handling) or translating all 34 MIPS16 functions, the emulator patches the ROM's BEV vectors at load time with purpose-built MIPS32 exception handlers. The MIPS16 function library in the ROM is never executed. objdump flag for MIPS16 disassembly: `-m mips:16` (not `-m mips:isa16`)

**Things to note**
- originally the kernels were loaded from a running WinCE (warm start, not cold) - hw may have been initialized by WinCE
- None of the test kernels had full hw support
- The BE-300 CAN be cold-booted from a fully unpowered state (battery removed). The ROM boot dispatcher handles all initialization.

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

# WinCE cold boot (SPL decompresses NK.exe, no hibernate redirect)
# Requires be300_boot_rom.bin in the working directory
gtimeout 60s ./be300 --nand ../ce/restore_images/All_nand_300.bin \
  --wince-cold-boot --log-mmio \
  > cold_stdout.log 2> cold_stderr.log

# WinCE NAND boot with warm-resume replay (legacy approach)
timeout 60s ./be300 --nand ../ce/restore_images/All_nand_300.bin \
  --wince-hw-seed --wince-resume-replay --log-wince-stall \
  > wince_stdout.log 2> wince_stderr.log

# WinCE NAND boot with MMIO logging
timeout 60s ./be300 --nand ../ce/restore_images/All_nand_300.bin --log-mmio \
  > wince_mmio_stdout.log 2> wince_mmio_stderr.log

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

### WinCE Cold Boot (--wince-cold-boot)

The `--wince-cold-boot` flag lets the SPL run its natural cold boot path. The SPL
decompresses NK.exe (~6.2MB) from NAND into RAM at PA 0x60000 and jumps to
the kernel entry at VA 0xA0060004.

**NK.exe Cold Boot Flow:**
- Entry: VA 0x80076B50 → CP0 init → JR to 0xA0076BA0 (kseg1)
- Sets SP=0xA0003800, enables CMU clock, calls ROM HW init
- BCU revision check → main HW init path at 0x76C60
- Version check at PA 0x2400 for 0x03020100, hibernate signature at PA 0x2524 for 0x3210xxxx
- VRC4173 init, NAND controller, cache init, switch SP to kseg0
- Jump to 0xA00772CC → more init → WAIT at VA 0xA0079598

**Cold Boot Continuation (0x800794C8):**
- Referenced by J instruction at 0x8007962C (after the unreachable cold boot init at 0x79DF8)
- Calls 6 init functions: 0x79ADC (VRC4173 interrupt init), 0x79B94, 0x79B9C, 0x79BA4, 0x79BAC, 0x7AB38 (OEMInit callback dispatcher)
- Flushes data cache (loop at 0x79500 using cache instruction)
- Calls 3 more init functions: 0x79BB4, 0x79B5C, 0x79C88
- Falls through to 0x79560 → PMU/DCU/SDRAMU → WAIT
- Function 0x7AB38 is an OEMInit callback dispatcher: walks 32 entries (20 bytes each) at PA 0xA0051680, calls function pointers via JALR. These callbacks do real WinCE kernel initialization.
- Running 0x794C8 does NOT update resume_ctx — the init functions at 0x794C8 don't write to PA 0x2200
- OEMInit callback table at PA 0x51680: 11 callback groups (55 non-zero words) captured from warm-boot and injected from `wince_resume_replay_data.h`. Contains function pointers into NK.exe OAL code (0x8007xxxx, 0x800Axxxx). On real hardware, populated by the ROM's MIPS16 boot dispatcher at 0x9FC00C21.

**ROMHDR / COPYentry (WinCE image section copier):**
- NK.exe has "ECEC" signature at offset 0x40, pTOC pointer at offset 0x44 (= 0x80655C54)
- ROMHDR at pTOC: physfirst=0x80060000, physlast=0x80656AC8, nummods=95, ulRAMStart=0x80660000
- 1 COPYentry: src=0x800BBA70 dst=0x80660000 copy=1029 total=52852 (kernel .data + .bss)
- Emulator processes COPYentry at NK.exe entry detection time (PC in PA 0x60000-0x100000 range)
- On real hardware, the ROM's MIPS16 section copier (0x9FC00C85) does this before NK.exe starts
- The COPYentry alone does not enable callback registration — additional boot context is needed

**ROM Boot Sequence (steps the emulator skips):**
The ROM at 0xBFC002F0 runs these steps before entering NK.exe:
1. CP0 init, HW init (JALR 0x9FC006F0)
2. Check functions (cold/warm detection)
3. Cold boot: clear PA 0x2400/24FC, init (JAL 0xFC00734)
4. Set SP=0x80003800, serial init (JAL 0xFC00498)
5. BINFS section copier (JALR 0x9FC00C85, MIPS16) — **we replicate this**
6. Boot dispatcher (JALR 0x9FC00C21, MIPS16) — **NOT replicated** (registers callbacks, NAND driver init)
7. SIU poke + BCU read loop (JAL 0xFC004E8/0xFC00488)
8. Load PA 0x24FC, JR to NK.exe entry
Steps 1-4 are handled by the SPL. Step 5 is our COPYentry code. Step 6 is the missing piece.

**Post-WAIT OAL Init (always taken on both cold and warm boot):**
- NOP sled → kseg0 switch → check1 (buttons) → check2 (VRC4173)
- check1 reads PA 0x0A00A042 (button register), masks 0x9E00
- **$t0 clobber bug:** check1 sets $t0=0xAA00A000 (button register base address).
  check2 (at 0x8007AFA8) only touches $a0/$a1/$v0, NOT $t0. The BNE $t0,$zero
  after check2 is ALWAYS taken → branch to 0x79634 (warm path). The cold boot
  init at 0x79DF8 is **dead code** via this path.
- 0x79634: JAL 0x78BC0 (OAL vtable init) → VR41xx HW setup → timer → ICU
- 0x79668: Full GPR/CP0 restore from resume_ctx table at PA 0x2200
- 0x797DC: LW $t0, 0($sp); JR $ra — function epilogue

**Kernel Cold-Start Entry (0x8007B398):**
The true kernel cold-start initialization, building everything from scratch:
- Clears CP0: Cause, EntryHi, Context, EntryLo0/1, PageMask, Count
- Zeros page table at PA 0x1000 (4KB)
- Sets up section table at PA 0x18C0 (64 entries, default handler 0x8008BC18, special handler 0x8008B8E4 for section 9)
- Initializes SP to 0xA00017E0 (kseg1, no TLB needed)
- Sets kernel data pointers at PA 0x1AC8 (0x80000000) and PA 0x1ACC (0x8008B84C)
- Sets PageMask=0x1800, Wired=2, writes fixed TLB entries for kernel address space
- Installs real exception handlers via JAL 0x8007B5F4:
  - PA 0x0000 (TLB refill): code from 0x8008C418
  - PA 0x0180 (general exception): code from 0x8008B240
  - PA 0x0100 (cache error): code from 0x800A8438
- Calls kernel functions: JAL 0x800A8448, JAL 0x800A83D8
- **Calls kernel main init: JAL 0x800947C8($a0=pTOC=0x80655C54)** — this is the function that should create processes, load the 95 XIP modules, and start the shell
- Calls OAL vtable init: JAL 0x80078BC0
- Calls more kernel functions: JAL 0x800A5C78, JAL 0x800942B4, JAL 0x800964FC
- Eventually enters the scheduler

**Kernel Entry Table (VA 0x80074D90):**
- [0] = 0x8008CEA4, [1] = 0x8009101C, [2] = 0x80090F34, [3] = 0x80090F40
- [4] = 0x80090F8C, [5] = 0x80090FC8, [6] = 0x80090FF4, [7] = 0x00000000
- 0x80074DB0 = 0x80655C54 (pTOC pointer)
- 0x80074DBC = "OEM\0" (ASCII identifier)

**NK.exe Memory Layout:**
```
0x80060000-0x80075FFF: Bootstrap and OAL data
0x80076B50-0x80079xxx: OAL initialization code
0x8007Axxx-0x8007Bxxx: OAL hardware drivers and cold-start
0x80080000-0x800Fffff: Kernel proper (~640KB)
  - 0x8008B240: General exception handler source (copied to PA 0x0180)
  - 0x8008BC18: Default section handler
  - 0x8008C418: TLB refill handler source (copied to PA 0x0000)
  - 0x800947C8: Kernel main init (takes pTOC as $a0)
0x800A0000-0x800Bxxxx: OAL callbacks and device drivers
0x80060000-0x80656AC8: Total NK.exe image (6.2MB)
0x80655C54: ROMHDR (pTOC) — physfirst=0x80060000, nummods=95
0x80660000-0x81000000: RAM (ulRAMStart to ulRAMEnd)
```

**Key SDRAM Data Structures:**
- PA 0x2200-0x22FF: resume_ctx — GPR/CP0 save area
- PA 0x2400: version marker (0x03020100 expected by check at 0x76CBC; mismatch clears PA 0x254C)
- PA 0x2524: hibernate signature (upper 16 bits == 0x3210 means valid hibernate state)
- PA 0x254C: hibernate flags (bits 0x03 must be non-zero for hibernate path)

**resume_ctx Table Layout (PA 0x2200, from disassembly of 0x79668-0x797E4):**
- GPR section (offsets 0x00-0x74): packed, skips $t0 (reg 8)
  - 0x00-0x18: $at(1)-$a3(7), 0x1C-0x68: $t1(9)-$gp(28)
  - 0x6C: $sp, 0x70: $fp, 0x74: $ra, 0x78: HI, 0x7C: LO
- CP0 section (offsets 0x80-0xD0): packed, skips regs 7,8,15,19,21-25,27,31
  - 0x80:Index 0x84:Random 0x88:EntryLo0 0x8C:EntryLo1
  - 0x90:Context 0x94:PageMask 0x98:Wired 0x9C:Count
  - 0xA0:EntryHi 0xA4:Compare **0xA8:Status** 0xAC:Cause
  - **0xB0:EPC** 0xB4:Config 0xB8:LLAddr 0xBC:WatchLo
  - 0xC0:XContext 0xC4:ECC 0xC8:TagLo 0xCC:TagHi 0xD0:ErrorEPC
- Status is loaded LAST (at 0x79714) for atomicity
- ICU registers follow at offsets 0xD4-0xE4
- Epilogue: LW $t0, 0($sp); JR $ra; ADDIU $sp, 4

**Hibernate State-Save Gating (0x76E68-0x76FB4):**
The state-save function that populates PA 0x2200 and installs exception handlers
is gated by four checks. ALL must pass for the save to run:
1. PA 0x2524 upper 16 bits == 0x3210 (hibernate signature)
2. VR4131 PMU register 0xC0 bit 4 == 0
3. PA 0x2404 != 0x31
4. PA 0x254C bits 0x03 != 0 (hibernate flags)
Note: the version check at 0x76CBC clears PA 0x254C if PA 0x2400 != 0x03020100,
which prevents check 4 from passing even if PA 0x254C was previously set.
This function is NOT called from the main NK.exe pre-WAIT init path.
It does NOT run during cold boot — resume_ctx is populated by the ROM dispatcher.

**VRC4173 Interrupt Registers:**
- VRC4173 ICU SYSINT1REG at offset 0x060 from VRC4173 base (0x0A000000) is **read-only** on real hardware — reflects active peripheral interrupt sources
- Interrupt status registers in ranges 0x060-0x077, 0x1100-0x113F, 0x1B00-0x1B2F use **write-1-to-clear** semantics in the emulator
- MSYSINT1 in dev_vr41xx.c must NOT force-enable ETIMER (bit 3) — prevents WinCE from controlling its own timer mask
- The NAND controller has phase-aware behavior (`wince_mode` flag in nand_state_t): buffer registers 0xA4A0-0xA4AC and STATUS2 at 0xA4C0 return 0 during SPL (to avoid ECC errors), active data after NK.exe loads

**NK.exe Analysis Tools:**
- Emulator dumps decompressed NK.exe to `nk_decompressed.bin` on first WAIT (6.2MB)
- NK.exe loaded at PA 0x60000: file offset = VA - 0x80060000
- `tools/scan_nk_producers.py` — scan for store instructions to specific VAs
- `tools/disasm_nk_ctx.py` — disassemble NK code regions
- Docker: `mipsel-linux-gnu-objdump -D -b binary -m mips:3000 -EL nk_decompressed.bin`

### Key Files for WinCE Boot
- `src/main.c` — `--nand`, `--wince-cold-boot` CLI flags
- `src/be300.h` — `nand_path`, `nand_data`, `nand_size`, `wince_cold_boot` fields
- `src/machine_be300.c` — WinCE boot path in `be300_create()`, NAND flash init, cold boot WAIT handling, ROM loading
- `src/loader.c` — `loader_load_nand()` B000FF parser
- `src/wince_boot.c` — WinCE diagnostic probes, replay logic, write watches (4500+ lines)
- `src/wince_boot_types.h` — WinCE boot state machine flags
- `src/wince_hw_seed_data.h` — captured initial memory/register state for warm resume
- `gxemul/src/devices/dev_vr41xx.c` — VR4131 ICU, timer, GPIO, interrupt assert/deassert; `timer_tick()` increments `pending_timer_interrupts`, `DEVICE_TICK(vr41xx)` asserts interrupt line; timer interrupt is deasserted on RTCINTREG write (offset 0x13E)
- `gxemul/src/devices/dev_ns16550.c` — VRC4173 SIU UART (GXemul native at 0x0A008680)
- `src/hw/rtc.c` — RTC elapsed time, RTCL1 timer, RTCINTREG write-one-to-clear
- `src/hw/nand.c` — NAND flash controller with SPL transfer engine, OOB synthesis, and phase-aware WinCE mode
- `src/wince_resume_replay_data.h` — captured OEMInit callback table, bootctx stub, dispatch tables (from warm-boot)
- `src/hw/bcu.c` — Silenced unhandled register reads (SPL probes many)
