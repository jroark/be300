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

5. Run BUILD.cmd.

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
     activate_handle=...
     active_key_after=...
     boot_log_after=...
