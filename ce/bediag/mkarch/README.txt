BEDiag MkArch packaging
=======================

Purpose
-------
This directory contains MkArch-ready installer source trees for the BEDiag
built-in diagnostic driver. The installer writes the BuiltIn registry key
directly through install.inf and installs a raw DLL, not a .cpk payload.

Variants
--------
1. windows_system
   Primary package. Installs:
     \Windows\BEDiag.dll
     \Windows\BEDiagKick.exe
   Registry:
     HKLM\Drivers\BuiltIn\BEDiag\Dll = "BEDiag.dll"

2. patch_fallback
   Fallback package. Installs:
     \Nand Disk\Program Files\Patch\BEDiag.dll
     \Windows\BEDiagKick.exe
   Registry:
     HKLM\Drivers\BuiltIn\BEDiag\Dll = "\Nand Disk\Program Files\Patch\BEDiag.dll"

Why raw DLLs
------------
The sample BE-300 update packages use .cpk files for normal NAND app payloads,
but built-in driver loading should target a real DLL filename. The primary
variant matches the stock built-in pattern most closely by putting BEDiag.dll
in \Windows and using Dll="BEDiag.dll".

Files in each source tree
-------------------------
install.inf
  Install actions for the package.

unapps.inf
  Cleanup manifest. It is copied onto the device during install so the same
  delete actions can be reused if you later want to build a removal-only
  package.

apps.txt
  Repo-side package manifest for the variant.

InsResetFlag.txt
  Empty marker copied to \Windows to request the stock post-install reset path.

BEDiag.dll
  Copy this in from the PB3/eVC build output before running MkArch:
    ce\bediag\WMIPSRel\BEDiag.dll
  Do not rename it.

BEDiagKick.exe
  Copy this in from the PB3/eVC build output before running MkArch:
    ce\bediag\WMIPSRel\BEDiagKick.exe
  Do not rename it.

Windows 2000 VM workflow
------------------------
1. Build BEDiag.dll from ce\bediag\BEDiag.dsp as "BEDiag - Win32 (WCE MIPS) Release".
2. Build BEDiagKick.exe from ce\bediag\BEDiagKick.dsp as "BEDiagKick - Win32 (WCE MIPS) Release".
3. Copy both outputs into exactly one source directory:
     windows_system\BEDiag.dll
     windows_system\BEDiagKick.exe
   or:
     patch_fallback\BEDiag.dll
     patch_fallback\BEDiagKick.exe
4. Run BUILD_WINDOWS.cmd for the primary package.
5. These scripts expect MkArch.exe and Setup.exe in the parent directory of
   mkarch, matching the local BE-300 VM layout:

     mkarch\..

6. If the package installs and resets the BE-300 but no \Windows\BEDiag_boot.txt
   appears on the next boot, do not switch packages first. Run BEDiagKick.exe
   and inspect \Windows\BEDiag_kick.txt for loadlibrary/export/activation
   results. Only try BUILD_PATCH.cmd after that if the primary package proves
   valid but still fails to autoload.

Outputs
-------
The build scripts produce these bundles:
  out\windows_system\Setup.exe
  out\windows_system\Setup.ini
  out\windows_system\BEDiag_windows.cbea

  out\patch_fallback\Setup.exe
  out\patch_fallback\Setup.ini
  out\patch_fallback\BEDiag_patch.cbea

Cleanup / uninstall
-------------------
These packages are patch-style installs, not normal launcher apps. The included
unapps.inf files define the authoritative delete actions:
  - remove HKLM\Drivers\BuiltIn\BEDiag
  - remove the installed BEDiag.dll
  - remove \Windows\BEDiagKick.exe
  - remove the copied cleanup INF
If the stock installer UI does not expose uninstall for this package type, use
that INF as the source for a removal-only package rather than editing the live
registry by hand.

Hardware test order
-------------------
1. Install the windows_system package on one stock-shell BE-300.
2. Capture COM1 from install through the post-install reset boot if available.
3. Inspect:
     \Windows\BEDiag_boot.txt
     \Windows\BEDiag_kick.txt
4. In BEDiag_boot.txt look for:
     --- BEDIAG INIT ---
     --- SNAPSHOT INIT ---
     --- SNAPSHOT +1S ---
     --- SNAPSHOT +5S ---
     --- DRIVER STATE ---
     --- BEDIAG DONE ---
5. If there is no boot file after reset, run BEDiagKick.exe manually and inspect:
     dll_exists=yes|no
     reg_key_exists=yes|no
     loadlibrary=yes|no
     export_BDG_Init=yes|no
     activate_handle=...
     active_key_after=...
     boot_log_after=...
6. Only if the primary package proves valid but still does not autoload, try
   the patch_fallback package.
