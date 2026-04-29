# BeShell Backup Runtime Diagnosis

Date: 2026-04-28

## Summary

The BeShell runtime exceptions are backup-content/runtime dependency issues, not a
NAND layout conversion failure.

A freshly generated bootable NAND from `BACKUP_BeShell_fixed.bin` has:

- partition table matching `All_nand_300.bin`
- SPL/NK/template prefix and suffix matching `All_nand_300.bin`
- FAT partition payload byte-identical to `BACKUP_BeShell_fixed.bin` after its
  512-byte Casio backup header

Stock `All_nand_300.bin` boots through the same 60-second control run without
exceptions. The BeShell image reaches the BeShell desktop, but the no-CF runtime
hits a BeShell null dereference and downstream GWES exceptions.

## Reproduction Matrix

Fresh throwaway image generation:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 tools/build_bootable_nand_from_backup.py \
  --template ce/restore_images/All_nand_300.bin \
  --backup ce/restore_images/BACKUP_BeShell_fixed.bin \
  --output /tmp/All_nand_BeShell_fresh.bin
```

Observed controls:

| Run | Result |
| --- | --- |
| Stock NAND, no CF, 60s | No WinCE exceptions |
| Fresh BeShell NAND, no CF, 180s | `filesys.exe` first-chance style exception, then `BeShell.exe` TLBL at `PC=01771410` / warning PC `01771414`, then downstream GWES exception at `PC=8007e9fc` |
| Fresh BeShell NAND, blank CF, 180s | `filesys.exe` exception remains; BeShell desktop does not finish populating |
| Fresh BeShell NAND, minimal CF with missing WisBar skin files, 180s | `filesys.exe` exception remains; BeShell desktop renders and the BeShell/GWES exception path does not appear |

The successful minimal CF contained only:

```text
\Program Files\WisBar Advance\skins\Attraction\Attraction_v1.0.skin
\Program Files\WisBar Advance\skins\Attraction\Attraction II.TSK
```

copied from the backup's `\Nand Disk\Windows\Skins\Attraction` files.

## Address Notes

- `PC=8009aea0` is in `nk.exe`, in the delay slot of a `jalr -1478` exception
  path using code `0xC000001C`. It is backup-specific but not fatal: the guest
  continues to CCFS/Oomui initialization afterward.
- `PC=01771410` / warning PC `01771414` is in the loaded `BeShell.exe` process.
  The `RA=01771418` value means the trap occurs at a BeShell call site / delay
  slot while dereferencing address zero.
- `PC=8007e9fc` is another `nk.exe` exception-raise path, here with code
  `0xC000000D`, reported while `gwes.exe` handles the fallout.
- Later GWES user-mode sites such as `000441b4` and `0004fba0` are null or
  near-null object/handle dereferences and appear downstream of BeShell state,
  not the first cause.

The `.cpk` files are not plain PE files on disk. They use the Casio
`CASIO.$&%-Z` format and are expanded by `CCFS.DLL` into `Program Disk` at
runtime, so direct PE offset mapping needs a CPK decoder or a guest memory dump.

## Root Cause

`BACKUP_BeShell_fixed.bin` contains registry state that points BeShell skin
configuration at:

```text
\Program Files\WisBar Advance\skins\Attraction\Attraction_v1.0.skin
\Program Files\WisBar Advance\skins\Attraction\Attraction II.TSK
```

Those paths are not present in the NAND backup payload. The backup does contain
the same files under:

```text
\Nand Disk\Windows\Skins\Attraction
```

The cached BE forum notes in `build-host/be300_forums_full/...` describe
`\Program Files` as a Storage Card-backed virtual folder on BeShell systems.
That matches the runtime test: supplying the missing Storage Card path with a
minimal CF image prevents the BeShell/GWES exception path.

## Recommendation

Keep `tools/build_bootable_nand_from_backup.py` focused on producing a bootable
NAND image. Add a separate companion-CF helper or documented recipe for BeShell
backups that need Storage Card paths. For this backup, the minimum companion CF
must provide the WisBar Attraction skin files listed above.

The remaining `filesys.exe` exception is independent of the BeShell/GWES loop.
It should be tracked separately only if it causes user-visible failure after the
companion CF is attached.
