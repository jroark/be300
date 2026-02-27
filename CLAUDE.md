# CLAUDE.md — Working Rules for This Repository

## Commit & Push Policy

**After every attempt — regardless of success or failure — commit and push all changes with a detailed message covering:**
- What was tried
- What the outcome was (success, partial, or failure)
- What was learned or confirmed
- What the next step should be

This applies to: code changes, diagnostic instrumentation, failed experiments, documentation updates, and analysis results.

## Branch

All work goes on: `claude/explain-codebase-mm1561dhacl5ikyh-zdk3b`

Push with: `git push -u origin claude/explain-codebase-mm1561dhacl5ikyh-zdk3b`

---

## Project Context

**Target:** Casio BE-300 (NEC VR4131 MIPS little-endian) emulator in Unicorn.

**Kernels**
- `linux4be20040908/vmlinux`   — ELF32 MIPS LE, 2.6.8.1, built 2004-09-08.
- `kernels/vmlinux`            - ELF 32-bit LSB executable, MIPS, MIPS-II version 1 (SYSV), statically linked, not stripped, too many notes (256)
- `kernels/vmlinux_sdlregtest` - ELF32 MIPS LE, Linux version 2.4.18-mips (mouse@mouse.office.altlinux.ru) (gcc version 3.0.4) #325   20 14:06:02 MSK 2003
- `kernels/vmlinux-mw`         - ELF32 MIPS LE, Linux version 2.4.18-mips (jroark@dhcppc4) (gcc version 3.0.1) #309 Sun May 18 03:01:37 PDT 2003
- `kernels/vmlinux-pgui-demo`  - ELF32
- `kernels/vmlinux-pgui-test1` - ELF32

**Kernel Changes for BE-300**
- `kernels/linux-2.6`

**Cross-dev Docker image:** `git@github.com:jroark/mipsel-cross-image.git`
- Provides `mipsel-linux-gnu-gcc` toolchain for building userspace / initramfs.
- Scripts: `build_tcl_kernel.sh`, `build_busybox.sh`, `create_initramfs.sh`.

**WinCE flash image**
- `BACKUP.bin` - restore image from Casio CDROM

**WinCE ELF loader**
- `linux4be20040908/loader.exe` - CyaCE compiled binary
- `linux4be20040908/cyacecfg.txt` - CyaCE config
- `ce/cyace` - source code for loader

**Things to note**
- originally the kernels were loaded from a running WinCE (warm start, not cold) - hw may have been initialized by WinCE
- None of the test kernels had full hw support

---

## Current Boot Status (as of 2026-02-26)

### Last confirmed stdout output
```
Linux version 2.6.8.1 ...
Calibrating delay loop... 99.84 BogoMIPS
fb0: Casio BE-x00 frame buffer device
RAMDISK / PPP / NFTL init
NET: Registered protocol family 2
IP: routing cache hash table of 512 buckets, 4Kbytes
TCP: Hash tables configured (established 512 bind 512)
RAMDISK: Compressed image found at block 0
VFS: Mounted root (ext2 filesystem) readonly.
Freeing unused kernel memory: 112k freed          ← last line
```

### Checkpoint evidence (from latest run)
```
[CHECKPOINT] rest_init
[CHECKPOINT] init (kernel thread)
[CHECKPOINT] do_pre_smp_initcalls
[CHECKPOINT] do_basic_setup
[CHECKPOINT] do_initcalls
[INITCALL] #01–#60  (packet_init = #60 = 0x802864d8)
[CHECKPOINT] prepare_namespace
[CHECKPOINT] run_init_process (entry)   a0="/sbin/init"
[CHECKPOINT] create_elf_tables (entry)
[EXC_SUSPEND] reason=tlb_store PC=0x800015B4
[INTR27] STATUS=0x1000FF01 PC=0x800015B4 pending_excode=0 pending_epc=0x00000000
← STUCK: endless intno=27 (TLB store refill) storms at PC=0x800015B4 while the
         syscall never retires and no page-fault handler ever fires.
```

`af_unix_init`, `packet_init`, `prepare_namespace`, and `run_init_process` all
fire. The remaining blocker occurs after `create_elf_tables()` when the execve
syscall never returns to user space and execution keeps re-entering the syscall
site at 0x800015B0. The new instrumentation shows that each intno=27 arrives
with `pending_excode=0`, so the kernel is looping entirely inside the TLB
refill handler without ever escalating to `do_page_fault`.

---

## Confirmed Root Cause of Current Blocker (2026‑02‑26)

The RCU/timer issue is fixed; initcalls complete and `/sbin/init` is located.
`run_init_process` successfully invokes `create_elf_tables`, but the execve
syscall never reaches user mode. With the new exception plumbing:

- Manual TLBS reinjection has been removed. intno=27 now arrives with
  `pending_excode=0`, so refills execute entirely inside the kernel’s TLB miss
  handler.
- `[PF_PROBE]` never fires and even the checkpoint hook on `do_page_fault`
  remains silent, proving that the kernel never escalates the fault to the
  high-level C handler. The fast refill path keeps trying (and failing) to fill
  the entry.
- `[MTC0_EPC]` still only records kernel EPC values (0x8000AE64,
  0x800405C4, …). No kuseg EPC write is observed before the refill storm, so
  `start_thread()` is never given a chance to install the user entry point.
- The init thread loops forever at PC=0x800015B4 while intno=27 fires millions
  of times and `pending_epc` remains zero, confirming we’re stuck in the refill
  hardware path rather than the software page-fault path.

**Most likely cause:** the refill handler is walking the page tables but never
managing to produce a writable TLB entry (e.g., the PTE’s dirty bit never gets
set or the emulator isn’t honoring the guest’s TLB writes). Until the refill
completes, `/sbin/init` can’t be mapped and the syscall continues to hammer the
same address.

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

## Next Steps (Priority Order)

1. **Instrument the TLB refill handler:** add hooks on the exception vectors
   (0x80000000, 0x80000080, 0x80000180) to log BadVAddr/EntryHi/EntryLo writes.
   We need to see whether the kernel is attempting to write dirty/writable PTEs
   and whether `tlbwi` is executing.
2. **Trace guest TLB writes:** add hooks in `machine.c` that watch for mtc0 to
   CP0 registers 0/2 and for the `tlbwi` instruction. Dump the values being
   written so we can confirm the emulator sees valid PTEs and ASIDs.
3. **Verify emulator TLB state:** use Unicorn’s `uc_ctl` APIs (or temporary
   instrumentation) to query whether tlbwi actually updates internal mappings.
   If not, we may need to patch Unicorn or emulate the refill by directly
   mapping the physical page when the kernel writes a TLB entry.
4. **Initramfs follow-up:** once `/sbin/init` finally runs, we’ll still need a
   proper mipsel initramfs (build via the Docker cross-dev image scripts).

---

## Verification Commands

```bash
# Build and run tests
mkdir -p build && cd build
cmake .. -DCMAKE_OSX_ARCHITECTURES=arm64  # arm64 for M1/M2/M3 Macs
make -j$(sysctl -n hw.ncpu)
./test_basic

# Run emulator
./be300 --kernel ../linux4be20040908/vmlinux

# Check probe output
grep -E '(RCU_PROBE|CHECKPOINT|INITCALL|PROGRESS)' /tmp/be300.err | head -80

# Symbol lookup
nm linux4be20040908/vmlinux | grep <symbol>

# Disassembly
llvm-objdump -d --start-address=0x<VA> --stop-address=0x<VA+N> linux4be20040908/vmlinux
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
   mkdir -p build && cd build
   cmake ..
   make -j$(nproc)
   timeout 120s ./be300 --kernel ../linux4be20040908/vmlinux \
     > docker_stdout.log 2> docker_stderr.log
   ```
   The image installs clang/meson/ninja plus mipsel cross-compilers. `PKG_CONFIG_PATH`
   already points at `/work/third_party/unicorn-linux/lib/pkgconfig`, and the
   base packages now include `gdb`, `gdb-multiarch`, `strace`, and `ltrace` for
   cross-debugging.

2. **Unicorn for macOS vs. Linux:**
   - macOS hosts use the system-installed or Homebrew unicorn (found via CMake).
   - The container builds Unicorn 2.1.4 from source and installs it into
     `third_party/unicorn-linux/` (CMake fallback auto-detects it).

3. **Logs & artifacts:**
   - Always capture both stdout and stderr from emulator runs (`docker_*.log`).
   - RCU/TLBS instrumentation prints to stderr; grep for `CHECKPOINT`, `TLB_INJECT`,
     `MTC0_EPC`, etc.

4. **Kernel sources:**
   - `kernels/kernel-2.6/` holds the patched BE-300 kernel tree used to produce
     `linux4be20040908/vmlinux`. Consult it when symbol hunting or adjusting
     platform-specific code (`arch/mips/vr41xx/...`).

5. **Commit etiquette:** every investigation (successful or not) gets its own
   commit summarizing what happened, what was learned, and next steps, then
   push to `claude/explain-codebase-mm1561dhacl5ikyh-zdk3b`.
