BEDiag MkArch packaging
=======================

Purpose
-------
This directory contains one canonical BE-300-style patch installer for BEDiag.
The package matches the working Casio examples by using App Information at the
head of Install.inf while keeping persistent binaries on NAND Patch storage.
On the BE-300, \Windows should be treated as volatile; only transient proof
files and \Windows\InsResetFlag.txt belong there.

Package contents
----------------
Install.inf
  Canonical Casio install INF. It declares BEDiag as a Patch-style app and
  copies:
    ?Drive?\Program Files\Patch\BEDiag.dll
    ?Drive?\Program Files\Patch\BEDiagKick.exe
    ?Drive?\Program Files\UnBEDiag.inf
    \Windows\InsResetFlag.txt
  It also writes:
    HKLM\Drivers\BuiltIn\BEDiag\Dll = "\Nand Disk\Program Files\Patch\BEDiag.dll"

UnBEDiag.inf
  Canonical uninstall INF for the same Maker/Program pair.

apps.txt
  Repo-side manifest only.

InsResetFlag.txt
  Empty marker copied to \Windows to request the stock post-install reset path.

BEDiag.dll
  Copy this in from:
    ce\bediag\WMIPSRel\BEDiag.dll

BEDiagKick.exe
  Copy this in from:
    ce\bediag\WMIPSRel\BEDiagKick.exe

Setup.ini
  Desktop wrapper template for Setup.exe.

BUILD.cmd
  Stages only the package payload files, runs MkArch, and emits:
    out\BEDiag.cbea
    out\Setup.exe
    out\Setup.ini

Windows 2000 VM workflow
------------------------
1. Build BEDiag.dll from ce\bediag\BEDiag.dsp as "BEDiag - Win32 (WCE MIPS) Release".
2. Build BEDiagKick.exe from ce\bediag\BEDiagKick.dsp as "BEDiagKick - Win32 (WCE MIPS) Release".
3. Copy both outputs into this directory:
     BEDiag.dll
     BEDiagKick.exe
4. Place MkArch.exe and Setup.exe in the parent directory of mkarch:

     mkarch\..

   Place native.ina in that same directory as well.

5. Run BUILD.cmd.

BUILD.cmd now stages Setup.exe and native.ina into the temporary package
directory before invoking MkArch, because MkArch expects them beside the other
package files during archive generation.

Hardware test order
-------------------
1. Install the generated BEDiag package on one stock-shell BE-300.
2. Allow the device to reset.
3. Inspect:
     \Windows\BEDiag_boot.txt
4. Confirm the persistent Patch payloads exist under:
     \Nand Disk\Program Files\Patch
   or the resolved ?Drive?\Program Files\Patch location:
     BEDiag.dll
     BEDiagKick.exe
5. Confirm the registry points at the persistent DLL:
     HKLM\Drivers\BuiltIn\BEDiag\Dll = \Nand Disk\Program Files\Patch\BEDiag.dll
6. If there is no boot file after reset, run BEDiagKick.exe manually and inspect:
     dll_exists=yes|no
     reg_dll=...
     reg_key_exists=yes|no
     loadlibrary=yes|no
     loadlibrary_path=...
     export_BDG_Init=yes|no
     tmp_copy_ok=yes|no
     phase=activate_builtin begin/end
     phase=activate_builtin activate_handle=...
     phase=register_device begin/end
     phase=register_device register_handle=...
     phase=register_device createfile_handle=...
     phase=register_device_windows_copy begin/end
     phase=register_device_windows_copy register_handle=...
     phase=activate_builtin_windows_copy begin/end
     phase=activate_builtin_windows_copy activate_handle=...
     phase=direct_init_nullctx begin/end
     phase=direct_init_nullctx init_ret=...
     phase=direct_init_builtinctx begin/end
     phase=direct_init_builtinctx init_ret=...

BEDiagKick is now the primary diagnostic ladder:
  1. ActivateDevice on Drivers\BuiltIn\BEDiag
  2. RegisterDevice using the same DLL path but a temporary BDG9: name
  3. RegisterDevice using basename loading from \Windows\BEDiagTmp.dll
  4. ActivateDevice using a temporary Drivers\BuiltIn\BEDiagTmp key
  5. direct Init / Deinit with null context
  6. direct Init / Deinit with Drivers\BuiltIn target-key context

The helper also supports a no-argument alternate name:
  BDGMiniKick.exe

That selects the manual-only BDGMini driver and writes:
  \Windows\BDGMini_kick.txt
  \Windows\BDGMini_boot.txt

BDGMini is intentionally not packaged through MkArch in this pass. Keep
BEDiag packaging frozen and deploy BDGMini manually from:
  \Nand Disk\Program Files\Patch\BDGMini.dll
with the registry template in:
  ce\bediag\BDGMini.reg.txt

If command-line launchers are available, `mini` still selects the same target.
On the BE-300, the expected flow is to copy the same built helper binary twice:
  \Nand Disk\Program Files\Patch\BEDiagKick.exe
  \Nand Disk\Program Files\Patch\BDGMiniKick.exe

BACKUP.bin note
---------------
The copied BACKUP.bin is useful as a filesystem-layout reference. It confirms
that persistent payloads live on NAND storage while \Windows is volatile. It
is not the primary source of live BuiltIn registry truth; BEDiag_boot.txt is.
