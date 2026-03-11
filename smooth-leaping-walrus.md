# Plan: WinCE NK Init Crash — Investigate & Fix Context Restore Failure

## Context

Steps 1-4 of the original plan are **DONE**. Two plateaus were broken:
1. **CP0 Count stall** — `mfc0 $9` returned 0; fixed by intercepting MFC0 Count and returning `insn_count/2`
2. **CLKSPEEDREG divide-by-zero** — BCU CLKSPEEDREG at PA 0x0F000014 was 0; fixed by setting to 0x020F (~131MHz)

Both WinCE images now advance past SPL into NK init. NK runs ~7M instructions (MMIO setup, delay loops, TLB clearing), then crashes doing a context restore from an **all-zero** thread control block at PA 0x00002200. Exception vectors at PA 0x00000000-0x00000200 are also all zeros — NK never installed them or created threads.

Linux regression: both 2.6 and 2.4 pass after all changes.

**Key discovery from code review:** The codebase already has full kseg0/kseg1 coherence (`shared_alias_active` via `uc_mem_map_ptr`, plus `alias_write_sync_hook` fallback). Exception vectors are seeded before SPL via `seed_wince_exception_vectors()`. So the aliasing theory is likely wrong — the real issue is that NK's init doesn't complete.

---

## Step 1: Quick diagnostics (non-destructive)

### 1a. Check aliasing state and "Illegal instruction" in existing logs

Run WinCE boot and grep stderr for:
```bash
grep -E "ALIAS_MODE|Illegal instruction|insn_invalid" wince3_stall_stderr.log | head -20
```

If `ALIAS_MODE` shows `shared SDRAM backing active` → aliasing works fine.
If `Illegal instruction` appears → Unicorn can't decode something NK executes (likely CACHE instruction, opcode 0x2F).

### 1b. Add BCU unknown-offset logging

**File:** `src/hw/bcu.c`, `bcu_read()` default case

Log each unique unknown BCU offset NK reads. This reveals if NK probes SDRAM config registers that return 0:
```c
default:
    {
        static uint32_t bcu_unknown_log = 0;
        if (bcu_unknown_log < 32u) {
            fprintf(stderr, "[BCU] unknown read offset=0x%04X size=%u -> 0\n",
                    offset, size);
            bcu_unknown_log++;
        }
    }
    return 0;
```

---

## Step 2: CACHE instruction NOP handler (highest priority fix)

**Theory:** MIPS CACHE instruction (opcode 0x2F) is used by NK during init for cache flush/invalidate. If Unicorn's VR5432 model doesn't handle it natively, it hits `insn_invalid_hook` which logs "Illegal instruction" and returns `false` → `uc_emu_stop`. NK's init would be silently aborted.

**Evidence supporting this theory:**
- NK runs ~7M instructions (enough for HW init) but never installs vectors or creates threads
- `insn_invalid_hook` only handles MACC; any other unknown opcode stops emulation
- WinCE NK universally uses CACHE instructions during early init (cache config, D-cache writeback before TLB invalidation)

### Fix in `prid_hook` (line ~2580, after `op` extraction)

```c
/* CACHE instruction (opcode 0x2F): NOP in cacheless emulator.
 * NK uses CACHE extensively during init for I-cache/D-cache management. */
if (op == 0x2Fu) {
    uint64_t next_pc = address + 4u;
    uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
    static uint32_t cache_skip_log = 0;
    if (cache_skip_log < 8u) {
        fprintf(stderr, "[CACHE_NOP] PC=0x%08" PRIX64 " insn=0x%08X\n",
                (uint64_t)(uint32_t)address, insn);
        cache_skip_log++;
    }
    return;
}
```

### Also in `insn_invalid_hook` (line ~7039, before "Unknown instruction" log)

```c
uint32_t inv_op = (insn >> 26) & 0x3Fu;
if (inv_op == 0x2Fu) {  /* CACHE */
    uint64_t next_pc = pc + 4u;
    uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
    return true;
}
```

**Files:** `src/machine.c`

---

## Step 3: Build, test, analyze

```bash
# In Docker
cd /work/build-docker && cmake .. && make -j$(nproc)

# WinCE 3.0
timeout 120s ./be300 --nand ../ce/restore_images/All_nand_300.bin --log-wince-stall \
  > wince3_stdout.log 2> wince3_stderr.log
grep -E "CACHE_NOP|ALIAS_MODE|Illegal|NULL_BAIL|WINCE_STALL\] PC|BCU.*unknown" wince3_stderr.log | head -50

# WinCE 4.0
timeout 120s ./be300 --nand ../ce/restore_images/CE_Net.bin --log-wince-stall \
  > wince4_stdout.log 2> wince4_stderr.log
grep -E "CACHE_NOP|ALIAS_MODE|Illegal|NULL_BAIL|WINCE_STALL\] PC|BCU.*unknown" wince4_stderr.log | head -50

# Linux regression
timeout 180s ./be300 --kernel ../kernels/vmlinux-2.6 > d26.log 2> d26e.log
grep -q "Freeing unused kernel memory" d26.log && echo "2.6 OK" || echo "2.6 FAIL"
timeout 180s ./be300 --cmdline "console=tty0 console=ttyS0,9600 root=/dev/ram" \
  --kernel ../kernels/vmlinux-pgui-demo > d24.log 2> d24e.log
grep -q "Freeing unused kernel memory" d24.log && echo "2.4 OK" || echo "2.4 FAIL"
```

### Outcome decision tree

- **CACHE_NOP lines appear AND NK advances past old crash** → CACHE was the blocker. Continue to next plateau (repeat analyze/fix cycle).
- **CACHE_NOP lines appear but same crash** → CACHE wasn't the only issue. Check BCU unknown reads, add extended NK trace.
- **No CACHE_NOP lines** → Unicorn handles CACHE natively. Move to Step 4.

---

## Step 4: If CACHE fix doesn't resolve — deeper investigation

### 4a. BCU SDRAM configuration registers

NK may read BCU registers to detect SDRAM size. If it gets 0, it may skip memory init → no threads created.

**File:** `src/hw/bcu.c`, `src/hw/bcu.h`

Initialize `bcucntreg1` to indicate 16MB SDRAM bank 0 configured. Exact value TBD from VR4131 datasheet §3 BCU registers and from what the diagnostic logging in Step 1b reveals.

### 4b. Extended NK init trace

Increase the NK trace window or use a circular buffer to capture the last 500 instructions before the crash. Look for:
- Error return values (v0 < 0 or v0 == 0 after a JAL)
- Branches that skip large code blocks (conditional jumps over init functions)
- Any MMIO read that returns an unexpected value

### 4c. CP0 Config register

NK reads and writes CP0 Config at entry (trace entries 4-9). The Config register controls cache mode. If Unicorn's Config doesn't match VR4131 expectations, NK might skip cache-dependent init code. Check what value Unicorn returns for MFC0 Config and whether it needs to be intercepted like PRId.

---

## Step 5: Commit and push

After each fix+regression cycle, commit from host with detailed message covering:
- What was investigated
- What fix was applied
- Whether NK advanced further
- Linux regression status

---

## Constraints (unchanged)
- **FORBIDDEN in machine.c:** No synthetic exception injection, no forced PC redirection, no stall-triggered side effects
- **Allowed:** CACHE NOP handling, hardware register value fixes, interrupt propagation fixes
- Build/test in Docker, commit/push from host
- Linux regression after EACH WinCE fix

## Key files
- `src/machine.c` — prid_hook (~2580), insn_invalid_hook (~7027), null-call handler (~3734), stall diagnostics (~5796)
- `src/hw/bcu.c` / `src/hw/bcu.h` — BCU register model
- `src/bus.c` — bus_init (~536), aliasing setup, MMIO dispatch
