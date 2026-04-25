# Handoff — Post Pass 35 + Pass 36 · welcome.exe never spawns; paint pipeline dormant

**Date:** 2026-04-22
**Predecessors:**
- `docs/HANDOFF_POST_PASS34_HIBERNATE_AND_AB_WINDOW_2026-04-22.md` (Pass 34 — HIBERNATE + 0xAB window)
- `docs/HANDOFF_POST_PASS33_AIU_HYPOTHESIS_REFUTED_2026-04-22.md` (Pass 33 — AIU refuted)
- `docs/HANDOFF_POST_PASS32_LAUNCHER_BLOCKS_ON_BOOTEXE_READY_2026-04-22.md` (Pass 32 — launcher table)

**State of tree (both working-copy, uncommitted from Pass 34):**
- `src/be300.h` +5 lines — HIBERNATE declarations
- `src/machine_be300.c` +127 lines — 0xAB000000 companion-window stub

Pass 35/36 probe additions are **fully reverted**. `src/be300_probe.c` clean.

## 1. Finding in one sentence

The post-Boot.exe paint stall is **not** a hardware gap and **not** a VirtualCopy failure — it's that **welcome.exe (the first-boot calibration wizard) never spawns in our emulator**, so no custom WndProc ever runs to paint the primary framebuffer, and OAL's kernel-mode "Starting" splash at PC `0x80F037CC` stays on screen indefinitely.

## 2. What Pass 35 + Pass 36 established (all with evidence)

### 2.1 Three hypotheses refuted, in order

| Hypothesis | Evidence | Status |
|-----------|----------|--------|
| Missing VRC4173 LCDC / VBLANK IRQ blocks ddi.dll's blit engine | `FUN_01a5bf00` decompile: 12-param software blit, no register reads, no `WaitForSingleObject`, no `InterruptInitialize`. Pure software. | **Refuted** |
| `VirtualCopy(va, 0xAA200000, 0x40000, 0x204)` fails silently for the primary FB | 5 VirtualCopy return-PC probes (`0x01a54608, 0x01a546e8, 0x01a54718, 0x01a54754, 0x01a54790`) all show `v0=1 (SUCCESS)`. | **Refuted** |
| ddi.dll spins reading stale VRC4173 register values | VRC4173-latch probe filtered to ddi PC range shows only **2 offsets written, 0 reads** in 60 s: `0x0C4C × 54` state-machine writes `{0, 2, 4, 6}`; `0x098C × 3` writes `0x7AC` (backlight intensity, matches `PA 0x0A000980 = BlgReg_BASE` per `hardware.txt`). | **Refuted** |

### 2.2 The real blocker

**Coredll paint-API exec watches, 200 s cold boot** (VAs resolved from `01_coredll.dll.bin` PE export dir at file off `0x5CE70`):

| API | Hits | Notes |
|-----|-----:|-------|
| `BitBlt`, `PatBlt`, `StretchBlt` | **0** | No user-mode blit ever |
| `BeginPaint`, `EndPaint`, `GetDC`, `ReleaseDC` | **0** | No paint session entered |
| `InvalidateRect`, `ValidateRect` | **0** | No public invalidate |
| `DrawTextW`, `ExtTextOutW`, `Rectangle` | **0** | No drawing primitives |
| `CreateCompatibleDC`, `CreateCompatibleBitmap`, `SelectObject` | **0** | No off-screen DC |
| `GetUpdateRect`, `GetUpdateRgn` | **0** | Nobody queries update regions |
| `CallWindowProcW` | **0** | No subclassed WndProc invocations |
| `UpdateWindow` | 3 | 2 gwes-internal HWND 0x0013eb10; 1 coshell desktop HWND 0x0013eed0 |
| `ShowWindow` | 3 | 2 SW_HIDE; 1 SW_SHOW on coshell desktop |
| `DefWindowProcW` | 34 | **All windows use the default WndProc**, including for WM_PAINT |
| `FillRect` | 1 | One fill during coshell init at PC `0x000120b4`, HDC=0x67 |
| `GetMessageW` / `DispatchMessageW` | 11 / 6 | Message pump mostly idle |
| `WaitForSingleObject` | 1964 | Threads idle-wait dominantly |
| **`TouchCalibrate`** | **0** | **Calibration dialog never triggered** |
| **`ShellExecuteEx`** | **0** | No shell-execute launches |
| `CreateProcessW` (user-mode) | 4 | shell.exe PC 0x00012848 ×2; modmonitor PC 0x018A184C ×2. None are welcome.exe |
| `RegOpenKeyExW` / `RegQueryValueExW` | 10093 / 10538 | Heavy registry activity — processes consulting config |

**Key trace of coshell's desktop window** (HWND 0x0013eed0, ASID 0x06):

```
DefWindowProcW msg=0x86 (WM_NCACTIVATE)
DefWindowProcW msg=0x30F
DefWindowProcW msg=0x06  (WM_ACTIVATE)
DefWindowProcW msg=0x07  (WM_SETFOCUS)
DefWindowProcW msg=0x47  (WM_WINDOWPOSCHANGED)
ShowWindow(SW_SHOW)
DefWindowProcW msg=0x03  (WM_MOVE)
DefWindowProcW msg=0x05  (WM_SIZE)
DefWindowProcW msg=0x47  (WM_WINDOWPOSCHANGED ×2)
UpdateWindow
DefWindowProcW msg=0x0F  (WM_PAINT)  ← handled by stub, no draw
FillRect                 ← coshell init, HDC=0x67 (not primary)
```

`WM_PAINT` is dispatched but goes to the **default window proc stub**, which just validates the rect without drawing. `WM_ERASEBKGND (0x14)` is never dispatched (consistent with `BeginPaint` never being called, since BeginPaint triggers WM_ERASEBKGND).

### 2.3 Process spawn evidence

Kernel-level `spawn_module_createprocess_path` (PC `0x8008690C`) decoded via UTF-16 of a0:

```
Boot 1: filesys → SystemPatchModule → shell → device → gwes → Boot → modmonitor
<KjCMU warm reset>
Boot 2: filesys → SystemPatchModule → shell → device → gwes → Boot → modmonitor → coshell
```

**welcome.exe is never spawned.** The Pass 32 launcher table at user VA `0x0203b4d0` has 5 entries `{0x0A shell, 0x14 device, 0x1E gwes, 0x3C coshell, 0x3B Boot}`. welcome is not in the launcher's job list.

### 2.4 welcome.exe in the image

- `welcome.exe` is module `[51]` in the XIP image, 5 KB, vbase `0x00010000`.
- The ASCII string `welcome.exe` exists exactly **once** in `docs/nk_decompressed.bin` at file offset `0x2D0FF1`, inside what looks like the **packed ROM registry** (preceding `0x01 0x01 0x01 0xFF` record-separator pattern; followed by unrelated UTF-16 "WinCE/Name/Ident/SslSetProtocols" — these are adjacent registry entries, not related to welcome).
- The string is **not present** in any of the 95 extracted `.bin` files' hard-coded pools — confirmed by scanning all of `build-host/modules/*.bin`. So no process spawns welcome by hard-coded name.

### 2.5 Framebuffer evidence

- `fb_body_kseg1_writes = 600934` over 200 s — **every single write** to PA `0x0A200000..0x0A226000` comes from kernel OAL PCs `0x80F037CC` / `0x80079130` redrawing the "Starting" splash at ~3 kHz.
- `ddi_mapped_user_va = 0/0` (absent from summary = zero reads, zero writes) for VA `0x001E0000..0x00206000`. User-mode never touches the mapped primary FB VA.
- VirtualCopy succeeds for the mapping, so the mapping lives in gwes's page table. It's just never accessed because GDI never routes primary blits through it.

Screenshot SHA across all 2026-04-22 runs: `e8a8c83cd66b9327f50fc1827eada71fb028b332` (the OAL "Starting" splash).

## 3. Stall chain, clearly localized

```
packed ROM registry at NK off 0x2D0FF1  →  (references welcome.exe via some key)
      ↓
something SHOULD read that key and spawn welcome.exe on first cold boot
      ↓
[GAP IN EMULATOR: welcome.exe never spawns — unknown who should spawn it]
      ↓
welcome.exe would call TouchCalibrate (coredll 0x01F91640) + paint via custom WndProc
      ↓
custom WndProc calls BeginPaint + FillRect/DrawTextW + EndPaint
      ↓
GDI routes primary blits through ddi.dll's iFunc table
      ↓
ddi.dll writes PA 0x0A200000 → overwrites OAL's "Starting" splash
      ↓
user sees calibration cross-hair (real-HW step 5)
```

**The GAP** is between the packed registry data and the first user-mode spawn. Something reads that registry and decides either "spawn welcome" or "don't" — and in our emulator always chooses "don't".

## 4. Ranked next steps for Pass 37

Options are independent and can be prioritized by the user. Each is scoped to one session.

### Option A — Decode the packed ROM registry around welcome.exe (highest signal)

Find what key in the registry references welcome.exe at NK offset `0x2D0FF1`. Candidate key paths on WinCE 3.0:
- `\HKLM\init\LaunchNN` (the launcher-table mechanism — but Pass 32 already enumerated the 5 launcher entries, welcome not among them).
- `\HKLM\Software\Microsoft\Shell\RunOnce` / `Run`.
- `\HKLM\Init\BootVars` or Casio-specific first-boot key.
- A Casio BE-300 OEM shell init key.

**Approach:**
1. filesys.exe implements the packed-registry reader. Extract `filesys.exe` (module 0 in XIP index), find the registry-walk code, and decode the binary format.
2. Alternatively, instrument the runtime: hook `RegOpenKeyExW` (VA `0x01FB2DB8`) to dump `a1` (UTF-16 subkey name) for the first N calls. With 10 093 hits in 180 s, the first few hundred will show the critical keys each process opens at startup. Search for key names matching welcome or LaunchXX patterns.
3. Compare against public WinCE 3.0 BE-300 SDK docs (available in the user's eMbedded Visual C++ 3.0 VM) for what registry keys the OEM shell expects.

**Effort:** medium. **Signal:** highest — tells us exactly who should spawn welcome.

### Option B — Decode welcome.exe's imports

Extract the PE import table from `build-host/modules/51_welcome.exe.bin` (5 KB) using the same heuristic scan approach that worked for coredll's export table (find known string, scan backwards for `IMAGE_IMPORT_DESCRIPTOR`). This reveals:
- Whether welcome imports `TouchCalibrate` (confirms it drives the calibration dialog).
- What else it does (shell APIs, registry queries, file I/O).

**Effort:** low. **Signal:** medium — confirms the "welcome.exe → TouchCalibrate" link, rules out other hypotheses about what welcome does.

### Option C — Decode the 4 user-mode CreateProcessW a0 strings

shell.exe PC `0x00012848` and modmonitor PC `0x018A184C` each fire 2 CreateProcessW calls (one per boot cycle). Decoding `a0` as UTF-16LE tells us what those spawns are (and rules them out as welcome spawners if they're not).

**Approach:** add a specialized probe hook at `coredll_CreateProcessW` (VA `0x01F8F89C`) that calls `be300_probe_read_utf16le_ascii(cpu, a0, ...)` and logs the image name (mirrors the existing `spawn_module_createprocess_path` probe at PC `0x8008690C`).

**Effort:** low. **Signal:** low–medium — fills a blind spot, but unlikely to find welcome. Useful for completeness.

### Option D — Register-key sampling for first-boot flags

Sample `a1` of the first ~200 `RegOpenKeyExW` calls to look for keys like `FirstBoot`, `CalibrationComplete`, `Setup`, `OOBE`, `RunOnce`, or Casio-specific first-boot markers. If any process checks a flag and skips welcome because the flag says "already done", we've found the lever.

**Effort:** low–medium (same instrumentation pattern as Option C, plus grep work). **Signal:** medium — could reveal a missing flag we need to set (or not set) on cold boot.

### Option E — Boot.exe's conditional spawn logic

Pass 32 established that Boot.exe at WinMain UVA `0x00012bd4` checks an init-marker file at UVA `0x00015390`; `-1` → triggers reboot via NK `0x800a84a8`, else → early exit. Investigate whether Boot.exe has a branch that **spawns welcome** before the early exit, and whether that branch is taken based on some other check we've missed.

**Approach:** decompile Boot.exe (module `[92]`, 6 KB) in Ghidra, trace paths from WinMain.

**Effort:** medium. **Signal:** medium — Boot.exe's role is partially decoded already; finishing the decode could reveal the welcome launcher.

### Out of scope / refuted — do NOT revisit

- VRC4173 LCDC / VBLANK IRQ (Pass 35 Phase A).
- VirtualCopy failure for primary FB (Pass 35 Phase B1).
- ddi.dll reading stale VRC4173 values (Pass 35 Phase B2).
- Audio / VRC4173 AIU (Pass 33).
- gwes_worker WFSO(0x000B6834) block (Pass 34 — real HW exhibits same block).
- RAM seeds, `resume_ctx` shortcuts, guest patches (CLAUDE.md §Emulation Philosophy).

## 5. Recommended sequence for Pass 37

The recommendation is **A → B → (D or E)** in that order.

- **A first** because decoding the packed registry directly tells us the mechanism. If the key is `\HKLM\init\Launch99=welcome.exe` but filtered by some condition, we need to understand why the condition fails.
- **B second** as a quick confirm of welcome's role (5 KB, trivial to parse).
- **D or E** depending on A's outcome: if A reveals the key is condition-gated (e.g., guarded by a RunOnce flag), D samples the flag queries. If A reveals a Boot.exe-mediated launch, E decompiles Boot.exe further.

**Option C is lowest priority** — useful only to rule out shell/modmonitor spawns as red herrings; the probe is 10 lines and can be batched with any other option.

## 6. Key VAs / file offsets for Pass 37 reuse

### Coredll export VAs (resolved from `01_coredll.dll.bin`, IMAGE_EXPORT_DIRECTORY at file off `0x5CE70`)

```
BitBlt=0x01F8FCDC  PatBlt=0x01F8FC64  StretchBlt=0x01F90868
BeginPaint=0x01F93D94  EndPaint=0x01F93D28  GetDC=0x01F93CBC  ReleaseDC=0x01F93C30
UpdateWindow=0x01F93B90  InvalidateRect=0x01F941C4  ShowWindow=0x01F93BB0
DefWindowProcW=0x01F923F8  CallWindowProcW=0x01F943E0
FillRect=0x01F900DC  DrawTextW=0x01F8FF5C  ExtTextOutW=0x01F90054  Rectangle=0x01F90580
CreateCompatibleDC=0x01F8FD84  CreateCompatibleBitmap=0x01F8FD64  SelectObject=0x01F90678
GetUpdateRect=0x01F93728  GetUpdateRgn=0x01F93748  ValidateRect=0x01F9363C
GetMessageW=0x01F9448C  PeekMessageW=0x01F93000  DispatchMessageW=0x01F94340
WaitForSingleObject=0x01F8B9F8  Sleep=0x01F8B930
TouchCalibrate=0x01F91640
CreateProcessW=0x01F8F89C  CreateThread=0x01F8F934  ShellExecuteEx=0x01FB4B30
RegOpenKeyExW=0x01FB2DB8  RegQueryValueExW=0x01FB2E84  RegCreateKeyExW=0x01FB2C70  RegCloseKey=0x01FB2BE4
GetFileAttributesW=0x01F8B048  CreateFileW=0x01F8B0B4  CreateEventW=0x01F8B8C4
GetSystemTime=0x01FA0BAC  GetVersionExW=0x01F95438  GetSystemPowerStatusEx=0x01F91ADC
```

### ddi.dll functions (renamed in Ghidra, persisted)

```
0x01A5BF00  ddi_blit_dispatcher_1Hz_hotspot  — 12-param software blit (DrvBitBlt shape)
0x01A5C90C  DrvCopyBits_or_DrvBitBlt_SRCCOPY_wrapper  — rop4=0xCCCC
0x01A5D2F0  DrvEnablePDEV_fills_GDIINFO_no_hw
0x01A5C604  DrvGetModes_fills_DEVMODE_array
0x01A5C43C  DrvEnableDriver_impl  — stashes engine callbacks + copies 9-entry DRVFN table
0x01A53FFC  driver_instance_ctor_vtable_01a51020
0x01A545CC  — VirtualAlloc+VirtualCopy chain; 5 HW mappings (primary FB + VRC4173 regs)
0x01A5D18C  DrvCreateDeviceBitmap-like (iFunc4) — always passes flags=2 (never FB-backed)
0x01A5629C  — only reader of cached_pdev[0x6C] (primary FB VA)
```

### Module extraction tool and outputs

- `tools/extract_xip_modules.py` — extracts all 95 WinCE XIP modules from `docs/nk_decompressed.bin` to `build-host/modules/NN_name.bin` + `NN_name.txt` (section map).
- Each `.bin` is the virtual image aligned at the module's vbase; file offset 0 = VA vbase.
- Key modules:
  - `01_coredll.dll.bin` — 512 KB, vbase `0x01F80000`
  - `03_gwes.exe.bin` / `03_gwes_slot2.bin`
  - `50_coshell.exe.bin` — 132 KB, vbase `0x00010000`
  - `51_welcome.exe.bin` — **5 KB, vbase `0x00010000`**
  - `06_shell.exe.bin` — 44 KB
  - `87_modmonitor.exe.bin` — 272 KB
  - `92_Boot.exe.bin` — 6 KB

### Parse recipe for WinCE 3.0 PE exports

```python
# 1. Find a known export-name string inside .rdata
# 2. Find that string's RVA as a DWORD in the bin — one position is inside
#    AddressOfNames[] (the name-RVA array)
# 3. Scan backwards from that position while entries look like plausible RVAs
#    (into .rdata). Start of array = AddressOfNames
# 4. Scan forwards similarly to find end
# 5. Search for a DWORD equal to AddressOfNames RVA → one hit will be inside
#    IMAGE_EXPORT_DIRECTORY's +0x20 field. Structure starts 0x20 before that.
# 6. Read +0x1C AddressOfFunctions, +0x24 AddressOfNameOrdinals, +0x14 NumberOfFunctions,
#    +0x18 NumberOfNames from that directory.
# 7. For each name: name_rva[i] → string; ordinals[i] → index into functions[];
#    functions[ord] → pfn RVA; VA = vbase + pfn_rva
```

Coredll example values: AddressOfFunctions=`0x5CE98`, AddressOfNames=`0x5EEAC`, AddressOfNameOrdinals=`0x60470`, n_funcs=2053, n_names=1393.

## 7. Supersedes

- Supersedes `HANDOFF_POST_PASS34_HIBERNATE_AND_AB_WINDOW_2026-04-22.md §6` (the "probe `ddi_iFunc10` src/dst" default next-step). That probe would not have found the real issue because ddi.dll is healthy.
- Updates `project_pass34_hibernate_window_stub` and `project_pass32_framebuffer_blit_missing` memory entries — the "missing primary-surface blit" is now understood as "no paint pipeline runs at all; welcome.exe never spawns".

## 8. Pending commit decisions

Neither Pass 35 nor Pass 36 produced a committable functional change — both ran diagnostics only. Pass 34's two uncommitted changes (HIBERNATE in gxemul submodule + 0xAB000000 companion-window stub in `src/machine_be300.c`) are still pending commit decision as of the Pass 34 handoff.

Recommendation: commit Pass 34's changes independently of Pass 37, since they're already functional improvements with citations (VR4131 UM §6.1.3 for HIBERNATE; `hardware.txt:204` for 0xAB000000).
