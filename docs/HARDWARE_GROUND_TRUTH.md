# BE-300 Hardware "Ground Truth" Reference

This document catalogs the findings from introspection and surveying of real Casio BE-300 hardware. This data serves as the "Oracle" for calibrating the Unicorn-based emulator.

## Log Files Summary

| File | Tool Version | Primary Focus | Key Insight |
| :--- | :--- | :--- | :--- |
| `hw_survey_v8.txt` | v8 | Timing & Identifiers | Confirmed 32.768kHz RTC and 0x7100 Board ID. |
| `introspection.txt` | v1 | Memory Layout | Confirmed 0x8008B240 as the primary WinCE exception handler. |
| `nand_sniff_v3.txt` | v3 | I/O Activity | Identified 0x0A000C38 as an active NAND-related register. |
| `cpu_state.txt` | v1 | CPU Internals | Confirmed 32-bit mode, 166MHz timing, and 0x10135923 Config. |
| `tlb_dump_v2.txt` | v2 | MMU State | Discovered variable page sizes (4KB and 16KB) in use by WinCE. |

---

## 1. Timing and Clocks (from `hw_survey_v8.txt` & `cpu_state.txt`)

### Real-Time Clock (RTC)
*   **Physical Address:** `0x0F000100` (VR4131 RTC Window)
*   **Measurement:** ~32,886 ticks in 1000ms.
*   **Interpretation:** The BE-300 uses a standard **32.768 kHz** crystal. 
*   **Emulator Action:** Set the RTC increment to exactly 32,768 per simulated second.

### CPU Pipeline Clock
*   **CP0 Count ($9) / Compare ($11):** Snapshot shows `Count` trailing `Compare` by ~16,000 ticks.
*   **Interpretation:** The VR4131 `Count` register increments at **half the PClock frequency**. At 166MHz, this is 83.3 million increments per second.

---

## 2. CPU Configuration (from `cpu_state.txt`)

### CP0 Status ($12): `0x00008401`
*   **Mode:** 32-bit Addressing (KX/SX/UX bits are 0).
*   **Interrupts:** Global Enable (IE=1), Timer Unmasked (IM7=1).
*   **Coprocessors:** CU0 enabled.

### CP0 Config ($16): `0x10135923`
*   **Endianness:** Little-Endian (BE=0).
*   **Caching:** KSeg0 set to Cached (K0=3).

### CP0 PRId ($15): `0x00000C80`
*   **Silicon:** NEC VR4131 Revision 0.0.

---

## 3. MMU and TLB Configuration (from `tlb_dump_v2.txt`)

Partial dump of the VR4131 TLB (32 entries) revealed critical mapping patterns:

### Variable Page Sizes
*   **Discovery:** WinCE 3.0 on BE-300 relies on the VR4131's ability to map different page sizes simultaneously (observed 4KB and 16KB).

### Pinned (Wired) Mappings
*   **Wired Count:** `2` (Indices 0 and 1 are fixed).
*   **Entry [00]:** Maps VA `0x00013000` (ASID 0x0C) to PA `0x00CC9000`.
*   **Entry [01]:** Maps VA `0x01F8F000` (ASID 0x00) to PA `0x0000C9000`.

### Active Address Spaces
*   **Observed ASIDs:** `0x00` (Kernel), `0x0B`, `0x0C`.
*   **Global Mappings:** Observed in Entry [03] (bit 0 of EntryLo set).

---

## 4. Component Identification (from `hw_survey_v8.txt`)

### Companion Chip (VRC4173 / D89041)
*   **Board ID (`0x0A00A0C0`):** `0x7100` (Confirmed).
*   **Interpretation:** The chip labeled `NEC D89041F1001` is functionally a VRC4173. WinCE checks this ID during boot to decide which drivers to load.
*   **Revision ID (`0x0A000002`):** `0x0000` (Note: some revisions return 0 here).

---

## 5. Exception Handling (from `introspection.txt`)

The memory dump of **PA 0x00000000 to 0x00000400** reveals how WinCE handles interrupts and memory faults.

### General Exception Vector (PA `0x0180`)
*   **Raw Bytes:** `00000000 00000000 3C1A8008 375AB240 03400008 00000000`
*   **Disassembly:**
    ```mips
    lui  k0, 0x8008
    ori  k0, k0, 0xB240
    jr   k0
    ```
*   **Interpretation:** WinCE **redirects all exceptions** to a high-memory handler at **`0x8008B240`**.
*   **Emulator Importance:** If the emulator hits an exception (like a timer tick) and doesn't have valid kernel code at `0x8008B240`, it will crash.

---

## 6. WinCE Kernel State (from `introspection.txt`)

The dump of **PA `0x00002000`** corresponds to the `KDataStruct` (Kernel Data) in WinCE.

*   **Interrupt Status:** Offset `0x02D4` shows `00300307`.
    *   This matches the ICU (`0x0F000080`) status.
    *   Bits 0-2 (RTC), 8 (GPIO), 9 (SIU) are active.
*   **Interpretation:** A healthy BE-300 always has the RTC and Power Button interrupts enabled and/or pending.

---

## 7. NAND Controller Sniffing (from `nand_sniff_v3.txt`)

The sniffer compared registers before and after a `ReadFile` from `\Nand Disk`.

### Active Registers
*   **Register `0x0A000C38`:** Changed from `0x0006 -> 0x0091` and later to `0x007C`.
*   **Interpretation:** This offset is within the companion chip space but outside the "official" VRC4173 NAND range. 
*   **Hypothesis:** This is a proprietary Casio/NEC latch used for NAND Ready/Busy polling or Command/Address/Data (CLE/ALE) switching.
*   **Emulator Action:** We should monitor this offset in `bus.c` to see if the guest OS waits for specific bits here before proceeding with NAND reads.

---

## How to Read the Hex Dumps

In `introspection.txt` and `hw_survey_v8.txt`, data is presented as:
`[Address]: [Word0] [Word1] [Word2] [Word3]`

*   **Endianness:** The BE-300 is **Little-Endian**. 
    *   A value like `3C1A80F0` at address `0x0000` means:
        *   Byte 0: `F0`
        *   Byte 1: `80`
        *   Byte 2: `1A`
        *   Byte 3: `3C`
*   **Memory Regions:**
    *   `0x00000000`: Physical SDRAM (Low Vectors).
    *   `0x0A000000`: VRC4173 Companion Chip MMIO.
    *   `0x0F000000`: VR4131 Internal Peripheral MMIO.
