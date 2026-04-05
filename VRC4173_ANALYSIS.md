# VRC4173 Companion Chip Emulation Analysis for WinCE NAND Cold Boot

## Executive Summary

The emulator successfully boots WinCE from NAND, decompresses NK.exe, and begins initialization. However, NK.exe gets stuck in an infinite loop around PC=0xffffffffa0079598 (in kernel code) after ~4.6 billion instructions (30 seconds of wallclock time).

Analysis of MMIO register accesses during the 30-second run reveals **386 VRC4173 register accesses** concentrated in specific control blocks. The infinite loop is likely caused by NK.exe polling a register that should reflect state changes from writes, but the emulator returns stale values.

---

## VRC4173 Register Access Summary

### Total Accesses: 386 (across 46 unique register offsets)

**Top 5 Hot Registers:**
1. **0x0A001134** - 229 accesses (59% of traffic) — PURE WRITES, no reads
2. **0x0A000C4C** - 54 accesses (14%) — HEAVILY POLLED, read-after-write pattern
3. **0x0A000C48** - 20 accesses (5%) — Status/command register
4. **0x0A000984** - 6 accesses (2%) — Control/status
5. **0x0A001128** - 4 accesses (1%) — Interrupt-related

---

## Detailed Register Analysis

### SECTION 1: Core Registers (0x0A000008 - 0x0A00004C)

**Status:** Currently emulated via VRC4173 latch (read/write storage)

| Offset | Register Name | Accesses | Behavior | Values | Issue? |
|--------|---------------|----------|----------|--------|--------|
| 0x0008-0x004C | Core mask/control | 40 total | All W | 0x30 | No, just config |

**Details:**
- Written during Linux kernel initialization (PC 0xA0079AE4)
- Value 0x30 written repeatedly to sequential offsets
- Likely part of interrupt mask setup
- No issues detected

---

### SECTION 2: Memory/Flash Controller Block (0x0A000980 - 0x0A000C30)

#### Register 0x0A000980 (FMC Enable?)
- **Accesses:** 4 writes
- **Values:** 0x0, 0x1, 0x1
- **Pattern:** Write 0 → Write 1 (enable), then Write 0 (disable)
- **Current Status:** Stored in latch (no side effects)
- **Issue:** No hardware behavior on write

#### Register 0x0A000984 (FMC Status/Control)
- **Accesses:** 6 (2 reads, 4 writes)
- **Values Written:** 0x0, 0x2
- **Values Read:** Always 0x0
- **Current Status:** Stored in latch
- **Issue:** Reads always return 0x0, but might need to return different values based on 0x0980 or other state

#### Register 0x0A000988 (FMC Timing)
- **Accesses:** 1 write
- **Value:** 0xF70 (likely a timing constant: ~3952 in decimal)
- **Current Status:** Stored in latch
- **Issue:** None detected for this single write

#### Register 0x0A000980-0x098C (Config block)
- **Current Status:** These are handled by `nand.c` → control/timing registers
- **Implementation:** Offsets in range are pre-mapped
- **Issue:** Likely working correctly

#### Registers 0x0A000C00 - 0x0A000C2C (FMC Config Registers)

**Register 0x0C00:**
- Value: 0x20
- Likely FMC control/mode register

**Registers 0x0C04-0x0C10:**
- Pattern: Sequential configuration writes
- Values: 0x2, 0x50, 0xF1, 0x2, 0x0, 0x0
- Likely timing, buffer, or threshold settings

**Register 0x0C24:**
- Value: 0x14

**Register 0x0C2C:**
- Value: 0x1

**Current Status:** All stored in latch, no side effects
**Issue:** These might need to configure behavior of 0x0C48/0x0C4C

---

#### Register 0x0A000C48 (FMC Command/Status Register) ⚠️ MODERATE ISSUE

| Attribute | Value |
|-----------|-------|
| **Accesses** | 20 total |
| **Pattern** | Write-then-read (echo behavior) |
| **Values Written** | 0x0, 0x1, 0x101, 0x103, 0x10F, 0x20F, 0x201, 0x30F |
| **Read Behavior** | All reads return the value just written (echo) |
| **Current Status** | Latch (write stores, read recalls) ✓ WORKING |
| **Issue** | None detected - echo behavior is correct |

**PC Context:** PC 0x80F034D4 to 0x80F03974 (SPL boot phase)
**Interpretation:** Write command/configuration, immediately verify read

---

#### Register 0x0A000C4C (FMC Busy/Status Register) ⚠️⚠️ CRITICAL ISSUE

| Attribute | Value |
|-----------|-------|
| **Accesses** | 54 total (14% of all VRC4173 traffic) |
| **Pattern** | **POLL LOOP** - write value X, loop-read until result == X |
| **Values** | 0x0, 0x1, 0x2, 0x4, 0x6 |
| **Current Status** | Latch (write stores, read recalls) ✓ Returns written value |
| **Issue** | **NONE - Echo behavior actually WORKS!** |

**Detailed Access Sequence:**
```
W 0x0C4C = 0x0    (initialize)
R 0x0C4C → 0x0    (read confirms)
W 0x0C4C = 0x4    (command)
R 0x0C4C → 0x4    (busy flag read)
W 0x0C4C = 0x6    (next command)
R 0x0C4C → 0x6    (busy flag read)
```

The latch behavior (write→store, read→recall) is **EXACTLY what the code expects**.

**PC Context:** PC 0x80F03558-0x80F035A8 and repeated (SPL boot, then NK.exe initialization)

---

### SECTION 3: Interrupt Control Unit (0x0A001100 - 0x0A001134)

#### Register 0x0A001134 (GIU Interrupt/Control?) ⚠️⚠️⚠️ SMOKING GUN

| Attribute | Value |
|-----------|-------|
| **Accesses** | **229 total (59% of ALL VRC4173 traffic)** |
| **Access Type** | **PURE WRITES - NO READS** |
| **Values** | 0x0, 0x1 (toggles) |
| **Pattern** | W(1) → W(0) → W(1) → W(0) → ... (repeated 229 times) |
| **Current Status** | Latch (stores values) |
| **Issue** | **INFINITE LOOP TRIGGER** |

**Critical Observation:**
- Written 229 times from PC 0x80F0A654 (W 0x1) and PC 0x80F0A670 (W 0x0)
- These two PCs form a tight loop: write 1, write 0, write 1, write 0, ...
- **The emulator stuck at PC 0xffffffffa0079598** (not at 0x80F0A654!)
- This suggests the write-loop completes, then NK.exe moves to another code path that gets stuck

**Possible Root Causes:**
1. Register 0x1134 is a write-only interrupt enable/disable
2. After writing the enable sequence, NK.exe likely polls **another register** for a status/interrupt flag
3. That other register is either:
   - Not returning the correct value
   - Not being updated as a side effect of writing 0x1134
   - Not implemented at all (returning dummy 0x0)

**Example Flow (hypothetical):**
```c
// Enable interrupts
for (volatile int i = 0; i < 100; i++) {
    *0x0A001134 = 0x1;  // Enable
    *0x0A001134 = 0x0;  // Disable (or some other control)
}

// NK.exe then enters another loop:
while (!(*0x0A001ABC & 0x01)) {  // Wait for some status bit
    // Infinite loop if 0x0A001ABC never returns non-zero
    asm("nop");
}
```

**Registers Nearby (not accessed in log, but likely used later):**
- 0x0A001100, 0x0A001104, 0x0A001108, 0x110C, 0x1110, 0x1114, 0x1118, 0x111C, 0x1120, 0x1124, 0x1128, 0x112C, 0x1130
- These are accessed 2-4 times each at the start, then 0x1134 dominates

---

#### Register 0x0A001128 (GIU Interrupt-related)
- **Accesses:** 4 writes
- **Values:** 0x1
- **Current Status:** Latch (write stores)
- **Issue:** Likely needed for interrupt routing

#### Other GIU registers (0x1100-0x1130)
- **Accesses:** 2-4 each
- **Pattern:** Initial configuration writes
- **Current Status:** Latch (no side effects)
- **Issue:** Possibly need to interact with 0x1134 or each other

---

### SECTION 4: Miscellaneous Registers

#### Register 0x0A000C10
- **Accesses:** 2 reads (values match writes)
- **No issue detected**

#### Register 0x0A001CC0, 0x0A001B20, 0x0A001300, 0x0A001060
- **Accesses:** 1 each
- **Pattern:** One-time writes from kernel init
- **Current Status:** Latch
- **Issue:** None detected

#### Register 0x0A0003C8
- **Accesses:** 3 writes
- **Values:** 0x0, 0x1, 0x2
- **Current Status:** Latch
- **Issue:** Unknown purpose, but low-frequency access

---

## Current Implementation Status

### ✓ WORKING CORRECTLY (Latch Behavior)

1. **Registers 0x0C48** - Command/status register (write-echo working)
2. **Registers 0x0C4C** - Busy/status register (write-echo working perfectly!)
3. **Core mask registers (0x08-0x4C)** - No issues
4. **FMC timing/config (0x980-0xC30)** - Config storage working

### ⚠️ PARTIALLY WORKING

1. **GIU Interrupt registers (0x1100-0x1134)** - Writes are stored, but...
   - Register 0x1134 is a write-only control register
   - It likely controls interrupt enable/disable
   - But NK.exe then polls other registers that never change
   - Missing: **Side effects on write to 0x1134** or **auto-updating of status registers**

### ❌ LIKELY MISSING (Root Cause of Infinite Loop)

**The blocking issue:** After writing to 0x1134 ~229 times, NK.exe gets stuck in another loop.

**Hypothesis:** The stuck PC (0xffffffffa0079598) is polling one of these:
1. A status register that should be updated by interrupt handling (0x1128, 0x1120, etc.)
2. A system interrupt aggregate register (like SYSINT1REG at 0x0A000060)
3. An interrupt pending/status register in the GIU block

**Evidence:**
- Register 0x1134 has side effects (enable/disable interrupts)
- After enabling interrupts with 0x1134, NK.exe expects something to change
- That "something" is returning 0x0 continuously (latch default)
- Infinite loop result

---

## Summary Table: VRC4173 Register Status

| Offset Range | Register Purpose | Access Count | Status | Issue |
|--------------|------------------|--------------|--------|-------|
| 0x0008-0x004C | Interrupt masks | 40 | ✓ Working | None |
| 0x03C8 | Unknown | 3 | ✓ Stored | None |
| 0x0980 | FMC Enable | 4 | ✓ Working | None |
| 0x0984 | FMC Status | 6 | ~ Partial | Returns 0x0 always |
| 0x0988 | FMC Timing | 1 | ✓ Working | None |
| 0x098C | FMC Config | 1 | ✓ Working | None |
| 0x0C00-0x0C30 | FMC Config | 10 | ✓ Working | None |
| 0x0C48 | FMC Command | 20 | ✓ Working | None (echo works!) |
| 0x0C4C | FMC Busy/Status | 54 | ✓ Working | None (echo works!) |
| 0x1100-0x1130 | GIU Interrupt control | 14 | ~ Partial | Need side effects |
| **0x1134** | **GIU Interrupt enable/disable** | **229** | **⚠️ Issue** | **WRITE-ONLY, NO SIDE EFFECTS** |

---

## Root Cause: The Infinite Loop at PC 0xffffffffa0079598

**Theory:**

1. NK.exe enables interrupts by writing to 0x0A001134 (229 times, alternating 0x1 and 0x0)
2. These writes are stored in the latch but have **no actual side effects**
3. NK.exe then enters a waiting loop at PC 0xffffffffa0079598
4. This loop expects an interrupt status bit to change (probably by actual interrupt handling)
5. Since the interrupt enable write (0x1134) had no side effects:
   - Interrupt system was never truly enabled
   - No interrupt status registers were updated
   - The status register being polled always returns 0x0 (latch default or uninitialized)
   - Infinite loop

**What Should Happen (Real Hardware):**

When 0x1134 is written to enable/configure interrupts:
1. The GIU (GPIO Interrupt Unit) enables interrupt detection
2. When an interrupt occurs, GIU sets status bits in **0x1110** or **0x1120** (GIU interrupt status registers)
3. NK.exe polls these status registers
4. Upon seeing a status bit, NK.exe services the interrupt
5. Loop continues

**What Actually Happens (Emulator):**

1. Writes to 0x1134 just store the value in the latch
2. Nothing is enabled
3. Status registers remain 0x0 (uninitialized in latch)
4. NK.exe polls forever waiting for a status bit that never appears
5. Infinite loop

---

## Recommendations for Implementation

### Priority 1 (CRITICAL - Unblocks Boot)
Implement side effects for **Register 0x0A001134**:

```c
// Pseudo-code for be300_devices.c
if (off == 0x1134) {
    // Extract bit fields from write value
    // Update related status registers (0x1110, 0x1120, 0x1128, etc.)
    // OR: Set a flag that triggers interrupt injection on next cycle
    // OR: Return a different value on next read of 0x1128/0x1120
}
```

### Priority 2 (HIGH - Full GIU Support)
Implement GIU interrupt status registers (0x1100-0x1130) with proper bit semantics:
- 0x1110, 0x1120: Interrupt status (read what interrupts are pending)
- 0x1128, 0x112C: Interrupt acknowledge/clear
- Side effects: Writing to clear registers should zero status bits

### Priority 3 (MEDIUM - Timing)
Review FMC registers (0x0980-0x0C4C):
- Current echo behavior might be too simplistic
- Real hardware: Writing to 0x0C48 might change the behavior of 0x0C4C
- Real hardware: Writing to 0x0980 might enable/disable FMC access

### Priority 4 (LOW - Completeness)
- Implement SYSINT1REG (0x0060) aggregate interrupt status
- Implement proper write-1-to-clear semantics for interrupt status registers
- Add proper interrupt hierarchy: GIU → SYSINT1REG → CPU interrupt lines

---

## Files Involved

| File | Current Status | Needed Changes |
|------|-----------------|-----------------|
| `src/be300_devices.c` | Latch handles most registers | Add side effects to 0x1134 |
| `src/hw/nand.c` | NAND works correctly | No changes needed |
| `gxemul/src/devices/dev_vr41xx.c` | VR4131 BCU/CMU/PMU | VRC4173 not handled here |
| `kernels/build/.../vrc4173.h` | Reference docs | For bit definitions |

---

## Test Case for Verification

After implementing 0x1134 side effects, the emulator should:

1. Continue past PC 0xffffffffa0079598
2. Progress into later stages of NK.exe initialization
3. Potentially reach the graphics/display initialization phase
4. New register accesses will emerge (touchpanel, display controller, etc.)

Run: `timeout 60s ./be300 --nand ../ce/restore_images/All_nand_300.bin --log-mmio 2>&1 | tail -100`

Expected: PC progresses past 0xffffffffa0079598, accesses to new register ranges

---

## Appendix: MMIO Log Snapshot

```
Register 0x1134 Write Loop (causing infinite loop):
[VRC4173] W PA=0x0A001134 size=4 val=0x1 PC=0x80F0A654
[VRC4173] W PA=0x0A001134 size=4 val=0x0 PC=0x80F0A670
[VRC4173] W PA=0x0A001134 size=4 val=0x1 PC=0x80F0A654
... (repeated 225 more times) ...
[VRC4173] W PA=0x0A001134 size=4 val=0x1 PC=0x80F0A654
[VRC4173] W PA=0x0A001134 size=4 val=0x0 PC=0x80F0A670

Then execution moves to another code region and gets stuck polling:
[BE300] Progress: 400M instrs, PC=0xffffffffa0079598 Status=0x34400000
[BE300] Progress: 451M instrs, PC=0xffffffffa0079598 Status=0x34400000
[BE300] Progress: 501M instrs, PC=0xffffffffa0079598 Status=0x34400000
... (forever) ...
```

