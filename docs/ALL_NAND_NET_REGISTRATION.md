# All_nand_Net Registration Gate

Status: investigated on 2026-05-09 from the local-only
`All_nand_Net.bin` image. This is reference work for the unsupported CE .NET
image; the primary emulator target remains unmodified WinCE 3.0 cold boot.

## Summary

`All_nand_Net.bin` is not blocked by a missing emulator feature. It boots into
the CE .NET shell and then `PowerOn.dll` shows the registration modal. The
registration artifact it expects is a NAND-resident DLL:

```text
\Nand Disk\Program Files\Cassiopeia.dll
```

The stock image does not contain `Cassiopeia.dll` or `Cassiopeia.dll.cpk` in
the FAT16 filesystem. `PowerOn.dll` also contains nearby `Certificate` and
`Verificate` strings, which are likely function names or command strings used
to validate that DLL. The exact validation ABI still depends on recovering a
real `Cassiopeia.dll` or the dead registration-site payload.

## Boot Evidence

The image reaches the CE .NET user-mode path:

```bash
gtimeout 90s ./build-host/be300 --nand /tmp/be300-All_nand_Net-run.bin --rtc-host-time
```

Relevant serial output:

```text
KLOADER Ver 0.62
Windows CE Kernel for MIPS Built on Dec 16 2001 at 18:18:46
Start CASIO Original Shell
CASIO Original Shell started.(COShell.exe = Ver1.00.34)
SetupWizard :Welcome.exe (NULL) started.
```

The screenshot at timeout shows the registration-blocking error dialog. Its
only action opens the Connections application so the user can connect to a PC
and complete registration.

## NAND And FAT Layout

The NAND partition table is the usual 16-byte BE-300 restore layout:

```text
[0] sectors 0x0000..0x001f  boot metadata
[1] sectors 0x0020..0x009f  KLOADER/SPL
[2] sectors 0x00a0..0x1cdf  compressed NK
[3] sectors 0x1ce0..0x7d7f  filesystem/container region
```

The usable FAT16 boot sector is not at partition 3's start. It is at
`0x4BDE00`:

```text
FAT16 offset: 0x4BDE00
FAT16 end:    0xFB0000
Label:        NandPT00
Sectors:      22417
```

`Program Files` contains the installed `.cpk` packages, but no registration
DLL:

```text
Program Files/Cassiopeia.dll:     missing
Program Files/Cassiopeia.dll.cpk: missing
```

## PowerOn.dll Evidence

Decoding NK from partition 2 gives:

```text
NK base:       0x80029000
NK entry:      0x80029004
module table:  0x806252C0
module count:  92
PowerOn.dll:   index 55, vbase 0x039A0000, vsize 0x9000
```

The reconstructed `PowerOn.dll` module contains the registration strings:

```text
rva 0x101C  Certificate
rva 0x1034  Verificate
rva 0x104C  \Nand Disk\Program Files\Cassiopeia.dll
rva 0x10EC  Connections
rva 0x1118  \Program Disk\Connections.exe
rva 0x6A46  registration modal text
rva 0x6C2E  Connections button text
rva 0x7352  user-registration reminder text
```

That makes `PowerOn.dll` the registration gate. The modal is not emitted by
the host PC software and it is not an emulator-side condition.

## PC Connect Package Notes

The local `pccon.7z` archive is PC Connect 1.01[US]. Its InstallShield package
metadata lists the expected PC-side tools and drivers, including:

```text
PCconnect.exe
PCLSTART.exe
PCLPATCH.exe
PCLPATCHS.exe
PCLASRV.exe
PCLASRVS.exe
PCLAUTL.dll
Jx740Api.dll
wceusbsh.sys
wceusbsh.inf
```

No `Cassiopeia.dll` string appears in the extracted InstallShield metadata.
The most likely flow is:

1. `PowerOn.dll` detects that `Cassiopeia.dll` is absent or invalid.
2. The user opens Connections and PC Connect establishes the device link.
3. PC software launches the retired
   `http://cassiopeia.casio.com/be300upg` registration flow using the Unit ID.
4. The site or PC software generates/downloads `Cassiopeia.dll`.
5. PC Connect copies that DLL to `\Nand Disk\Program Files`.

This is still an inference. The recovered DLL or a capture of the original
server response is needed to identify the certificate format, the Unit ID
binding, and the `Certificate`/`Verificate` ABI.

`CoLineCheck.exe.cpk` is a separate install/link monitor. Its strings mention
`PCLAUTL.dll`, `WilChkCnct`, `SOFTWARE\SETINSTALL`, and "Monitoring PC
connection...", so it is useful for the PC install path but it is not the
source of the registration modal.

## Reproducible Analyzer

Use the analyzer to reproduce the findings from the NAND image:

```bash
python3 tools/analyze_net_registration.py All_nand_Net.bin
```

If a real `Cassiopeia.dll` is recovered, copy it into a new NAND image:

```bash
python3 tools/analyze_net_registration.py All_nand_Net.bin \
  --install-cassiopeia-dll /path/to/Cassiopeia.dll \
  --out-image /tmp/All_nand_Net_registered.bin
```

The DLL-install path uses `mcopy` from mtools and verifies that the new FAT
directory entry exists. It does not modify the source image, synthesize a DLL,
patch NK, or alter emulator behavior.

## Registration Bypass

For local CE .NET experiments where registration with the retired service is
impossible, the analyzer can also patch a copy of the NAND image so
`PowerOn.dll` treats registration as complete:

```bash
python3 tools/analyze_net_registration.py All_nand_Net.bin \
  --patch-registration-check \
  --out-image /tmp/All_nand_Net_noreg.bin
```

This replaces the `PowerOn.dll` registration-check function at VA
`0x039A1370` with:

```mips
addiu v0, zero, 1
jr    ra
nop
```

The original function does the following:

1. waits briefly for `Program Disk` / `\Nand Disk\Program Files`
2. calls `LoadLibraryW("\\Nand Disk\\Program Files\\Cassiopeia.dll")`
3. resolves `Verificate` and `Certificate` with `GetProcAddressW`
4. computes a 16-bit value from the current time/date path
5. calls `Certificate(seed)` and compares its 16-bit return to that value
6. returns `Verificate()` when the certificate check matches
7. unloads the DLL and returns false on any failure

That gives the fake-registration ABI: a replacement `Cassiopeia.dll` would need
exports named `Certificate` and `Verificate`; `Certificate` must accept the
computed 16-bit seed and return the same 16-bit value, and `Verificate` must
return nonzero. Building that DLL would be a cleaner behavioral fake if a
WinCE/MIPS DLL toolchain is available, but the direct NAND patch is smaller
and deterministic.

Verification from the patched NAND:

```text
original words at NK flat offset 0x49E370:
  0x27BDFFC8 0xAFBF0034 0xAFBE0010

patched words:
  0x24020001 0x03E00008 0x00000000
```

Booting the patched copy reaches the touch calibration screen. That does not
prove final registration state by itself, because calibration is a separate
first-run blocker, but it confirms the previous registration modal is no
longer the first visible gate.
