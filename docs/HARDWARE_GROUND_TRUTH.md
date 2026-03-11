# BE-300 Hardware "Ground Truth" Reference

This document catalogs the findings from introspection and surveying of real Casio BE-300 hardware. This data serves as the "Oracle" for calibrating the Unicorn-based emulator.

## Log Files Summary

| File | Tool Version | Primary Focus | Key Insight |
| :--- | :--- | :--- | :--- |
| `hw_survey_v8.txt` | v8 | Timing & Identifiers | Confirmed 32.768kHz RTC and 0x7100 Board ID. |
| `introspection.txt` | v1 | Memory Layout | Confirmed 0x8008B240 as the primary WinCE exception handler. |
| `nand_sniff_v3.txt` | v3 | I/O Activity | Identified 0x0A000C38 as an active NAND-related register. |

---

## 1. Timing and Clocks (from `hw_survey_v8.txt`)

### Real-Time Clock (RTC)
*   **Physical Address:** `0x0F000100` (VR4131 RTC Window)
*   **Measurement:** ~32,886 ticks in 1000ms.
*   **Interpretation:** The BE-300 uses a standard **32.768 kHz** crystal. The slight excess in the log is due to the latency of the WinCE `Sleep()` call and the survey tool's overhead.
*   **Emulator Action:** Set the RTC increment to exactly 32,768 per simulated second to maintain sync with the guest OS.

### CPU Clock (BCU)
*   **Register (`CLKSPEEDREG`):** `0x020C`
*   **Interpretation:** Confirms the VR4131 is Strapped for High Speed (likely 166MHz). This matches the expectation for a "warm" WinCE system.

---

## 2. Component Identification (from `hw_survey_v8.txt`)

### Companion Chip (VRC4173 / D89041)
*   **Board ID (`0x0A00A0C0`):** `0x7100` (Confirmed).
*   **Interpretation:** The chip labeled `NEC D89041F1001` is functionally a VRC4173. WinCE checks this ID during boot to decide which drivers to load.
*   **Revision ID (`0x0A000002`):** `0x0000` (Note: some revisions return 0 here).

---

## 3. Exception Handling (from `introspection.txt`)

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

## 4. WinCE Kernel State (from `introspection.txt`)

The dump of **PA `0x00002000`** corresponds to the `KDataStruct` (Kernel Data) in WinCE.

*   **Interrupt Status:** Offset `0x02D4` shows `00300307`.
    *   This matches the ICU (`0x0F000080`) status.
    *   Bits 0-2 (RTC), 8 (GPIO), 9 (SIU) are active.
*   **Interpretation:** A healthy BE-300 always has the RTC and Power Button interrupts enabled and/or pending.

---

## 5. NAND Controller Sniffing (from `nand_sniff_v3.txt`)

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
