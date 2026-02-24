# BE-300 Emulator — Handover Notes

## Project Goal

Functional emulator for the Casio BE-300/500 PDA (NEC VR4131 / µPD30131 CPU).
End goal: boot the device's WinCE firmware and load linux4.be kernels/ramdisks.

Architecture: **Unicorn Engine** (QEMU-derived MIPS CPU core) + custom C peripheral
implementations. See the plan file at `.claude/plans/nifty-floating-emerson.md` for the
full design.

---

## Current State (as of 2026-02-24)

### What works

- Full project builds cleanly with `make` (clang, macOS ARM)
- 7/7 unit tests pass (`make test`)
- **MIPS64 / VR5432 mode** — kernel runs past the MIPS-III 64-bit instruction barrier
- SDRAM (16 MB), ROM (32 MB), internal I/O MMIO, and VRC4173 CS3 (16 MB) are mapped
- Peripheral stubs: BCU, CMU, PMU, ICU, SIU (NS16550 UART → stdout), RTC, GPIO
- **VRC4173 companion UART stub** — returns LSR=0x60 (THRE|TEMT) so the kernel's
  transmitter-ready polling loop exits; THR writes forwarded to stdout
- ELF32 LE kernel loader (`--kernel vmlinux`) with correct sign-extended entry VA
- ROM loader (`--rom rom.bin`) for flat binary at reset vector PA 0x1FC00000
- MACC instruction emulation (VR4131 SPECIAL2) via `insn_invalid` hook
- Permissive fault handler — maps 1 MB zero pages on READ/WRITE unmapped; stops on FETCH
- **PRId intercept hook** — `prid_hook` in machine.c intercepts every `mfc0 $rt, $15`
  and substitutes VR4131_PRID (0x00000C80); advances PC past the instruction
- **BCU CLKSPEEDREG pre-filled** to 0x004A in kernel boot path
  (TCLKDIV=2 → PClock/4=TClock; CPUCLK=10 → PClock=165888000 Hz)

### Kernel boot output (last known state — NOT yet run after latest fixes)

Before the PRId hook and BCU clock fix, the output was:
```
<4>Linux version 2.6.8.1 ... #39 Wed Sep 8 16:15:43 CEST 2004
<4>CPU revision is: 00000400          ← VR5432 PRId (wrong)
<6>Unexpected CPU of NEC VR4100 series
<6>PClock: 0Hz                        ← CLKSPEEDREG=0
<0>Kernel panic: Unexpected CPU of NEC VR4100 series
```

The PRId hook and CLKSPEEDREG=0x004A should fix both blockers.
**The next boot attempt has NOT been run yet — do this first.**

### Immediate next action

```bash
make test    # verify 7/7 still pass
./be300 --kernel linux4be20040908/vmlinux 2>boot.log
cat boot.log
```

Expected (if PRId hook works): `CPU revision is: 00000c80` and
`PClock: 165888000Hz  TClock: 41472000Hz`, then boot continues further.

---

## Key Technical Facts

### CPU

| Fact | Value |
|------|-------|
| CPU | NEC VR4131 (µPD30131) |
| ISA | MIPS III (64-bit registers, 32-bit phys addr space) |
| PRId (CP0 reg 15) | 0x00000C80 |
| Endianness | Little-endian (BIGENDIAN pin = 0 on BE-300) |
| FPU | None (COP1 absent) |
| LL/SC | Not implemented |
| MACC (SPECIAL2 0x1C) | Custom NEC extension, emulated via insn_invalid hook |

### Memory Map (physical)

| PA Range | Region | Size |
|----------|--------|------|
| 0x00000000 | SDRAM | 16 MB (configurable up to 64 MB) |
| 0x0A000000 | VRC4173 companion chip (CS3) | 16 MB MMIO |
| 0x0F000000 | Internal I/O (MMIO) | 4 KB |
| 0x1E000000 | ROM/Flash | 32 MB |
| 0x1FC00000 | Reset vector (within ROM region) | — |

### VRC4173 companion chip (CS3, PA 0x0A000000)

The linux4.be kernel uses the VRC4173 NS16550 UART for early console output before
the SIU is configured.  Register layout (4-byte register spacing):

| PA | Register | Direction | Notes |
|----|----------|-----------|-------|
| 0x0A008680 | THR | write | Transmit byte — our stub forwards to stdout |
| 0x0A008680 | RBR | read | Receive — stub returns 0 |
| 0x0A008694 | LSR (+0x14) | read | Our stub returns 0x60 (THRE\|TEMT always) |

### vmlinux (linux4be20040908/vmlinux)

| Item | Value |
|------|-------|
| ELF entry VA | 0x80272018 (kseg0 → PA 0x00272018) |
| Seg 0 (text) | PA 0x00001000, filesz 0x1C51F0 |
| Seg 1 (data) | PA 0x001C8000, filesz 0xC5086, memsz 0xD5EC8 |
| BSS | PA 0x0028D086, len 0x10E42 |
| Embedded initramfs | CPIO magic at file offset 0x1AF0D0 (within seg 0) |
| 64-bit insns | 133,297 (LD/SD/DADDIU/LDL/LDR/BEQL/BNEL dominate) |

### CyaCE / Boot path

The linux4.be kernel was designed to be loaded by **CyaCE**, a WinCE ELF loader app
that runs from within WinCE.  WinCE has already done hardware init (BCU timing,
CMU clocks, cache setup) before CyaCE jumps to the kernel.

**Consequence for emulation:** The kernel's `prom_init()` / `vr41xx` platform code
assumes a "warm" system.  Early failures are possible if:
1. **BCU registers** return zeros — kernel may miscalculate clock frequencies
2. **CMU clkdiv** returns zero — divide-by-zero in loops_per_jiffy
3. **Cache flush routines** — early head.S uses CACHE instruction; VR4131 Rev 1.2
   has cache bugs (see `docs/` patches)
4. **PRId check** — prom_init() checks for VR41xx PRId (0x00000C80 family) ← FIXED

### Unicorn quirks discovered

| Issue | Cause | Fix |
|-------|-------|-----|
| `uc_mem_map(PA=0, 16MB)` returned `UC_ERR_ARG` | `uc_ctl_set_cpu_model()` creates internal QEMU regions at low PA | Call `bus_init()` **before** `uc_ctl_set_cpu_model()` |
| `UC_MIPS_REG_AC0` is no-op | Deprecated in Unicorn 2.1.4 | Use `UC_MIPS_REG_HI`/`UC_MIPS_REG_LO` |
| MACC never fires `insn_invalid` hook in MIPS32 | 4Kc interprets funct=0x20 as CLZ | Non-issue in MIPS64/VR5432 mode |
| Trace hook shows `insn=00000000` for kseg0 | `uc_mem_read` uses physical addresses | Strip kseg bits: `address & 0x1FFFFFFFu` |
| `UC_MIPS_REG_CP0_PRID` not writable | Not exposed by Unicorn 2.1.4 | `prid_hook`: UC_HOOK_CODE intercepts `mfc0 $rt,$15`, writes VR4131_PRID, advances PC+4 |
| kseg0/1 VAs must be sign-extended to 64-bit | MIPS64 mode treats 0x80000000 as kuseg | `mips_sext()`: `(uint64_t)(int32_t)va32` |

### PRId hook — implementation detail

`prid_hook()` in `src/machine.c` is registered as `UC_HOOK_CODE` (fires every instruction).

MFC0 $rt, $15 encoding: `(insn & 0xFFE0FFFF) == 0x40007800`
- Bits[31:26] = 0x10 (COP0)
- Bits[25:21] = 0 (MF function)
- Bits[20:16] = rt (destination, variable)
- Bits[15:11] = 15 (CP0 PRId register)
- Bits[10:0]  = 0 (sel=0)

When matched: writes VR4131_PRID to `UC_MIPS_REG_0 + rt`, sets PC = address+4 to skip
the instruction before Unicorn executes it.

### BCU CLKSPEEDREG encoding

`m->bcu.clkspeedreg = 0x004A` in the kernel boot path of `machine_create()`.

Formula: PClock = (CPUCLK + 8) × 2 × 4608000 Hz
- CPUCLK = bits[4:0] = 0x0A = 10 → (10+8) × 2 × 4608000 = **165,888,000 Hz**
- TCLKDIV = bits[7:5] = 0b010 = 2 → TClock = PClock / 4 = **41,472,000 Hz**

---

## Concrete Next Steps

### Step 1 (IMMEDIATE): Run the boot attempt

The PRId hook and CLKSPEEDREG fix were committed but the boot was NOT run yet:

```bash
./be300 --kernel linux4be20040908/vmlinux 2>boot.log
cat boot.log           # stderr diagnostic
# stdout = VRC4173 UART output (kernel printk)
```

Watch for:
- `CPU revision is: 00000c80` — confirms PRId hook works
- `PClock: 165888000Hz` — confirms CLKSPEEDREG works
- Any new panic or UC_ERR — next blocker to diagnose

### Step 2: PRId hook tuning (if needed)

If the kernel still panics with "Unexpected CPU":
- The hook may not be firing (verify with `--trace 2>trace.log | grep "mfc0"`)
- Some MFC0 PRId reads may use `sel != 0` → check bits[2:0] of matched insns
- The hook overhead fires on every instruction; if performance is a problem,
  consider patching the bytes in SDRAM at the PRId read site instead

### Step 3: CMU pre-initialization (if PClock still 0)

If CLKSPEEDREG=0x004A doesn't produce the right PClock, the kernel formula may differ.
Try reading the formula from the kernel source:
```bash
# linux4be20040908/ contains a vmlinux with debug symbols
# Use nm/objdump style Python or gdb to find get_pclock() in vmlinux
python3 -c "
import struct, sys
data = open('linux4be20040908/vmlinux','rb').read()
# Search for the CLKSPEEDREG read pattern: 0F001814 (PA of BCU+0x14)
# to find the clock calculation code
"
```

### Step 4: Serial output milestone

Goal: see "Linux version" or any kernel boot message appear on stdout.
The kernel outputs via VRC4173 UART (PA 0x0A008680 THR) which our stub forwards
to stdout.  Any printk before the SIU is configured goes there.

### Step 5 onwards (follow boot log)

Once past prom_init(), common next blockers:
- **Timer interrupt** — kernel programs CP0 Count/Compare and expects ICU to fire;
  our ICU stub accepts writes but never raises interrupts → idle loop hangs
- **TLB initialization** — early kuseg accesses need TLB entries
- **Memory detection** — kernel may probe SDRAM size via BCU registers
- **VRC4173 full init** — LCD/keyboard for WinCE; not needed for Linux headless

---

## File Map

```
src/
  machine.h / machine.c   — machine struct, lifecycle, run loop, prid_hook
  bus.h / bus.c           — PA dispatch, uc_mmio_map callbacks, VRC4173 stub
  loader.h / loader.c     — ROM flat binary and ELF32 segment loaders
  macc.h / macc.c         — VR4131 MACC instruction emulation
  main.c                  — CLI (--kernel, --rom, --trace, --log-mmio, --sdram, --ram)
  hw/
    bcu.h / bcu.c         — Bus Control Unit (stub, REVID=0x0104, CLKSPEEDREG pre-filled)
    cmu.h / cmu.c         — Clock Mask Unit (stub)
    pmu.h / pmu.c         — Power Management Unit (stub)
    icu.h / icu.c         — Interrupt Control Unit (W1C status, mask registers)
    siu.h / siu.c         — NS16550 UART → putchar(stdout)
    rtc.h / rtc.c         — RTC elapsed time counter
    gpio.h / gpio.c       — GPIO (stub)

linux4be20040908/
  vmlinux                 — MIPS III LE ELF kernel with embedded initramfs
  loader.exe              — CyaCE WinCE ELF loader (reference only)
  cyacecfg.txt            — CyaCE config (boot args, load address reference)

docs/
  Vr4131-um_200203.pdf    — VR4131 hardware manual (authoritative reference)
  vr4131_ds.pdf           — VR4131 data sheet
  *.patch                 — VR4131 Rev 1.2 cache bug patches for linux4.be

CASIO30E1.ISO             — Factory restore ISO (contains WinCE ROM image)
```

---

## Useful Commands

```bash
make                    # build emulator
make test               # run unit tests
make clean && make      # full rebuild

./be300 --kernel linux4be20040908/vmlinux 2>boot.log
./be300 --kernel linux4be20040908/vmlinux --log-mmio 2>boot.log
./be300 --kernel linux4be20040908/vmlinux --trace 2>trace.log
./be300 --help          # full option list

# Inspect ELF
python3 -c "import struct; ..."   # macOS has no readelf; use Python

# Check kernel instruction types
python3 -c "..."  # see docs above for 64-bit instruction census script
```
