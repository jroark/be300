# Restore Images Documentation

This document describes the WinCE restore images for the Casio BE-300 found in `ce/restore_images/`.

## Summary

The repository contains raw NAND restore images and Casio backup files. The
NAND images contain the boot metadata, SPL, NK.exe, and FAT16 filesystem
partition. The backup files contain only a Casio backup header plus the FAT16
filesystem payload; they are not directly bootable.

| File | Size | OS Version | Description |
|------|------|------------|-------------|
| `All_nand_300.bin` | 16 MB | Windows CE 3.0 | Factory restore image for BE-300. Bootable NAND template. |
| `BE500.bin` | 16 MB | Windows CE 3.0? | Likely from the BE-500 model. Features Bootloader Ver 0.62. |
| `CE_Net.bin` | 16 MB | Windows CE 4.0 (.NET) | An unofficial or experimental port of WinCE 4.0. |
| `NANDWRITER.bin` | 32 KB | Windows CE (App) | A "B000FF" formatted utility for CF-to-NAND flashing/verification. |
| `org_CE_30.bin` | 16 MB | Windows CE 3.0 | Original CE 3.0 factory image, Bootloader Ver 0.60. |
| `BACKUP.bin` | 12 MB | Windows CE 3.0 user data | Casio backup file. Not bootable by itself. |
| `BACKUP_BeShell_fixed.bin` | 12 MB | Windows CE 3.0 user data | Casio backup file with BeShell payload. Not bootable by itself. |

## NAND Layout

The NAND images (`All_nand_300.bin`, `BE500.bin`, `CE_Net.bin`,
`org_CE_30.bin`) share a consistent active restore layout.

| Offset | Page | Description |
|--------|------|-------------|
| `0x00000000` | 0 | Partition Table / Boot Metadata |
| `0x00004000` | 32 | **Block 1**: Secondary Bootloader (SPL) or Loader. Starts with `B000FF\n`. |
| `0x00014000` | 160 | **Block 5**: Main OS Image (WinCE Kernel). Starts with `B000FF\n`. |
| `0x003B4000` | 7584 | **Filesystem**: Start of user data / system partitions (FAT16). |

The partition table in block 0 uses 16-byte entries:

```text
8 bytes 0xFF + start_sector(4 LE) + sector_count(4 LE)
```

For the WinCE 3.0 images, partition 3 is the filesystem partition at sector
7584 with 24544 sectors (`0xBFC000` bytes).

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

### Backup-only images
- **Type**: Casio backup file, not a full NAND dump.
- **Format**: Small backup-tool header followed by the FAT16 filesystem
  partition payload.
- **Known wrappers**:
  - `BACKUP.bin` and `BACKUP_BeShell_fixed.bin`: 512-byte header beginning
    with `CASIO WinCE PDA backup file`; payload offset `0x200`.
  - `expod61_Backup.dat`: UTF-16LE `Jx740 CFBackup Ver1.00` header; payload
    offset `0x240`.
- **Payload size**: `0xBFC000` bytes, matching NAND partition 3.
- **Bootability**: These files do not contain the SPL or NK.exe partitions.
  Use `tools/build_bootable_nand_from_backup.py` to graft the filesystem
  payload onto a bootable NAND template. The tool autodetects the wrapper by
  finding a FAT16 boot sector whose BPB matches the template's partition 3
  size.


## Usage in Emulator

To boot these images in the emulator:
1. The emulator must model the NEC VR4131 CPU and the custom NAND controller.
2. The `B000FF\n` images are likely copied into RAM at boot by the internal ROM/IPL.
3. The addresses found in the `B000FF\n` header (e.g., `0x8480F000`) indicate the target RAM execution address.

To build a bootable NAND image from a backup-only image:

```bash
python3 tools/build_bootable_nand_from_backup.py \
  --backup ce/restore_images/BACKUP_BeShell_fixed.bin \
  --template ce/restore_images/All_nand_300.bin \
  --output build-host/All_nand_BeShell_fixed_bootable.bin
```

The tool preserves the template's boot metadata, SPL, and NK.exe partitions
and replaces only partition 3 with the backup FAT16 payload.

The same command works for other backup wrappers, for example:

```bash
python3 tools/build_bootable_nand_from_backup.py \
  --backup ./expod61_Backup.dat \
  --template ce/restore_images/All_nand_300.bin \
  --output build-host/All_nand_expod61_bootable.bin
```
