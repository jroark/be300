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

## Current Boot Status (as of 2026-02-25)

### Last confirmed stdout output
```
Linux version 2.6.8.1 ...
Calibrating delay loop... 99.84 BogoMIPS
fb0: Casio BE-x00 frame buffer device
RAMDISK / PPP / NFTL init
NET: Registered protocol family 2        ← last line
```

### Checkpoint evidence (from latest run)
```
[CHECKPOINT] rest_init
[CHECKPOINT] init (kernel thread)
[CHECKPOINT] do_pre_smp_initcalls
[CHECKPOINT] do_basic_setup
[CHECKPOINT] do_initcalls
[INITCALL] #01–#58  (inet_init = #58 = 0x80285ea0)
[CHECKPOINT] inet_init: JAL sock_register
[CHECKPOINT] inet_init: JAL inet_register_protosw (loop)
← STUCK: PC=0x80000180 for 1000M+ instructions
```

`af_unix_init`, `packet_init`, `arp_init`, `ip_init`, `tcp_init`, `prepare_namespace`,
`run_init_process` — **none of these ever fire**.

---

## Confirmed Root Cause of Current Blocker

`inet_register_protosw` tail-calls `synchronize_net` → `synchronize_kernel`
→ `call_rcu` + **`wait_for_completion`**.

`wait_for_completion` blocks the init thread until the RCU grace period completes.
The grace period requires `rcu_check_callbacks` to be called from:

```
timer_interrupt → do_timer → update_process_times → scheduler_tick → rcu_check_callbacks
                                                                      → __tasklet_schedule
ll_timer_interrupt (on return) → do_softirq → rcu tasklet → rcu_process_callbacks
                                                           → complete() → wakes init
```

**Suspected issue:** `tick_jiffies_hack` (in `machine_run`) advances `jiffies` directly
in RAM every batch, bypassing the kernel's timer interrupt handler.
As a result `do_timer` / `scheduler_tick` / `rcu_check_callbacks` may never
be called, or the call rate is too low to complete the grace period before the
emulator run limit is hit.

**RCU diagnostic probes added (multi-fire, up to 8 each):**
| Address    | Symbol                 |
|------------|------------------------|
| 0x801ab590 | `wait_for_completion`  |
| 0x800379a8 | `do_timer`             |
| 0x800428b8 | `rcu_check_callbacks`  |
| 0x80042780 | `rcu_process_callbacks`|
| 0x8000eb30 | `timer_interrupt`      |

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

1. **Run RCU probes** — confirm `wait_for_completion` fires and whether
   `timer_interrupt` / `rcu_check_callbacks` / `rcu_process_callbacks` are reached.

2. **If `rcu_check_callbacks` fires but `rcu_process_callbacks` does not:**
   - The tasklet is being scheduled but `do_softirq` isn't processing it.
   - Check `do_softirq` condition in `ll_timer_interrupt` (0x8000edec).

3. **If `timer_interrupt` fires but `rcu_check_callbacks` does not:**
   - `scheduler_tick` may not be calling `rcu_check_callbacks` due to a condition
     check failing (user_mode flag, preempt_count, etc.).

4. **If neither `timer_interrupt` nor `do_timer` fire:**
   - `inject_hw_irq_if_pending` may not be generating the right interrupt or
     the kernel may be dispatching it to the wrong handler.
   - Consider removing `tick_jiffies_hack` and letting the real timer interrupt
     path handle jiffies via `do_timer`.

5. **Once RCU unblocks:** `af_unix_init`, `packet_init`, `prepare_namespace`
   should fire. If they don't, check `do_softirq` path or task wakeup.

6. **After initcalls complete:** `prepare_namespace` will try to mount root.
   The kernel found an initrd but freed it ("looks like an initrd → freed 472k").
   Need to provide a proper mipsel initramfs (use Docker cross-dev image to
   build BusyBox + `create_initramfs.sh`).

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
