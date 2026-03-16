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
Primary proof path is:

  \Windows\BEDiag_boot.txt

BEDiag opens this file synchronously in BDG_Init and appends deterministic
stage breadcrumbs plus the full snapshot output. COM1 logging remains enabled
as best-effort only and should not be treated as the primary WinCE proof
channel on this device.

If \Windows logging is not available, BEDiag falls back to the older secondary
file search on writable roots and replays the in-memory backlog there.

Required sections:
  --- BEDIAG INIT ---
  --- SNAPSHOT INIT ---
  --- SNAPSHOT +1S ---
  --- SNAPSHOT +5S ---
  --- DRIVER STATE ---
  --- BEDIAG DONE ---

Deterministic stage breadcrumbs are also written:
  stage=init_enter
  stage=worker_created
  stage=snapshot_init_done
  stage=snapshot_1s_done
  stage=snapshot_5s_done
  stage=done status=...

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
BEDiagKick.exe is a tiny no-UI helper that attempts to activate the same
built-in driver key manually and logs to:

  \Windows\BEDiag_kick.txt

Build it from BEDiagKick.dsp as:

  "BEDiagKick - Win32 (WCE MIPS) Release"

Install
-------
Preferred path:
  Use the MkArch packaging bundle in ce\bediag\mkarch and install BEDiag
  through the Casio Setup.exe + Setup.ini + .cbea flow. The package writes the
  BuiltIn registry key, installs BEDiag.dll plus BEDiagKick.exe, and requests
  the stock post-install reset path.

Fallback manual path:
1. Copy BEDiag.dll and BEDiagKick.exe to the device, for example:

     \Windows\BEDiag.dll
     \Windows\BEDiagKick.exe

2. Add the BuiltIn registry key from BEDiag.reg.txt.
3. Soft reset the BE-300.
4. After reset, inspect:

     \Windows\BEDiag_boot.txt
     \Windows\BEDiag_kick.txt

5. Capture COM1 if useful, but do not treat it as the primary WinCE proof
   channel.

Interpretation
--------------
Use the first boot with BEDiag to decide whether the emulator gap is:

  - already present by Device Manager init
  - produced later by normal driver/service churn
  - or earlier than Device Manager, which means the next step should move to
    NK/loader instrumentation instead of more built-in driver work

Compatibility note
------------------
The BE-300 driver guidelines indicate BE can use PPC-standard embedded drivers
that have no UI, provided they are rebuilt for MIPS. BEDiag follows that
constraint and intentionally keeps the code path passive.
