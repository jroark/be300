# Installing BE-300 Apps Without Windows

Casio CASSIOPEIA application packages (`Setup.exe` + `Setup.ini` + `*.cbea`)
normally install through a Windows host: `Setup.exe` launches PC Connect's
`appmgr.exe`, which pushes the `.cbea` over ActiveSync to the device-side
`CoInstall.exe`. `src/cbea_install.c` applies a package directly to a NAND
image instead, so no Windows VM or `--pcconnect-bridge` session is needed.

## Host CLI

```bash
cd build-host && make be300-install
./be300-install --output ../nand_wmp.bin ../ce/restore_images/All_nand_300.bin \
  "../WMP 1.1 for eXpod 6.x"
./be300 --nand ../nand_wmp.bin
```

- `<package>` can be a `.cbea`, a `Setup.ini`, or a package directory. For a
  directory with `MAIN/` and `CF/` variants, `MAIN/` is used.
- Several packages can be listed. Each install is all-or-nothing, and nothing
  is written if any package fails.
- The default output is `<nand>.installed.bin`. `--in-place` modifies the
  image directly. Don't run it on an image a running `be300` has open: the
  emulator rewrites the NAND file when it exits.

## Web frontend

Pick packages with **Install apps (.cbea)**, or drop `.cbea` files anywhere
on the page.

- **Before boot:** packages are queued and installed into the NAND image when
  Boot is pressed. They stay installed across Reset.
- **While running:** the page asks to reboot. The worker destroys the machine,
  which flushes the guest's NAND writes to MEMFS, then installs the package
  and boots again.

Install logs appear in the serial pane as `[INSTALL]` lines.

## What an install does

`Install.inf` (the first `.cbea` entry) drives everything:

| INF item | Result |
| --- | --- |
| `[Copy Files] "dest","archive name",flags` | file written to the `\Nand Disk` FAT16 volume (block-0 partition entry 3). `?Drive?` = `\Nand Disk`, `%INSTALL_DIR%` = `InstallDir`. `COPYFLG_NOOVERWRITE` keeps an existing file. |
| `\Windows\<file>` destination | written to `\Nand Disk\Program Files\Patch\<file>`, which the BE-300 shadows into the volatile `\Windows` on every cold boot |
| `[Add Registry]` | merged into the compact `\Backup\System.reg` snapshot (and `System.$$$`) |
| `[App Information]` `ExeFile`/`Caption`/... | launcher entry `HKLM\SOFTWARE\MENU\<Program>` |

`.cpk` payloads (`CASIO.$&%-Z`) are copied unchanged. The guest's CCFS
(`Drivers\BuiltIn\CompDisk`, `BaseDirectory = "Program Files"`) expands them
into `\Program Disk` at runtime.

### Registry format details (from guest-written snapshots)

- Key record: `u16 len, u16 1, u16 root, u16 nchars, u16 class_nchars`, then
  the UTF-16Z path and optional class. Roots: 0 = HKCR, 1 = HKCU, 2 = HKLM.
  A guest-saved snapshot has `HKLM\SOFTWARE\Microsoft\Clock` with class
  `Preon`. `tools/inject_stowaway.py` reads `nchars` as a u32, which only
  works when the class is empty.
- Value record: `u16 len, u16 2, u16 type, u16 nchars, u16 data_len`, then the
  name and data. Default values are stored under the literal name `Default`.
- `REG_DWORD` digits are parsed as hex. WMP 1.1's INF lists Buttons
  `13/27/37`, and the installed WMP 8 registry in `nand/expod.bin` holds
  `0x13/0x27/0x37`.
- A trailing `\\` in an INF string is one `\`. The installed WMP 8 `SkinDir`
  ends in a single backslash.

### Launcher entry

The Casio launcher (`menu.exe`) lists `HKLM\SOFTWARE\MENU\*`. The shape is
copied from six device-installed apps in `nand/beshell.bin` (Tweak, RegEdit,
Notepad, ...): `ANAME`=Caption, `APATH`=ExeFile, `ACMND`="",
`ULOADFILE`=Uninstall, `ULOAD`=Unload, `APPNO`=8, empty
`SETTING_DLL/OP_DATA/SOUNDBMP/WAVE/ICON`, `INSTALL_DIR`, and
`\VERSION MAJOR/MINOR`. `HKLM\SOFTWARE\SETINSTALL` only holds installer
status (free space, paths, `installresult`) and is not written.

## Limits

- Only main-memory installs. `Destination = STORAGE_ONLY` packages (the `CF/`
  variants) are rejected, because the emulated Storage Card is not mounted
  yet. `ALTERNATIVE` and `MAINMEM_ONLY` install to `\Nand Disk`.
- Packages don't declare their runtime prerequisites. WMP 1.1 "for eXpod 6.x"
  imports `CEShell.DLL`, which is in no stock NK. Among the local images, only
  `nand/beshell.bin` provides it (`Program Files\SystemCE\ceshell.dll.cpk`).
  On the stock WinCE 3.0 image, WMP shows in the launcher but fails with
  "Cannot execute ... not supported by this system".
