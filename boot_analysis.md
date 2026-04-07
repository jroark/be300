# WinCE Cold Boot Emulation State Analysis

## Current Boot Progress

The BE-300 emulator has successfully implemented cold boot through:
1. **ROM reset vector** → executes patched BEV exception handlers
2. **SPL (Secondary Program Loader)** → runs NAND decompression
3. **NK.exe decompression** → binary loaded to PA 0x60000
4. **NK.exe entry detected** → jumps to 0x80076B50

However, NK.exe initialization **STALLS** after "Initializing..." splash screen and never reaches the touch calibration screen.

## Critical Issue: The STANDBY Redirect Problem

### What the Code Shows

In `/Users/jroark/src/be300-framebuffer/src/machine_be300.c` at line 876-892, there's a **COMMENTED-OUT critical patch**:

```c
if (!m->wince.cold_boot_redirected && m->nand_data) {
    // ... detect NK.exe entry ...
    if (entry_pa >= 0x60000u && entry_pa < 0x100000u) {
        /*jpatch[0] = (j_cold >>  0) & 0xFF;
        jpatch[1] = (j_cold >>  8) & 0xFF;
        jpatch[2] = (j_cold >> 16) & 0xFF;
        jpatch[3] = (j_cold >> 24) & 0xFF;
        uint64_t standby_va = 0xffffffff80079598ULL;
        m->cpu->memory_rw(m->cpu, m->cpu->mem,
            standby_va, jpatch, 4, MEM_WRITE, CACHE_NONE);
        m->cpu->invalidate_translation_caches(
            m->cpu, 0x79000, INVALIDATE_PADDR);
        m->wince.cold_boot_copy_done = true;
        m->nand.wince_mode = true;
        fprintf(stderr,
            "[BE300] *** COLD START PATCH:..."
        wince_boot_note_cold_boot_redirect(
            m, "standby-patched-to-cold-start");*/
    }
}
```

**This code is entirely disabled.** Meanwhile, an **identical functional patch** exists and is ACTIVE at line 998-1015 in the same file.

### The Two Patches

#### INACTIVE PATCH 1 (line 876-892):
- **Condition**: `!m->wince.cold_boot_redirected && m->nand_data` + probes PA 0x24FC
- **Trigger point**: Early loop probe that checks PA 0x24FC for entry point
- **Detection**: Sets `m->wince.cold_boot_copy_done = true` + `m->nand.wince_mode = true`

#### ACTIVE PATCH 2 (line 998-1015):
- **Condition**: `!m->wince.cold_boot_copy_done && m->nand_data`
- **Trigger point**: When PC reaches NK.exe entry range (PA 0x60000-0x100000)
- **Detection**: Sets `m->wince.cold_boot_copy_done = true` + `m->nand.wince_mode = true`
- **Behavior**: **FIRES AND PATCHES STANDBY** ✓

Both patches are trying to accomplish the same goal: patch the STANDBY instruction at VA 0x80079598 with a JR to the cold-start entry point 0x8007B398.

**KEY POINT**: Patch 2 actually executes and patches STANDBY, so NK.exe SHOULD be jumping to the cold-start sequence, but it's apparently not reaching the touch calibration screen.

## Boot State Tracking: The Vector Ready Gate

The wince_boot.c file implements a sophisticated state machine for tracking exception vector ownership:

### Vector Owner States
1. **WINCE_VECTOR_NONE** (0): Initial state
2. **WINCE_VECTOR_SYNTHETIC** (1): ROM has synthetic handlers in place
3. **WINCE_VECTOR_GUEST** (3): NK.exe has installed its own handlers

### Vector Ready Gate

Function `wince_boot_timer_irq_allowed()` (line 775-800 in wince_boot.c):

```c
bool wince_boot_timer_irq_allowed(struct machine *gxm, struct cpu *cpu)
{
    if (!m->wince.vectors_ready) {
        // TIMER INTERRUPTS BLOCKED until vectors_ready becomes true
        return false;
    }
    // TIMER INTERRUPTS ALLOWED once vectors_ready is true
    return true;
}
```

This gate is consulted during every VR41xx device tick in `dev_vr41xx.c` line 562:
```c
timer_allowed = wince_boot_timer_irq_allowed(cpu->machine, cpu) ? 1 : 0;
```

### When vectors_ready is Set to TRUE

In `scan_low_vectors()` (wince_boot.c line 313-364):
1. Reads low memory exception vectors (TLB @0x0000, General @0x0180)
2. Compares against synthetic vectors (what ROM wrote)
3. When NK.exe writes real non-synthetic handlers, detects transition:
   - `vector_owner = WINCE_VECTOR_SYNTHETIC` → `WINCE_VECTOR_GUEST`
   - Sets `vectors_ready = true` once real handlers are detected

**This function is called once per tick via `wince_boot_on_vr41xx_tick()`** (dev_vr41xx.c line 561)

## The Stall Hypothesis: Three Possible Causes

### 1. VECTORS NOT INSTALLED (Most Likely)

NK.exe hasn't written exception handlers to PA 0x0000 / 0x0180 yet.
- `scan_low_vectors()` never detects guest handlers
- `vectors_ready` remains FALSE
- Timer interrupts remain **PERMANENTLY BLOCKED**
- NK.exe cannot run the scheduler (needs timer interrupts to work)
- **Result**: Machine runs non-interruptible code → hangs/loops forever

**Symptom**: Diagnostic output would show:
```
[WINCE_CKPT] timer_irq_gate active waiting_for_vectors
```
Repeated, never transitioning to:
```
[WINCE_CKPT] timer_irq_gate released owner=3
```

### 2. SYNTHETIC VECTORS NOT INSTALLED

The ROM's synthetic vector installation may have never happened or been overwritten.
- `wince_boot_install_synthetic_low_vectors()` is called from... (need to find caller)
- If not called, `vector_owner` starts as `WINCE_VECTOR_NONE`, not `WINCE_VECTOR_SYNTHETIC`
- When NK.exe writes vectors, `scan_low_vectors()` may not properly detect the transition

### 3. STANDBY PATCH TIMING / EXECUTION ISSUE

Even though Patch 2 (active) patches STANDBY, there could be:
- **Cache coherency issue**: `invalidate_translation_caches()` called with wrong parameters
- **Wrong execution path**: NK.exe never actually executes the patched STANDBY instruction
- **Early patch (Patch 1)** was commented out for a reason—maybe to debug Patch 2

## Diagnostic Infrastructure in wince_boot.c

The codebase has comprehensive one-shot logging:

```c
typedef struct {
    bool active;
    
    // Checkpoint flags (one-shot logging)
    bool spl_handoff_logged;
    bool cold_boot_redirected;
    bool timer_config_logged;
    bool first_exception_logged;
    bool fatal_exit_logged;
    bool timer_gate_logged;          // ← Would show if timer gate is active
    bool timer_release_logged;       // ← Would show when gate releases
    bool vectors_ready;              // ← TRUE when NK.exe vectors detected
    // ... more tracking ...
} wince_boot_state_t;
```

## What Should Happen vs. What's Happening

### Expected Cold Boot Flow (Linux for comparison):
1. ROM loads SPL
2. SPL decompresses NK.exe
3. NK.exe starts → sets CP0 Status IE=1 (enable interrupts)
4. NK.exe writes exception handlers to PA 0x0000 / 0x0180
5. `scan_low_vectors()` detects guest handlers → `vectors_ready = TRUE`
6. Timer interrupts unblocked → NK.exe scheduler runs
7. NK.exe displays "Initializing..." then "Touch calibration" screen

### Current Behavior (Stalled):
1. ✓ ROM loads SPL
2. ✓ SPL decompresses NK.exe  
3. ✓ NK.exe starts
4. ? NK.exe writes exception handlers (UNKNOWN - probably not happening)
5. ✗ `scan_low_vectors()` never sees guest handlers
6. ✗ `vectors_ready` stays FALSE → timer interrupts permanently blocked
7. ✗ Scheduler cannot run without timer interrupts → hangs

## Where NK.exe Stalls

The stall occurs:
1. **After "Initializing..." splash** (displayed by GDI, runs without interrupts)
2. **Before touch calibration screen** (needs scheduler/timer interrupts)

This is the **classic software initialization order bug**: code runs fine in non-preemptive mode, but hangs when it tries to enable preemption.

## Files to Examine

1. **`/Users/jroark/src/be300-framebuffer/src/wince_boot.c`** - All boot state tracking
2. **`/Users/jroark/src/be300-framebuffer/src/wince_boot_types.h`** - Boot state struct
3. **`/Users/jroark/src/be300-framebuffer/src/machine_be300.c`** - Patch application + boot loop
4. **`/Users/jroark/src/be300-framebuffer/gxemul/src/devices/dev_vr41xx.c`** - Timer interrupt gating
5. **`/Users/jroark/src/be300-framebuffer/src/hw/icu.c`** - Interrupt controller state

