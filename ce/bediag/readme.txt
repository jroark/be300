BEDiag - BE-300 built-in diagnostic driver
==========================================

Purpose
-------
BEDiag is a passive WinCE 3.0 MIPS stream driver for earlier-than-shell
state capture on the Casio BE-300. It is not a functional hardware driver.
It does not claim interrupts, own devices, or replace Casio components.

What it captures
----------------
At built-in driver init, +1 second, and +5 seconds, BEDiag snapshots:

  0x00002200..0x000022FF  resume_ctx
  0x00006000..0x00006FFF  bootctx
  0x0001D000..0x0001DFFF  bootparam0
  0x0002D000..0x0002DFFF  bootparam1
  0x00051680..0x00051AFF  cb_tbl
  0x00660000..0x0066003F  objptr
  0x0066BFC0..0x0066BFDF  obj_header
  0x00679400..0x006795FF  post-boot textual/object region
  0x0A000C00..0x0A000C4F  VRC4173 sideband window
  0x0F000000..0x0F00011F  VR4131 BCU + ICU/PMU/RTC safe window

Per region it logs:
  - read status
  - CRC32
  - changed/not-changed vs prior phase

It also logs focused raw words for:
  - 0x0A000C30
  - 0x0A000C34
  - 0x0A000C38
  - 0x0A000C48
  - 0x0A000C4C
  - 0x0F000080
  - 0x0F000100
  - 0x00002220
  - 0x00002228
  - 0x00002274
  - 0x00660000
  - 0x0066BFC0
  - 0x0066BFC4
  - 0x0066BFC8
  - 0x0066BFCC

Logging behavior
----------------
Primary output is COM1 text logging.
Secondary output is an optional text file if a writable filesystem appears
after Device Manager starts. The driver keeps an in-memory backlog so a late
file open still captures earlier log lines.

Required sections:
  --- BEDIAG INIT ---
  --- SNAPSHOT INIT ---
  --- SNAPSHOT +1S ---
  --- SNAPSHOT +5S ---
  --- DRIVER STATE ---
  --- BEDIAG DONE ---

Registry churn
--------------
At each phase BEDiag logs shallow metadata from:
  HKLM\Drivers\Active
  HKLM\Drivers\BuiltIn
  HKLM\System\StorageManager\Profiles
  HKLM\System\StorageManager\AutoLoad

Only one level of subkeys is walked. Value data is limited to DWORD/string
rendering and basic type reporting.

Build
-----
1. Open BEDiag.dsp in eMbedded Visual C++ 3.0 / Platform Builder 3.0.
2. Select "BEDiag - Win32 (WCE MIPS) Release".
3. Build. The output should be:

     WMIPSRel\BEDiag.dll

Notes:
  - The project wrapper is a minimal WCE MIPS project that links as a DLL via
    /dll and BEDiag.def.
  - No resources or UI are required.

Helper EXE
----------
BEDiagKick.exe is the primary proof tool for current hardware debugging. It
logs to:

  \Windows\BEDiag_kick.txt

It first records:
  - the configured Dll value from HKLM\Drivers\BuiltIn\BEDiag
  - whether that DLL exists
  - whether LoadLibrary succeeds
  - whether the BDG_* exports are present

Then it runs a three-phase ladder:
  1. activate_builtin
  2. register_device
  3. direct_bdg_init

Each phase logs:
  - begin/end markers
  - boot-log delete/probe state
  - BEDiagLoaded / BEDiagInitTick / BEDiagWorkerStarted / BEDiagLastStatus
  - handle/error results
  - Active-key appearance
  - boot-log appearance

Expected high-signal fields:
  - phase=activate_builtin activate_handle=...
  - phase=register_device register_handle=...
  - phase=register_device createfile_handle=...
  - phase=direct_bdg_init bdg_init_ret=...
  - phase=direct_bdg_init bdg_deinit_ret=...

Install
-------
Preferred package path:
1. Install the MkArch package from ce\bediag\mkarch.
2. Verify:

     \Nand Disk\Program Files\Patch\BEDiag.dll
     \Nand Disk\Program Files\Patch\BEDiagKick.exe
     HKLM\Drivers\BuiltIn\BEDiag\Dll = \Nand Disk\Program Files\Patch\BEDiag.dll

3. Soft reset the BE-300.
4. Inspect:

     \Windows\BEDiag_boot.txt
     \Windows\BEDiag_kick.txt

Fallback manual path:
1. Copy BEDiag.dll and BEDiagKick.exe to the device, for example:

     \Nand Disk\Program Files\Patch\BEDiag.dll
     \Nand Disk\Program Files\Patch\BEDiagKick.exe

2. Add the BuiltIn registry key from BEDiag.reg.txt.
3. Soft reset the BE-300.
4. Run BEDiagKick.exe if there is still no BEDiag_boot.txt.

Interpretation
--------------
Use the first boot with BEDiag to decide whether the emulator gap is:

  - already present by Device Manager init
  - produced later by normal driver/service churn
  - or earlier than Device Manager, which means the next step should move to
    NK/loader instrumentation instead of more built-in driver work

Current decision order:
  - activate_builtin fails, register_device succeeds:
    BuiltIn semantics are the blocker.
  - activate_builtin fails, register_device fails, direct_bdg_init succeeds:
    device.exe / Device Manager loading semantics are the blocker.
  - all three phases fail:
    the driver entrypoint path is still broken despite LoadLibrary succeeding.

Compatibility note
------------------
The BE-300 driver guidelines indicate BE can use PPC-standard embedded drivers
that have no UI, provided they are rebuilt for MIPS. BEDiag follows that
constraint and intentionally keeps the code path passive.
