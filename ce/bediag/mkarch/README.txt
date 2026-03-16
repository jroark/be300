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
   Registry:
     HKLM\Drivers\BuiltIn\BEDiag\Dll = "BEDiag.dll"

2. patch_fallback
   Fallback package. Installs:
     \Nand Disk\Program Files\Patch\BEDiag.dll
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

Windows 2000 VM workflow
------------------------
1. Build BEDiag.dll from ce\bediag\BEDiag.dsp as "BEDiag - Win32 (WCE MIPS) Release".
2. Copy the built DLL into exactly one source directory:
     windows_system\BEDiag.dll
   or:
     patch_fallback\BEDiag.dll
3. Run BUILD_WINDOWS.cmd for the primary package.
4. If the package installs and resets the BE-300 but no BEDiag COM1 markers
   appear on the next boot, run BUILD_PATCH.cmd and test the fallback package.

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
  - remove the copied cleanup INF
If the stock installer UI does not expose uninstall for this package type, use
that INF as the source for a removal-only package rather than editing the live
registry by hand.

Hardware test order
-------------------
1. Install the windows_system package on one stock-shell BE-300.
2. Capture COM1 from install through the post-install reset boot.
3. Look for:
     --- BEDIAG INIT ---
     --- SNAPSHOT INIT ---
     --- SNAPSHOT +1S ---
     --- SNAPSHOT +5S ---
     --- DRIVER STATE ---
     --- BEDIAG DONE ---
4. Only if the driver does not load after reset, try the patch_fallback package.
