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

**Kernel:** `linux4be20040908/vmlinux` — ELF32 MIPS LE, 2.6.8.1, built 2004-09-08.

**Cross-dev Docker image:** `git@github.com:jroark/mipsel-cross-image.git`
- Provides `mipsel-linux-gnu-gcc` toolchain for building userspace / initramfs.
- Scripts: `build_tcl_kernel.sh`, `build_busybox.sh`, `create_initramfs.sh`.

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
[TLB_INJECT] EPC=0x800015B0 (multiple intno=27 storms)
[CHECKPOINT] do_page_fault (entry)
← STUCK: repeated TLBS injections loop back to run_init_process and eventually
         crash with UC_ERR_READ_UNMAPPED while fetching the exception vector.
```

`af_unix_init`, `packet_init`, `prepare_namespace`, and `run_init_process` now fire.
The remaining blocker occurs after `create_elf_tables()` when the syscall never
returns to user space and execution keeps re-entering the syscall site at
0x800015B0 with intno=27 (TLB store misses).

---

## Confirmed Root Cause of Current Blocker (2026‑02‑26)

The RCU/timer issue is fixed; initcalls complete and `/sbin/init` is located.
The new blocker is the syscall exit path for `run_init_process`:

- After `create_elf_tables`, the init thread should write the user-mode entry
  point into CP0 EPC and execute ERET.
- Instead, repeated intno=27 (TLB store miss) interrupts arrive at
  `run_init_process+0x4`, keeping `pending_excode=8` alive while the kernel
  handles page faults.
- Our instrumentation shows `MTC0 EPC` only ever writes kernel addresses, so the
  user entry never sticks. Eventually Unicorn faults at 0x80000180 with
  `UC_ERR_READ_UNMAPPED` because TLBS injections are happening while the vector
  is already running.

**Key insights from probes:**
- `[MTC0_EPC]` logs show EPC toggling between kernel addresses; no kuseg value
  is written before the TLBS storm.
- `[TLB_INJECT]` confirms we’re manually reinjecting TLBS to reach
  `do_page_fault`, but we don’t yet restore the exception vector cleanly after
  multiple nesting levels.

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

1. **Vector mapping audit:** the UC_ERR_READ_UNMAPPED happens while refetching
   0x80000180. Verify that SDRAM still covers this page after repeated TLBS
   injections, and ensure we’re not remapping over it. If needed, hard-map the
   exception page in machine_create.
2. **Syscall/TLBS sequencing:** only inject TLBS when not already on the general
   exception vector, or extend the save/restore stack so nested TLBS can unwind
   cleanly and restore the pending SYSCALL context.
3. **Track user EPC writes:** enhance the `[MTC0_EPC]` log to flag when a kuseg
   address is written; if it never happens, inspect `start_thread()` in the
   BE-300 kernel (see `kernels/kernel-2.6/`) to determine whether extra fixes
   are needed.
4. **Initramfs follow-up:** once `/sbin/init` runs, we’ll need a proper mipsel
   initramfs (build via the Docker cross-dev image scripts).

---

## Verification Commands

```bash
# Build and run with probes
make -j4 && ./be300 --kernel linux4be20040908/vmlinux > /tmp/be300.out 2> /tmp/be300.err

# Check probe output
grep -E '(RCU_PROBE|CHECKPOINT|INITCALL|PROGRESS)' /tmp/be300.err | head -80
tail -30 /tmp/be300.out

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
   make -j$(nproc)
   ./be300 --kernel linux4be20040908/vmlinux > build/docker_stdout.log \
     2> build/docker_stderr.log
   ```
   The image installs clang/meson/ninja plus mipsel cross-compilers. `PKG_CONFIG_PATH`
   already points at `/work/third_party/unicorn-linux/lib/pkgconfig`.

2. **Unicorn for macOS vs. Linux:**
   - macOS hosts use the prepatched dylib under `third_party/unicorn/`.
   - The container builds Unicorn 2.1.4 from source and installs it into
     `third_party/unicorn-linux/` (Makefile auto-detects the `.so` there).

3. **Logs & artifacts:**
   - Always capture both stdout and stderr from emulator runs (`build/docker_*.log`).
   - RCU/TLBS instrumentation prints to stderr; grep for `CHECKPOINT`, `TLB_INJECT`,
     `MTC0_EPC`, etc.

4. **Kernel sources:**
   - `kernels/kernel-2.6/` holds the patched BE-300 kernel tree used to produce
     `linux4be20040908/vmlinux`. Consult it when symbol hunting or adjusting
     platform-specific code (`arch/mips/vr41xx/...`).

5. **Commit etiquette:** every investigation (successful or not) gets its own
   commit summarizing what happened, what was learned, and next steps, then
   push to `claude/explain-codebase-mm1561dhacl5ikyh-zdk3b`.
