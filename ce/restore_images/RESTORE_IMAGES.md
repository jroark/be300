# Restore Images Documentation

This document describes the WinCE restore images for the Casio BE-300 found in `ce/restore_images/`.

## Summary

The repository contains two 16MB NAND images representing the full flash memory of the Casio BE-300. These images are raw binary dumps without OOB (Spare Area) data.

| File | Size | OS Version | Description |
|------|------|------------|-------------|
| `All_nand_300.bin` | 16 MB | Windows CE 3.0 | Factory restore image for BE-300. |
| `BE500.bin` | 16 MB | Windows CE 3.0? | Likely from the BE-500 model. Features Bootloader Ver 0.62. |
| `CE_Net.bin` | 16 MB | Windows CE 4.0 (.NET) | An unofficial or experimental port of WinCE 4.0. |
| `NANDWRITER.bin` | 32 KB | Windows CE (App) | A "B000FF" formatted utility for CF-to-NAND flashing/verification. |
| `org_CE_30.bin` | 16 MB | Windows CE 3.0 | Original CE 3.0 factory image, Bootloader Ver 0.60. |

## NAND Layout (Estimated)

The 16MB NAND images (`All_nand_300.bin`, `BE500.bin`, `CE_Net.bin`, `org_CE_30.bin`) share a consistent layout.

| Offset | Page | Description |
|--------|------|-------------|
| `0x00000000` | 0 | Partition Table / Boot Metadata |
| `0x00004000` | 32 | **Block 1**: Secondary Bootloader (SPL) or Loader. Starts with `B000FF\n`. |
| `0x00014000` | 160 | **Block 5**: Main OS Image (WinCE Kernel). Starts with `B000FF\n`. |
| `0x003B5000` | 7584 | **Filesystem**: Start of user data / system partitions (FAT16). |

### Signatures

- **`B000FF\n`**: A custom Casio variant of the standard WinCE binary image signature (`B000`). It is found at the beginning of each major executable section in the NAND.
- **`ECEC`**: A common WinCE image signature found within the kernel blocks.
- **`nk.exe`**: Found in the Table of Contents (TOC) of the kernel image.

## Detailed Analysis

### `All_nand_300.bin` & `org_CE_30.bin`
- **Bootloader**: `Ver 0.52`, `Ver0.60`.
- **OS**: Windows CE 3.0 (MSIE 3.02).
- **Target Hardware**: Casio Cassiopeia BE-300 (VR4131 MIPS).

### `BE500.bin`
- **Bootloader**: `Ver0.62`.
- **Features**: Includes Ethernet-based debugging support (KDBG/CESH).
- **Target Hardware**: Casio Cassiopeia BE-500.

### `CE_Net.bin`
- **Bootloader**: `Ver0.62`.
- **OS**: Reported as Windows CE 4.0 in filename, but internal strings still reference `MSIE 3.02`. This might be a "Net" edition that still uses parts of the CE 3.0 UI or is an early port.
- **Notes**: Consistent `B000FF\n` structure.

### `NANDWRITER.bin`
- **Type**: Standalone binary image (not a full NAND dump).
- **Function**: Appears to be a specialized tool for flashing or verifying a full NAND image (`all_nand.bin`) from a Compact Flash (CF) card. Contains debug strings for NAND verification and CF sector access.


## Usage in Emulator

To boot these images in the emulator:
1. The emulator must model the NEC VR4131 CPU and the custom NAND controller.
2. The `B000FF\n` images are likely copied into RAM at boot by the internal ROM/IPL.
3. The addresses found in the `B000FF\n` header (e.g., `0x8480F000`) indicate the target RAM execution address.
