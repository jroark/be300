# Surgical MIPS16 Support: Extract from GXemul into be300-framebuffer

## Context

The BE-300 boot ROM has 34 MIPS16 functions (5.5KB at offsets 0xC20-0x219B) that GXemul cannot execute. The ROM's boot dispatcher at 0x9FC00C21 (MIPS16) populates the OEMInit callback table and resume_ctx before NK.exe starts. Without it, the WinCE kernel cold-start can't fully initialize.

A version of GXemul at `/Users/jroark/src/GXemul/` has MIPS16 support implemented as a **slow interpreter bypass** — when `cpu->cd.mips.mips16` is set, the dyntrans loop calls `mips_cpu_interpret_mips16_SLOW()` instead of the IC cache. This is the same pattern already used for ARM Thumb at line 261-269 of our `cpu_dyntrans.c`. The MIPS16 interpreter is self-contained (1,426 lines) and covers all instructions the ROM uses.

## Files to Copy (from /Users/jroark/src/GXemul/)

### 1. `src/cpus/cpu_mips16.c` → `gxemul/src/cpus/cpu_mips16.c`
- 1,426 lines, self-contained slow interpreter
- Contains: disassembler (`mips_cpu_disassemble_instr_mips16`), interpreter (`mips_cpu_interpret_mips16_SLOW`), helper functions (`m16_fetch`, `m16_load_word`, etc.)
- Dependencies: `cpu.h`, `cop0.h`, `cpu_mips.h`, `machine.h`, `memory.h`, `misc.h`, `opcodes_mips16.h`, `symbol.h` — all already in our repo except `opcodes_mips16.h`
- The `symbol.h` include is for optional function call tracing — stub or `#ifdef` out if needed

### 2. `src/include/opcodes_mips16.h` → `gxemul/src/include/opcodes_mips16.h`
- 151 lines, opcode definitions and register map
- No external dependencies

## Files to Modify (in be300-framebuffer)

### 3. `gxemul/src/include/cpu_mips.h` — Add mips16 field and declarations

At line 219 equivalent (inside the `struct mips_coproc_data` or wherever the MIPS CPU state fields are), add:

```c
int     mips16;     /*  1 = currently in MIPS16 mode  */
```

Find the struct by looking for `gpr[N_MIPS_GPRS]` — the `mips16` field goes nearby. In the GXemul source it's at line 219, right after `rmw` and before `resume`.

Also add function declarations (near other MIPS function declarations):

```c
int mips_cpu_interpret_mips16_SLOW(struct cpu *cpu);
int mips_cpu_disassemble_instr_mips16(struct cpu *cpu, unsigned char *ib,
    int running, uint64_t dumpaddr, int bintrans);
```

### 4. `gxemul/src/cpus/cpu_dyntrans.c` — Add MIPS16 dispatcher

After the interrupt check block (after line 259 `}` closing `if (!single_step)`), add the MIPS16 bypass before the ARM Thumb bypass:

```c
#ifdef DYNTRANS_MIPS
    if (cpu->cd.mips.mips16) {
        int m16_count = 0;
        while (cpu->cd.mips.mips16 && cpu->running &&
            m16_count < N_SAFE_DYNTRANS_LIMIT) {
            if (!mips_cpu_interpret_mips16_SLOW(cpu))
                break;
            m16_count++;
        }
        return m16_count > 0 ? m16_count : 1;
    }
#endif
```

### 5. `gxemul/src/cpus/cpu_mips_instr.c` — Add mode switching in JR/JALR

The GXemul source shows exactly what changes are needed. Each JR/JALR variant checks bit 0 of the target address:

**X(jr)** at line 987 — replace the body after `if (likely(...))`:
```c
if (rs & 1) {
    cpu->cd.mips.mips16 = 1;
    cpu->pc = rs & ~(MODE_int_t)1;
    cpu->delay_slot = NOT_DELAYED;
    cpu->cd.mips.next_ic = &nothing_call;
} else {
    cpu->pc = rs;
    cpu->delay_slot = NOT_DELAYED;
    quick_pc_to_pointers(cpu);
}
```

**X(jr_ra)** at line 1001 — same pattern, checking `rs & 1`.

**X(jr_ra_addiu)** at line 1015 — same pattern.

**X(jr_ra_trace)** at line 1025 — same pattern.

Any other JR variants need the same treatment.

### 6. `gxemul/src/cpus/cpu_mips.c` — Exception handler mode preservation

In the exception handler (search for `reg[COP0_EPC] = cpu->pc`), add MIPS16 mode bit to EPC:

```c
if (cpu->cd.mips.mips16)
    reg[COP0_EPC] |= 1;  /* Mark return as MIPS16 */
```

Also when entering exception handler, clear MIPS16 mode (exceptions always execute in MIPS32):

```c
cpu->cd.mips.mips16 = 0;
```

### 7. `gxemul/src/cpus/cpu_mips_instr.c` — ERET mode restoration

In the ERET instruction handler, restore MIPS16 mode from EPC bit 0:

```c
cpu->cd.mips.mips16 = (cpu->pc & 1) ? 1 : 0;
cpu->pc &= ~(uint64_t)1;
```

### 8. Build system — `CMakeLists.txt`

Add `gxemul/src/cpus/cpu_mips16.c` to the source file list.

## What NOT to Change

- Do NOT modify the IC entry structure or DYNTRANS_IC_ENTRIES_PER_PAGE
- Do NOT modify PC-to-IC mapping (MIPS_PC_TO_IC_ENTRY)
- Do NOT try to JIT MIPS16 — the slow interpreter is sufficient for the 34 ROM functions

## Verification

1. Build: `cd build-host && cmake .. && make -j$(nproc)`
2. Linux kernel regression (must still boot to userspace):
   ```
   gtimeout 20s ./be300 --cmdline "console=tty0 root=/dev/ram" \
     --kernel ../kernels/vmlinux-pgui-demo
   ```
3. WinCE cold boot — the ROM's MIPS16 boot dispatcher at 0x9FC00C21 should now execute:
   ```
   gtimeout 60s ./be300 --nand ../ce/restore_images/All_nand_300.bin \
     --wince-cold-boot --log-mmio > cold_stdout.log 2> cold_stderr.log
   ```
   Check stderr for MIPS16 execution (the ROM dispatcher will call MIPS32 helpers via JALX at 0x9FC00464, 0x9FC00834, etc.)
4. Verify the ROM boot dispatcher runs by checking if resume_ctx at PA 0x2200 gets populated with values different from our emulator-forced values

## Key Addresses for Testing

- ROM MIPS16 entry: 0x9FC00C21 (bit 0 = MIPS16 flag, actual code at 0x9FC00C20)
- ROM MIPS16 region: 0x9FC00C20-0x9FC0219B
- MIPS32 helpers called via JALX: 0x9FC00464, 0x9FC00834, 0x9FC00888, 0x9FC00980, 0x9FC009BC, 0x9FC00BC0
- MIPS32 trampoline: 0x9FC00C04 (pops s0,s1,a0, JR a0)

## Integration with Current Cold Boot Path

Once MIPS16 works, the ROM boot sequence should run naturally:
1. ROM reset vector → MIPS32 init
2. ROM calls boot dispatcher at 0x9FC00C21 via JALR (bit 0 triggers MIPS16 mode)
3. Boot dispatcher runs in MIPS16, populates callback table, resume_ctx, dispatch tables
4. Boot dispatcher returns to ROM MIPS32 code
5. ROM jumps to NK.exe
6. NK.exe pre-WAIT init → WAIT
7. Post-WAIT OAL restores from ROM-populated resume_ctx
8. Kernel cold-start runs with proper RA from ROM

This would eliminate the need for the emulator's callback table injection, resume_ctx population, and cold-start jump hack — the ROM does it all natively.
