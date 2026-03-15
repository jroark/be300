# BE-300 Hardware Survey

This utility is intended to be built with **eMbedded Visual Tools 3.0** using the **BE-300 SDK**. It probes physical hardware registers and memory ranges to provide "ground truth" for the emulator.

## Building Instructions

1.  **Launch eMbedded Visual C++ 3.0**.
2.  Go to `File` -> `New`.
3.  Select the **Projects** tab.
4.  Choose **WCE Application**.
5.  Set **Project name** to `HardwareSurvey`.
6.  Select **MIPS** as the target CPU (ensure others are unchecked).
7.  Click `OK` and choose **An empty project**.
8.  Go to `Project` -> `Add to Project` -> `Files`.
9.  Select the `main.cpp` provided in this directory.
10. Ensure the active configuration is set to **Win32 (WCE MIPS) Release**.
11. **Check Linker Settings**:
    *   Go to `Project` -> `Settings`.
    *   Select the `Link` tab.
    *   Ensure `coredll.lib` is listed in the **Object/library modules** box (it usually is by default).
12. Build the project (`Build` -> `Build HardwareSurvey.exe`).

## Running on the BE-300

1.  Transfer `HardwareSurvey.exe` to your BE-300 (via CF Card or ActiveSync).
2.  Run the application on the device.
3.  Wait for the "Survey Complete!" message box.
4.  Copy the resulting `\HardwareDump.txt` from the device's root directory back to your PC.

## Priority Data for Emulator Development

The most important sections for us right now are:
*   **Exception Vectors**: If this is non-zero, it confirms the device uses low-RAM vectors.
*   **VR4131 ICU**: This shows the interrupt mask and status of a running device.
*   **VRC4173 Video**: This gives us the hardware-configured display timings.

---

## Post-Boot Probe Utility v1 / v2

For the existing post-boot-only data collection utility, use `be300_probe_v1.cpp`.
That source now corresponds to the v2 report format and writes `\BE300Probe_v2.txt`.

### Build

Use the same eVC3 project flow as above, but add `be300_probe_v1.cpp` instead of `main.cpp`.

### Run / Output

1. Copy the built EXE to the BE-300.
2. Run it after WinCE fully boots.
3. Copy back `\BE300Probe_v2.txt`.

The output includes fixed sections:

- `--- BASELINE SNAPSHOT ---`
- `--- NAND WORKLOAD ---`
- `--- POST SNAPSHOT ---`
- `--- DIFF SUMMARY ---`
- `--- DRIVER INVENTORY ---`

The current checked-in source writes `\BE300Probe_v2.txt` and includes the v2 focus regions around:

- `0x00002200`
- `0x00001700`
- `0x00679400`
- `0x0A000C00`
- `0x0F000000`

---

## Post-Boot Probe Utility v3

For the cold-vs-warm comparison and retained-state survey, use `be300_probe_v3.cpp`.

### Build

Use the same eVC3 project flow as above, but add `be300_probe_v3.cpp` instead of `main.cpp`.

### Run / Output

1. Copy the built EXE to the BE-300.
2. Run it as early as practical after WinCE becomes usable.
3. At startup, choose the boot tag:
   - `Yes` = `Cold battery boot`
   - `No` = `Warm retained boot`
   - `Cancel` = `Unknown`
4. If the system remains responsive, wait for the completion message box.
5. Copy back the generated report from the same directory as the EXE.

The output file is no longer hardcoded to root. It is created beside the EXE with a unique name per run:

- `BE300Probe_v3_cold_<tick>.txt`
- `BE300Probe_v3_warm_<tick>.txt`
- `BE300Probe_v3_unknown_<tick>.txt`

If a file with that name already exists, the probe appends `_01`, `_02`, and so on until it finds a free filename.

The output includes fixed sections:

- `--- RUN METADATA ---`
- `--- EARLY SNAPSHOT ---`
- `--- SETTLE SNAPSHOT ---`
- `--- NAND WORKLOAD ---`
- `--- STORAGE WORKLOAD ---`
- `--- POST SNAPSHOT ---`
- `--- DIFF SUMMARY ---`
- `--- DRIVER INVENTORY ---`
- `--- STORAGE MANAGER ---`
- `--- FILESYSTEM ROOTS ---`

The v3 report uses a hybrid logging model so it completes in practice on hardware:

- Small regions (`<= 0x0200`) keep full raw word dumps.
- Large regions (`0x1000`) log region status + CRC32 and only keep focused raw subwindow dumps for:
  - `0x0A000C00..0x0A000C4F`
  - `0x0F000080..0x0F00011F`
  - `0x006794E0..0x0067951F`
- Diff sections keep all small-region word diffs, but cap large-region detailed diffs to the first 32 changed words per region/pair while still reporting total changed-word counts.

### Intended Use

`be300_probe_v3.cpp` is intended for cold-vs-warm comparison for emulator targeting. The highest-value runs are:

1. Battery-disconnect boot, then run `BE300Probe_v3` once.
2. Warm-retained boot, then run `BE300Probe_v3` once.
3. Repeat each mode multiple times and keep the boot tag with each returned report.
4. You can leave multiple runs in the same directory; each run generates a distinct filename instead of overwriting the previous result.

`be300_probe_v3.cpp` is now the heavier legacy probe. Returned runs showed that it still tends to:

- fault on the broad `0x0F000000..0x0F000FFF` capture at `0x0F000200`
- finish as `PARTIAL_ERROR`
- leave low-value shell-facing overhead in the storage-manager / filesystem sections

Use it only if you specifically want the older broader behavior.

---

## Post-Boot Probe Utility v4

For the recommended stock-shell hardware survey pass, use `be300_probe_v4.cpp`.

### Build

Use the same eVC3 project flow as above, but add `be300_probe_v4.cpp` instead of `main.cpp`.

### Run / Output

1. Copy the built EXE to the BE-300.
2. Run it as early as practical after WinCE becomes usable.
3. At startup, choose the boot tag:
   - `Yes` = `Cold battery boot`
   - `No` = `Warm retained boot`
   - `Cancel` = `Unknown`
4. The report is written beside the EXE with a unique name per run:
   - `BE300Probe_v4_cold_<tick>.txt`
   - `BE300Probe_v4_warm_<tick>.txt`
   - `BE300Probe_v4_unknown_<tick>.txt`
5. On success, the probe exits without a completion message box. A message box is only used for the startup prompt and hard file-creation failure.

The output includes the same fixed sections as v3:

- `--- RUN METADATA ---`
- `--- EARLY SNAPSHOT ---`
- `--- SETTLE SNAPSHOT ---`
- `--- NAND WORKLOAD ---`
- `--- STORAGE WORKLOAD ---`
- `--- POST SNAPSHOT ---`
- `--- DIFF SUMMARY ---`
- `--- DRIVER INVENTORY ---`
- `--- STORAGE MANAGER ---`
- `--- FILESYSTEM ROOTS ---`

### Why v4 is the recommended stock-shell probe

`be300_probe_v4.cpp` trims the behaviors that proved noisy or destabilizing in v3:

- replaces the faulting full `0x0F000000 size=0x1000` capture with two targeted safe windows:
  - `0x0F000000 size=0x0020`
  - `0x0F000080 size=0x00A0`
- flushes output incrementally so a hang is more likely to leave a readable partial text report
- reduces both NAND and storage workloads to 3 loops
- searches `\Nand Disk`, `\Storage Card`, `\CF Card`, and `\PC Card` recursively up to depth 3 for the first regular file
- keeps filesystem listings shallow:
  - root existence
  - first 8 entries only
- keeps registry inventory shallow:
  - key names
  - value names
  - DWORD/string data
  - one subkey level

### Intended Use

`be300_probe_v4.cpp` is the supported baseline for stock WinCE shell runs and cold-vs-warm comparison. The preferred workflow is:

1. Run v4 on battery-disconnect boots.
2. Run v4 on warm-retained boots.
3. Preserve each returned report separately.
4. Treat BE Shell or other community shell environments as best-effort only.

---

## Boot-ROM Survey Utility v1

For post-boot ROM visibility data collection, use `be300_bootrom_v1.cpp`.

### Build

Use the same eVC3 project flow as above, but add `be300_bootrom_v1.cpp` instead of `main.cpp`.

### Run / Output

1. Copy the built EXE to the BE-300.
2. Run it after WinCE fully boots.
3. Copy back `\BE300BootROM_v1.txt`.

The report includes:

- Full capture of `PA 0x1FC00000..0x1FC03FFF` (16 KB) with per-page CRC32 fingerprints.
- Explicit reset-window dump of `PA 0x1FC00000..0x1FC003FF`.
- BCU readback dump of `PA 0x0F000000..0x0F00001F` with explicit `ROMSIZEREG` and `ROMSPEEDREG` lines.
- Three in-run capture passes and pass-to-pass fingerprint/data change summaries.
