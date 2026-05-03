# Local Assets

This repository does not redistribute NAND restore images, Casio backup
images, restore-package executables/DLLs, copied vendor readmes, or
vendor/manual PDFs. Those files may be copyrighted or device/user specific and
must be supplied locally by each developer.

## Required For WinCE Boot Testing

Place a raw BE-300 WinCE 3.0 NAND restore image at:

```text
ce/restore_images/All_nand_300.bin
```

Expected shape:

- raw logical NAND sector data
- 16,449,536 bytes (`1004 * 32 * 512`)
- `B000FF\n` SPL signature at offset `0x4000`
- FAT16 filesystem partition at offset `0x3B4000`

The standard smoke-test command assumes that local path:

```bash
build-host/be300 --nand ce/restore_images/All_nand_300.bin
```

## Optional Local Assets

Other restore images and backups can be kept locally under `ce/restore_images/`
or another ignored path:

- `org_CE_30.bin`
- `BE500.bin`
- `CE_Net.bin`
- `All_nand_Net.bin`
- `BACKUP*.bin`
- `expod61_Backup.dat`
- generated `*.nand` / `*.img` files

Restore package tools and copied vendor text are also local-only assets. If
needed for `--restore --cf` or reverse engineering, keep files such as
`NANDWRITER.bin`, `KLOADER.bin`, `Setup.exe`, `DevOSInstall.exe`, helper DLLs,
and package readmes in an ignored local path.

The hardware manuals previously referenced from `docs/hardware/*.pdf` should
also be kept outside Git. Documentation may still cite the manual names and
sections, but the PDF files themselves are local prerequisites.

## Notes For Contributors

- Do not commit ROM/NAND dumps, backup images, restore-package binaries,
  copied vendor text, user CF images, generated web NAND bundles, or vendor
  PDFs.
- Keep derived observations, hashes, offsets, and reverse-engineering notes in
  text documentation where they are useful.
- If a tool needs a local asset, make the path explicit and fail with a clear
  message when the file is absent.
