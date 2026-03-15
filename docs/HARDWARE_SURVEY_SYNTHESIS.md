# Hardware Survey Synthesis

## Artifacts Reviewed
This memo consolidates the current `hardware_survey/*.txt` artifacts that were used to drive emulator-target conclusions in this pass:

- `hardware_survey/HardwareDump.txt`
- `hardware_survey/HardwareDump 2.txt`
- `hardware_survey/HardwareDump6.txt`
- `hardware_survey/BE300Probe_v1.txt`
- `hardware_survey/BE300Probe_v2.txt`
- `hardware_survey/BE300BootROM_v1.txt`

This memo is intentionally limited to those six survey artifacts. It does not treat later emulator traces, `cyace_probe` serial logs, or ad hoc discussion as equivalent ground-truth inputs, except where explicitly called out as a later observation.

Additional post-boot probe integration evidence was reviewed from the current `BE300Probe_v3_*.txt` outputs in `hardware_survey/`:

- Completed text reports:
  - `BE300Probe_v3_cold_21381.txt`
  - `BE300Probe_v3_cold_27030.txt`
  - `BE300Probe_v3_warm_12105.txt`
  - `BE300Probe_v3_warm_14694.txt`
  - `BE300Probe_v3_warm_42066.txt`
  - `BE300Probe_v3_warm_48518.txt`
  - `BE300Probe_v3_warm_65448.txt`
  - `BE300Probe_v3_warm_9120.txt`
  - `BE300Probe_v3_unknown_154600.txt`
- Aborted NUL-only stub files:
  - `BE300Probe_v3_cold_43081.txt`
  - `BE300Probe_v3_warm_33316.txt`

## High-Confidence Stable Inputs

### Stable VR4131 BCU / core-page values at `0x0F000000`
These values are stable across the historical hardware dumps, both post-boot probe runs, and the dedicated boot-ROM survey BCU readback.

| Address | Stable value(s) | Evidence | Notes |
| --- | --- | --- | --- |
| `0x0F000000` | `0000000C 100C4444 26721242 00000000` | `HardwareDump.txt`, `HardwareDump 2.txt`, `HardwareDump6.txt`, `BE300Probe_v1.txt`, `BE300Probe_v2.txt`, `BE300BootROM_v1.txt` | Safe readback seed candidate. |
| `0x0F000010` | `00005002 0883020C 00000000 00000000` | `BE300Probe_v1.txt:3351`, `BE300Probe_v2.txt:4636`, `BE300BootROM_v1.txt:1112` | Includes the stable `0x00005002` word seen in the boot-ROM probe. |
| `0x0F000020` | `01FFF800 01FFF800 01FFF800 01FFF800` | `BE300Probe_v1.txt:3352`, `BE300Probe_v2.txt:4637`, `HardwareDump6.txt` | Stable in every later core-page capture. |
| `0x0F000030` | `00003800 00003800 00000000 00000000` | `BE300Probe_v1.txt:3353`, `BE300Probe_v2.txt:4638`, `HardwareDump6.txt` | Stable in every later core-page capture. |
| `0x0F000040` | `00010000 00000000 00000000 00000000` | `BE300Probe_v1.txt:3354`, `BE300Probe_v2.txt:4639`, `HardwareDump6.txt` | Stable later-page readback. |
| `0x0F000060` | `09020902 ...` | `BE300Probe_v1.txt:3356-3357`, `BE300Probe_v2.txt:4641-4642`, `HardwareDump6.txt` | Stable patterned timing/config page. |

### Stable VR4131 ICU values at `0x0F000080`
`HardwareDump 2.txt`, `HardwareDump6.txt`, and both post-boot probe runs agree on the ICU window. `HardwareDump.txt` appears to duplicate the BCU window under the ICU label and should not be used for ICU seeding.

| Address | Stable value(s) | Evidence | Notes |
| --- | --- | --- | --- |
| `0x0F000080` | `00000004 00000000 00000000 00000307` | `HardwareDump 2.txt:43-46`, `HardwareDump6.txt`, `BE300Probe_v1.txt:3358-3361`, `BE300Probe_v2.txt:4643-4646` | High-confidence stable ICU readback. |
| `0x0F000090` | `00000000 00000E83 00000001 00000000` | `HardwareDump 2.txt`, `HardwareDump6.txt`, `BE300Probe_v1.txt:3359`, `BE300Probe_v2.txt:4644` | Stable across later captures. |
| `0x0F0000A0` | `00000001 00000000 00000000 00000000` or `...00010000` later | `HardwareDump 2.txt`, `HardwareDump6.txt`, `BE300Probe_v1.txt:3360`, `BE300Probe_v2.txt:4645/9534` | Treat `0x0F0000AC` / nearby PMU-adjacent words as less stable than `0x80` / `0x8C`. |

### Stable post-boot VRC4173 C-window values at `0x0A000C00..0x0A000C4C`, excluding `0x0C38`
The post-boot surveys agree strongly on the `0x0A000Cxx` window except for `0x0C38`.

| Address | Stable value | Evidence |
| --- | --- | --- |
| `0x0A000C00` | `0x00000020` | `BE300Probe_v1.txt:2770/6374`, `BE300Probe_v2.txt:2770/7659` |
| `0x0A000C04` | `0x00000002` | same as above |
| `0x0A000C08` | `0x00000050` | same as above |
| `0x0A000C0C` | `0x000000F1` | same as above |
| `0x0A000C10` | `0x00000002` | same as above |
| `0x0A000C24` | `0x00000014` | `BE300Probe_v1.txt:2772/6376`, `BE300Probe_v2.txt:2772/7661` |
| `0x0A000C2C` | `0x00000001` | same as above |
| `0x0A000C30` | `0x0000E000` | `BE300Probe_v1.txt:2773/6377`, `BE300Probe_v2.txt:2773/7662` |
| `0x0A000C34` | `0x000001FF` | same as above |
| `0x0A000C3C` | `0x00001F1F` | same as above |
| `0x0A000C40` | `0x00000500` | `BE300Probe_v1.txt:2774/6378`, `BE300Probe_v2.txt:2774/7663` |
| `0x0A000C48` | `0x0000030F` | same as above |
| `0x0A000C4C` | `0x00000000` | same as above |

### Boot ROM fingerprints and reset vector bytes
The dedicated boot-ROM survey shows the visible `0x1FC00000..0x1FC03FFF` window is stable across all three in-run passes.

| Item | Value | Evidence | Notes |
| --- | --- | --- | --- |
| Full ROM CRC (`16 KB`) | `0xFA3B5582` | `BE300BootROM_v1.txt:11/1117/1197` | Stable across all three passes. |
| Page 0 CRC (`0x1FC00000..0x1FC00FFF`) | `0x93B00B74` | `BE300BootROM_v1.txt:12/1118/1198` | Stable across all three passes. |
| Reset vector words | `00000000 3C1ABFC0 375A02F0 03400008` | `BE300BootROM_v1.txt:18`, `:1044`, `:1124`, `:1204` | Safe boot-ROM content reference. |
| BCU readback during boot-ROM survey | `0x0F000000: 0000000C 100C4444 26721242 00000000` | `BE300BootROM_v1.txt:1111/1191/1271` | Confirms BCU stability independent of the post-boot probes. |

### `0x2200` context-block words observed in post-boot probes
The `0x00002200..0x000022FF` context block is real and structured, but not fully stable across post-boot captures.

| Address | Probe v1 | Probe v2 | Notes |
| --- | --- | --- | --- |
| `0x00002220` | `0185F124` | `0185F124` | Stable across both post-boot probes. |
| `0x00002224` | `00000001` | `00000001` | Stable. |
| `0x00002228` | `FFFFD760` | `FFFFD760` | Stable stack VA. |
| `0x0000222C` | `80000005` | `80000005` | Stable. |
| `0x00002270` | `BDBDBDBD` | `BDBDBDBD` | Stable sentinel. |
| `0x00002274` | `80096894` | `80096894` | Stable resume/continuation value. |
| `0x000022D0` | `8007A178` | `8007A178` | Stable function/code reference. |
| `0x000022D4` | `00300307` | `14400307` | Changed between v1 and v2. |
| `0x00002200` | `01860000` | `01860000` | Stable header word. |
| `0x00002208` | `0024A874` | `0002D1C2` | Not stable. |

### `0x1700` caller-frame / stack-frame words captured in v2
`BE300Probe_v2.txt` captured both the baseline and post-workload state for the `0x1700` page, which is directly relevant to the WinCE caller/callee frame investigation.

| Address | v2 baseline | v2 post | Evidence |
| --- | --- | --- | --- |
| `0x00001760` | `0x80D46720` | `0x000000FB` | `BE300Probe_v2.txt:3981`, `:8870`, `:10021` |
| `0x00001764` | `0x80D46720` | `0x0000001F` | `BE300Probe_v2.txt:3981`, `:8870`, `:10022` |
| `0x00001788` | `0x800833C4` | `0x80D46720` | `BE300Probe_v2.txt:3983`, `:8872`, `:10023` |
| `0x0000179C` | `0x80F308BC` | `0x80FFCB54` | `BE300Probe_v2.txt:3984`, `:8873`, `:10024` |
| `0x000017B4` | `0x083FFE94` | `0xBDBDBDBD` | `BE300Probe_v2.txt:3986`, `:8875`, `:10029` |

## Cold/Warm-Sensitive or Post-Boot-Only State

### `0x0A000C38` is phase-sensitive
`0x0A000C38` changes within both post-boot probe runs, and the before/after values differ between runs:

- `BE300Probe_v1.txt:2773/6377/7389/7408`
  - baseline `0x000000B3`
  - post `0x00000032`
- `BE300Probe_v2.txt:2773/7662/9960/10053`
  - baseline `0x000000E5`
  - post `0x00000027`

This makes `0x0A000C38` a state-machine / phase-model target, not a fixed full-word seed target.

### `0x006794EC..0x00679510` is post-boot structured data, not obviously callback pointers
The `0x00679400` window is clearly populated with UTF-16-like textual or structured records in both v1 and v2.

Examples:

- `BE300Probe_v1.txt:2078-2079` shows words corresponding to text-like sequences near `0x006794E0`.
- `BE300Probe_v2.txt:2078-2079` and `:6967-6968` show a different but still text-like / record-like region.
- Focus words remain stable within each run but differ across runs:
  - `BE300Probe_v1.txt:7415-7416` -> `0x006E0065`, `0x00610064`
  - `BE300Probe_v2.txt:10060-10061` -> `0x0064002E`, `0x006C006C`

Treat this as structured post-boot RAM content unless later evidence proves these words are actually executable callback registration state.

### `0x2200` context-block contents are partly stable and partly variable
The context block includes some stable structural words (`0x2220`, `0x2228`, `0x222C`, `0x2274`, `0x22D0`) and some words that differ between v1 and v2 (`0x2208`, `0x223C`, `0x2284`, `0x229C`, `0x22A4`, `0x22D4`).

This makes the block useful for targeted investigation, but not safe for blind whole-block seeding.

### `0x1700` page is live working RAM, not a static warm seed
`BE300Probe_v2.txt:10018-10039` shows many changing words in the `0x1700` page. That confirms this area is active caller/frame state and should not be treated as a static seed region.

### `0x0F000100+` is less stable than the BCU / ICU base windows
`HardwareDump6.txt` and the post-boot probes agree on the low BCU / ICU windows, but the `0x100` / `0x110` region changes across captures and likely reflects timer / PMU / retained-state behavior. That region needs cold-vs-retained comparison, not unconditional seeding.

## Post-Boot Probe v3 Integration

The current `BE300Probe_v3_*.txt` runs materially refine the next hardware-collection step.

### Observed facts from v3

1. Output-path behavior is fixed.
   - Completed runs correctly write beside the EXE, for example under `\Storage Card\Program Files\`.

2. Most v3 runs complete as partial text reports rather than hard crashes.
   - The dominant terminal status is `probe_complete status=PARTIAL_ERROR`.

3. All completed runs fault on the same VR4131 read span.
   - Every completed file includes:
     - `[READ_FAULT] PA=0x0F000200 size=0xE00 code=0xC0000005`
   - The useful implication is that `0x0F000080..0x0F00011F` is readable and informative, while the later part of the `0x0F000000` page is not safe to probe as a single full `0x1000` window.

4. `0x0F000080..0x0F00011F` is already sufficient for the stable ICU/PMU words we care about.
   - The focused dump survives in completed runs even when the broader `0x0F000000` page read is only partial.

5. `0x006794E0..0x0067951F` is confirmed to hold post-boot textual or record-like content.
   - UTF-16 hints in the v3 outputs include strings such as `imodem.dll`, `Calendar.exe` fragments, and other module-name-like text.
   - This reinforces that the `0x006794xx` window should not be treated as a fixed callback-pointer seed block.

6. `NAND status=ABSENT` in v3 is a file-selection artifact, not evidence that `\Nand Disk` does not exist.
   - Current v3 only looks for root-level files under `\Nand Disk`.
   - The returned reports already show `\Nand Disk\Program Files` exists, so recursive search is required.

7. Cold/warm/device runs already show multiple CRC families.
   - Distinct CRC families are visible for:
     - `0x00002200`
     - `0x00001700`
     - `0x00051600`
     - `0x0A000000`
     - the readable subwindows of `0x0F000000`
   - This is enough to justify clustering by reset mode and device, rather than promoting those pages directly into fixed emulator seeds.

8. The two aborted NUL-only files show that end-of-run flush/close is still not enough if the system hangs before enough text is written.
   - Future probe versions should flush incrementally after each major section and avoid success-path UI that is not needed for data capture.

## Post-Boot Probe v4 Integration

The first stock-shell `BE300Probe_v4` batch is materially better than v3 and is now good enough to use as the fixed collection baseline.

### Current v4 batch summary

- Dataset:
  - `BE300Probe_v4_cold_22664.txt`
  - `BE300Probe_v4_cold_26319.txt`
  - `BE300Probe_v4_warm_11583.txt`
  - `BE300Probe_v4_warm_81147.txt`
  - `BE300Probe_v4_warm_9550.txt`
  - `BE300Probe_v4_unknown_112248.txt`
- All six reports are readable text and end with `probe_complete status=OK`.
- The old v3 fault is gone:
  - no v4 run contains `[READ_FAULT] PA=0x0F000200 ...`
- The narrowed `0x0F000000` / `0x0F000080` windows are sufficient to capture the BCU and ICU/PMU words we care about.
- NAND workload is now real:
  - every current v4 run selected `\Nand Disk\Program Files\ieceext.dll.cpk`
  - selection method is `found_by=recursive`
- Storage workload is stable on the current setup:
  - current v4 runs selected the EXE path on `\Storage Card\Program Files`

### Stable enough in the current one-device stock-shell sample

The current v4 clustering report (`hardware_survey/BE300Probe_v4_cluster_report.md`) shows these fields are stable across all current runs and all three phases:

- `vr4131_bcu_window` CRC
- `callback_table_51600` CRC
- `icu_0080`
- `nand_c30`
- `nand_c34`
- `nand_c48`
- `nand_c4c`
- `stack_1760`
- `stack_1764`
- `stack_179C`
- `stack_17B4`

Only the first six items above are currently attractive emulator readback targets. The stable `0x1700` words are still live stack-frame values and should not be promoted into warm seeds just because they happened to stay constant in this small batch.

### Phase-varying in the current one-device stock-shell sample

These fields change within a run and are therefore dynamic by construction:

- `nand_sideband_c38`
- `pmu_0100`
- `stack_frame_1700` CRC
- `vrc4173_base` CRC
- `vr4131_icu_pmu_window` CRC

For `nand_sideband_c38`, that confirms the earlier conclusion that it needs a phase model rather than a fixed seed. For `pmu_0100` and the two CRC windows, the current sample shows per-run uniqueness rather than a reusable cold/warm family.

### Multi-family but not yet cold/warm-correlated

The current v4 clustering report shows the following fields already split into multiple stable families across runs, but the families do **not** yet correlate cleanly with the current `cold` vs `warm` labels:

- `resume_context_2200` CRC
- `callback_globals_80679400` CRC
- `ctx_2220`
- `ctx_2228`
- `ctx_2274`
- `g94EC`
- `g94F0`
- `g9508`
- `g9510`

Current families overlap across cold and warm runs. Example:

- `resume_context_2200`
  - one family appears in both `BE300Probe_v4_cold_26319.txt` and the warm runs `BE300Probe_v4_warm_81147.txt` / `BE300Probe_v4_warm_9550.txt`
  - another family appears in `BE300Probe_v4_warm_11583.txt` and the `unknown` run

That means these are still reset-sensitive or runtime-sensitive candidates, but not yet safe to map directly onto a simple cold/warm emulator profile.

### Post-boot textual/object evidence remains confirmed

`0x006794E0..0x0067951F` continues to decode to module-name or record-like text in v4:

- `imodem.dll`
- `FileCalendar.exe` fragments
- other short text-like variants such as `X?`

This reinforces that `g94EC/g94F0/g9508/g9510` should still be treated as structured post-boot data, not as primitive callback-pointer slots.

## Corrections to Prior Assumptions

1. `0x0A000C38` is **phase-sensitive / non-stable**, not a fixed full-word seed candidate.
2. `0x006794EC..0x00679510` in the post-boot probes is **not a simple callback-pointer block**. Treat it as structured/textual post-boot data unless later evidence proves otherwise.
3. `BE300Probe_v1.txt` and `BE300Probe_v2.txt` are **post-boot captures**. They are not SPL-entry or NK-entry ground truth.
4. `HardwareDump.txt` duplicates the BCU page under the ICU / PMU labels. For actual ICU values, prefer `HardwareDump 2.txt`, `HardwareDump6.txt`, and the post-boot probes.
5. Boot consistency depends on **reset mode / retained state**, based on later real-hardware observations outside these six `.txt` artifacts. That means post-boot values alone are not enough; cold-vs-retained comparison is now the main missing capture.

## Direct Emulator Targets

### Stable readable subwindows worth seeding or mirroring

- VR4131 BCU/core-page words at `0x0F000000..0x0F00007F`
- VR4131 ICU words at `0x0F000080..0x0F00009F`
- Post-boot VRC4173 C-window words at `0x0A000C00..0x0A000C4C`, **excluding `0x0A000C38`**
- Boot ROM visibility / reset window bytes rooted at `0x1FC00000`
- The currently stable v4 `callback_table_51600` page, if later emulator work needs a post-boot callback table reference rather than a boot-time seed

These have cross-artifact agreement strong enough to use as emulator readback targets, provided the boot path being modeled is compatible with post-boot state.

### Reset-sensitive RAM / context regions that need clustering first

- `0x0A000C38`
- `0x0F000100..0x0F00011F`
- `0x00002200..0x000022FF`
- `0x00001700..0x000017FF`

These areas are still important, but they should be modeled as workload-, time-, or reset-dependent state rather than dumped into a fixed seed blob.

### Post-boot textual/object regions that should not be treated as fixed seeds

- `0x00679400..0x0067951F`
- `0x0066BF00..0x0067C0FF` object/string tables
- any UTF-16/module-name-like subwindow captured in the v2/v3 post-boot probes

These are useful for identifying what WinCE drivers and services are alive, but they are not direct emulator seed targets.

### Do not treat as direct SPL / NK-entry warm seeds

- Any whole-page content from `BE300Probe_v1.txt`
- Any whole-page content from `BE300Probe_v2.txt`
- The `0x1700` stack-frame page
- The `0x2200` context page
- The `0x00679400` structured text / record window

## Remaining Gaps and Required Hardware Captures

1. **Cold-vs-retained comparison of the same post-boot ranges**
   - Capture the same ranges immediately after a battery-disconnect boot and after a warm-retained boot.
   - Highest-priority ranges:
     - `0x00002200..0x000022FF`
     - `0x00001700..0x000017FF`
     - `0x00679400..0x006795FF`
     - `0x0F000080..0x0F00011F`
     - `0x0A000C00..0x0A000C4F`

2. **Early-vs-settled post-boot comparison**
   - The next probe should capture both an immediate post-boot snapshot and a delayed settled snapshot before storage workloads.
   - This is needed to separate reset-conditioned state from ordinary post-login driver churn.

3. **Storage-sideband comparison**
   - Capture storage roots and run controlled file-read workloads from `\Nand Disk` and removable storage roots (`\Storage Card`, `\CF Card`, `\PC Card`) when available.
   - This will help isolate companion/storage state changes that may be relevant to Linux handoff consistency.

4. **Retained-state interpretation**
   - If later cold-vs-retained captures show stable differences in ICU/RTC/PMU or companion-chip windows, those should become explicit emulator profile inputs rather than being inferred indirectly from Linux outcomes.

5. **Immediate action for the next hardware pass**
   - Keep `hardware_survey/be300_probe_v4.cpp` unchanged and collect one more controlled same-device batch:
     - same device only
     - stock WinCE 3.0 shell only
     - same EXE location, ideally `\Storage Card\Program Files`
     - 3 additional cold battery-disconnect boots
     - 3 additional warm-retained boots
     - run once per boot as early as practical after the shell is usable
     - keep original filenames after copy-back
     - keep `unknown` runs as extra evidence but do not use them as primary cold/warm labels
   - Re-run the host-side clustering tool on the expanded set and only then decide whether the family-sensitive fields belong in an emulator reset profile or should be classified as post-boot/runtime-only state.

That is the purpose of `tools/cluster_probe_v4.py` plus the unchanged `hardware_survey/be300_probe_v4.cpp`.
