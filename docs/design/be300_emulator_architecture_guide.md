# Casio BE-300 Emulator Architecture Guide (Platform-Agnostic)

## 1. Purpose

This document describes the hardware and boot contracts required to emulate the Casio Cassiopeia BE-300 well enough to boot recovered Linux4BE kernels and ramdisks.  
It is written for developers building on any emulator core, not tied to QEMU internals.

The goal is to reproduce BE-300 architectural behavior, not to match one emulator implementation line-for-line.

## 2. Target Hardware Profile

- CPU family: NEC VR41xx class (Linux expects VR4131-like identity/behavior).
- ISA mode: MIPS32 little-endian userland/kernel.
- RAM: 16 MiB physical at `0x00000000`.
- ROM window: `0x1fc00000` (4 MiB region used as boot ROM space).
- Interrupt controller block (VRIP ICU): base `0x0f000080`.
- RTC/timer block (VRIP RTC): base `0x0f000100`.
- UART used by Linux: base `0x0a008680`, IRQ line 19, 18.432 MHz clock, 8250-compatible programming model with 4-byte register stride.
- Framebuffer aperture expected by kernel: around `0x0a200000` (exact display model can be stubbed initially).
- Companion windows (board-specific glue logic) in `0x0a00xxxx` range; many accesses can be stubbed early if safely ignored.

## 3. Architectural Principles

1. Model physical addresses, not KSEG aliases.  
   Kernel code uses `0x8xxxxxxx`/`0xAxxxxxxx` virtual aliases; your board model should expose physical devices at their physical addresses.

2. Prioritize boot-critical subsystems in this order:
   1. CPU + RAM + kernel entry
   2. ICU interrupt routing
   3. periodic timer/RTC interrupt
   4. UART console
   5. initrd handoff

3. Favor deterministic reset semantics.  
   On every reset, restore the same entry PC/registers and in-RAM boot argument structures.

4. Preserve little-endian semantics end-to-end.  
   The recovered kernels are mipsel; mismatched endianness in MMIO or handoff structures causes silent failure.

## 4. Memory Map Contract (Minimum)

- `0x00000000 - 0x00ffffff`: 16 MiB RAM.
- Optional RAM mirror at `0x08000000 - 0x08ffffff`: useful as compatibility alias for some observed mappings.
- `0x0a000000 - 0x0a00ffff`: companion I/O windows (start as benign stubs if needed).
- `0x0a008680`: UART MMIO.
- `0x0a200000`: framebuffer/VRAM aperture (can be RAM-backed initially).
- `0x0f000000 - 0x0f000fff`: VR core/system control window (include at least clock/divider register used by kernel clock init).
- `0x0f000080`: ICU registers.
- `0x0f000100`: RTC registers.
- `0x1fc00000 - 0x1fffffff`: ROM space.

## 5. CPU and MMU Requirements

### 5.1 CPU identity and reset

The Linux4BE kernels are sensitive to processor identity.

- Set PRID to VR4131-compatible value (`0x00000c81`) or a value your target kernel recognizes as VR41xx-compatible.
- Ensure reset places PC at kernel ELF entry and preserves boot argument register contract.
- Keep Count/Compare behavior coherent with your core timer implementation; do not partially spoof one without the other.

### 5.2 Addressing model

- Respect MIPS KSEG0/KSEG1 translation behavior in CPU core.
- Board-level device map should remain physical.
- Be careful with TLB EntryLo PFN width/physical mask assumptions on mixed 32/64-bit emulator builds; this is a common source of bad physical addresses and usermode crashes.

### 5.3 Observed failure mode to watch

A recurring late-boot failure is invalid user PTE/swap entry propagation (`swap_dup: Bad swap file entry ...`).  
This typically appears after kernel reaches root mount and first usermode transitions.  
Instrument TLB writes, refill path, and first `execve`/page-fault transitions early.

## 6. Boot ABI / Handoff Contract

Recovered kernels use bootloader-style argument passing and may also read fixed command-line symbols in RAM.

Minimum robust handoff:

- `PC`: kernel ELF entry.
- `a0`/`a1`: argc/argv according to kernel expectation.
- Place command line string in RAM, NUL-terminated.
- Place argv table in RAM (pointer(s) + NULL terminator), 32-bit pointer layout.
- Reapply these memory structures on every reset.

Important nuance:

- Some BE-300 kernel boot code starts parsing from `argv[1]` (ignores `argv[0]`), so only supplying `argv[0]=cmdline` can yield an empty kernel command line.
- A robust pattern is:
  - `argv[0] = "vmlinux"` dummy
  - `argv[1] = actual cmdline`
  - `argc = 2`

Fallback compatibility path:

- Seed known command-line globals (for kernels that look at fixed symbols like `arcs_cmdline`/`saved_command_line`) in addition to argv.

## 7. Interrupt Controller (ICU) Contract

Implement enough of VR4131 ICU behavior for Linux `arch/mips/vr41xx` paths:

- Status and mask registers for low/high interrupt groups.
- Write-to-clear behavior for pending bits where expected.
- Software interrupt register behavior.
- Parent IRQ output asserted when `(pending & mask)` indicates any enabled source.

Kernel-facing requirement:

- IRQ line used by timer must be routable and maskable.
- UART IRQ 19 must propagate through ICU to CPU interrupt pending state.

## 8. RTC/Timer Contract

Linux startup requires periodic interrupts for timekeeping/scheduler progress.

Minimum functional behavior:

- Implement elapsed-time counter and at least one periodic compare path.
- Generate periodic interrupt at effective 100 Hz path used by BE-300 kernels (or equivalent path they program).
- Allow interrupt status to be acknowledged/cleared by register write.

If the kernel hangs around `calibrate_delay`/early scheduler bring-up, timer IRQ behavior is usually first suspect.

## 9. UART Contract (Console-Critical)

Kernel serial driver expectations observed for BE-300:

- 8250/16550-compatible register model is sufficient.
- MMIO register spacing: 4-byte stride (`regshift=2` equivalent).
- UART base: `0x0a008680` physical.
- IRQ: 19 via ICU.
- Input clock: 18.432 MHz (matches kernel divisor tables for expected baud rates, e.g. 9600).

Bring-up command line commonly used:

- `console=tty0 console=ttyS0,9600 root=/dev/ram rw init=/linuxrc`

Without UART correctness, you lose direct panic/boot text and debugging slows dramatically.

## 10. Initrd Loading Contract

- Load initrd into RAM without overlapping kernel image.
- Keep alignment conservative (64 KiB alignment worked reliably in tests).
- Pass location via command line:
  - `rd_start=0x80xxxxxx` (kseg0 virtual form of initrd physical base)
  - `rd_size=<bytes>`

Validate size and underflow/overlap checks explicitly.

## 11. Minimal Bring-Up Milestones

### M0: CPU/RAM/reset handoff

Success criteria:
- Kernel image loads and starts executing.
- Reset reliably restarts kernel with same handoff state.

### M1: ICU + timer

Success criteria:
- Early kernel progresses past initial architecture setup and delay calibration.
- Interrupt masking/ack behavior is functional.

### M2: UART console

Success criteria:
- Early kernel log visible on serial console.
- UART IRQ traffic observable.

### M3: initrd + root mount

Success criteria:
- Kernel mounts ramdisk root filesystem.
- `Freeing unused kernel memory` appears.

### M4: userspace transition

Success criteria:
- PID 1 executes successfully (`/linuxrc` or BusyBox init path).
- No repeating `swap_dup`/fault loop.

## 12. Debugging Strategy (Architecture-Oriented)

Instrument these points regardless of emulator base:

1. TLB writes (`EntryHi`, `EntryLo0/1`, derived PFN/PA).
2. TLB refill/page-table walk (faulting VA, selected PTE values).
3. Data/instruction bus faults (PC, VA, PA, access type, MMU mode).
4. Usermode boundary transitions:
   - first `execve`
   - first user instruction fetch
   - first user page fault
5. Interrupt edges:
   - timer source assert/deassert
   - ICU parent IRQ transitions
   - UART IRQ assertions

High-value symptom correlations:

- Kernel reaches root mount then dies on first usermode fetch: inspect user TLB PFN derivation.
- Infinite `swap_dup` warnings: inspect first bad swap entry origin upstream of `swap_duplicate`, not just the check function.

## 13. Compatibility Knobs You Should Keep (Even on Other Emulator Cores)

Expose runtime toggles for:

- PRID forcing.
- Legacy vs strict argv handoff mode.
- Boot timer source/IRQ pin selection and pulse limits.
- Extra tracing categories (TLB/MM/IRQ/UART/syscall).
- Optional temporary compatibility hacks (for bisecting failures), clearly separated from “correct” mode.

This lets you bisect boot blockers quickly without recompiling.

## 14. Known-Good Boot Progress Markers

When things are healthy you should see, in order:

1. CPU/cache identification.
2. RAM map print.
3. `Calibrating delay loop`.
4. Serial driver probe (`ttyS0 ...`).
5. `VFS: Mounted root`.
6. `Freeing unused kernel memory`.
7. PID 1 userspace execution.

If you stop between 6 and 7, treat MMU/TLB/user-page setup as primary suspect.

## 15. Implementation Notes for New Emulator Bases

If you are building on a different emulator framework:

- Start with a generic MIPS core that supports CP0, TLB refill, and little-endian operation.
- Build a board wrapper that only maps the BE-300 physical topology and boot ABI.
- Keep device models independent and testable:
  - ICU unit tests for mask/pending routing.
  - RTC unit tests for compare and periodic interrupts.
  - UART unit tests for reg spacing and divisor behavior.
- Add scripted smoke tests for each milestone before integrating next subsystem.

Avoid optimizing for graphics/storage first; serial + timer + MMU correctness are the real gating path to userspace.

---

For recovered register/address references, use:

- `/Users/jroark/src/linux4be/emulator/design/register_map.json`

For UART-specific notes captured from kernel driver behavior, use:

- `/Users/jroark/src/linux4be/emulator/design/uart_model_notes.md`
