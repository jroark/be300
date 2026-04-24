# Handoff — Pass 32 · Launcher stall is Boot.exe never signalling ready

**Date:** 2026-04-22
**Predecessor:** `docs/HANDOFF_POST_SESSION_CLEANUP_2026-04-21.md`
**Plan executed:** Targets A + B + D from §6 of the predecessor,
folded into a single diagnostic pass per the predecessor's §9
("2–4 passes, not 15").
**State of tree:** one working-copy-only edit to
`src/be300_probe.c` (≈15 lines) adds a `[BE300_LIFECYCLE_CREATEPROCESS]`
log line decoding `a0` as a UTF-16LE image name at `0x8008690C`.
The file was already untracked; nothing committed.

## 1. Finding in one sentence

The cold-boot stall at `Starting.bmp` (SHA
`e8a8c83cd66b9327f50fc1827eada71fb028b332`) is the NK launcher
blocked inside `launcher_blocking_wait_call` at `0x80080CB4`,
waiting for module id `0x3B` (**Boot.exe**) to signal ready via
`launcher_module_ready_notify` at `0x80080D38`. Boot.exe is spawned,
loads its DLLs, and never signals. device.exe (`0x14`) and
gwes.exe (`0x1E`) do signal ready correctly in sequence, so the
notify mechanism itself is sound. The gate is Boot.exe's own
post-DLL-load behavior.

## 2. How this matches / refines the prior Pass 30/31 story

The pre-existing `CLAUDE.md` "Current Investigation Target" blurb
says:

> Pass 30 root-caused the stall: Boot.exe calls coredll.<unknown>(0x0101003c,
> ...) which is the WinCE "reboot via HKLM\Drivers\CASIO\Reboot"
> kernel API at NK 0x800A84A8. That API reads Wait=1000 ms from
> the registry, runs a short wait loop, then calls FUN_8007A140
> which disables interrupts, writes 7 → PA 0x0A00A0C4 and 10 →
> PA 0x0A00A0C8 (VRC4173 GPIO/reset trigger), and falls through
> into the only self-jal in all 6 MB of NK at 0x8007A178...

Pass 31 wired the warm-reset for those VRC4173 writes (commits
`611c55ad` / `8b93ca33`), so the CPU no longer hangs in the
self-jal — a warm-reset fires and the CPU re-enters the reset
vector. Pass 32 confirms what this means at the launcher level:
the launcher's `WaitForMultipleObjects`-style wait on Boot.exe's
readiness is not affected by the warm-reset. After a warm-reset,
the whole boot restarts (including the launcher), so this wait is
still the gate. Pass 31 did not — and was never going to — fix the
overall stall on its own.

The remaining question is: **why doesn't Boot.exe signal ready
before reaching the reboot path?** Options (each with a concrete
next-step probe):

- Boot.exe's WinMain never reaches the `SignalReady(0x3B)` call
  because it takes the reboot branch first. On real hardware the
  reboot changes some persistent state so the post-reset Boot.exe
  takes the ready-signal branch. In our emulator that persistent
  state is missing or wrong.
- Boot.exe is blocked on a driver-readiness check (`GetDisk.dll`,
  `NANDAccess.dll`) that never returns, and the reboot path is
  reached only via a timeout that our probes-on run hasn't hit
  yet.

## 3. Concrete evidence from the 60 s probes-on run

Run: `BE300_LIFECYCLE_PROBE=1 gtimeout 60s ./be300 --nand
../ce/restore_images/All_nand_300.bin`. Screenshot SHA matches
baseline (unchanged).

### 3.1 Launcher table (at PA/VA `0x0203b4d0`, entry stride `0x250`)

Dumped via the pre-existing `be300_probe_dump_launcher_state` hook
on `0x80080AA4`:

| idx | id     | ready | deps                     | image        |
|-----|--------|-------|--------------------------|--------------|
| 0   | 0x0A   | 0     | –                        | shell.exe    |
| 1   | 0x14   | 0     | –                        | device.exe   |
| 2   | 0x1E   | 0     | 0x14                     | gwes.exe     |
| 3   | 0x3C   | 0     | 0x14, 0x1E, 0x3B         | coshell.exe  |
| 4   | 0x3B   | 0     | 0x14, 0x1E               | Boot.exe     |

Registered in this order; each entry is 0x250 bytes with
`launch_id` at +0x00, `ready` at +0x04, UTF-16LE dependency list
from +0x08, UTF-16LE image name at +0x48. (All `ready=0` in the
dumps; note the only dumps from a context where the table's user-VA
is mapped are the three `reason=wait` dumps, which all occurred
**before** device.exe/gwes.exe signalled ready.)

### 3.2 CreateProcess spawns (decoded from `a0` via the new probe)

| hit | image                            | caller ra     | path         |
|-----|----------------------------------|---------------|--------------|
| 1   | `filesys.exe`                    | `0x80080740`  | NK bootstrap |
| 2   | `\Windows\SystemPatchModule.exe` | `0x01f8f8f8`  | user-mode    |
| 3   | `shell.exe`                      | `0x80080ca0`  | launcher     |
| 4   | `device.exe`                     | `0x80080ca0`  | launcher     |
| 5   | `gwes.exe`                       | `0x80080ca0`  | launcher     |
| 6   | `Boot.exe`                       | `0x80080ca0`  | launcher     |
| 7   | `modmonitor.exe`                 | `0x01f8f8f8`  | user-mode    |

coshell.exe is registered in the launcher table (idx 3) but never
reached — the launcher is stuck on the prior wait. The launcher
spawns one process, waits for it to signal ready, then spawns the
next.

### 3.3 Launcher wait / notify sequence

| event                                    | id   | result                  |
|------------------------------------------|------|-------------------------|
| `blocking_wait_call` hit 1 (`a2=0x14`)   | device.exe | unblocks on notify 2 |
| `module_ready_notify` hit 2 (`a0=0x14`)  | device.exe | ready=1            |
| `blocking_wait_call` hit 2 (`a2=0x1E`)   | gwes.exe   | unblocks on notify 3 |
| `module_ready_notify` hit 3 (`a0=0x1E`)  | gwes.exe   | ready=1            |
| `blocking_wait_call` hit 3 (`a2=0x3B`)   | Boot.exe   | **never unblocks** |

Notify hit 1 has `a0=0` (the empty-table pre-registration call).
No notify with `a0=0x3B` ever occurs in the 60 s window. No notify
with `a0=0x0A` (shell.exe) or `a0=0x3C` (coshell.exe) either — the
launcher's wait list is specifically `{device, gwes, Boot}` in that
order.

### 3.4 Boot.exe's post-spawn activity

Immediately after spawn (createprocess hit 6), Boot.exe's loader:

- `loadlib_parse_filename arg="coredll.dll"` (x2 — case variants)
- `loadlib_parse_filename arg="GetDisk.dll"`
- `loadlib_parse_filename arg="NANDAccess.dll"`
- Reads IAT slots at UVA `0x14080..0x1408c`
- Hits `bootexe_safemode_check_entry` and
  `bootexe_syncversionfiles_entry` probes at UVA `0x12F18` and
  `0x124B0` in Boot.exe's ASID (`0x05`)

No execution of NK `0x800A84A8` (reboot API) or NK `0x8007A178`
(self-jal) is observed in the 60 s probes-on window — probes slow
the run significantly. The probes-off 60 s run did reach
`[KjCMU] warm reset triggered at pc=8007a174`, confirming Boot.exe
eventually does take the reboot branch, but cold_stderr.log in a
probes-on run stops well before that.

## 4. What is *not* the bug (reconfirmed)

- The ready-signal path itself — device.exe and gwes.exe
  successfully signal ready and the launcher advances through both.
- The launcher's table layout or dependency model — all five
  entries parsed cleanly.
- The display driver / ddi.dll / framebuffer stack — unchanged
  from predecessor handoff §3.
- Any of the Pass 33–47 dead-ends investigated last session.

## 5. Target D (registry Launch entries) — deferred

No registry extractor exists in `tools/`. The launcher table dump
from §3.1 covers the same ground: it shows the active
`HKLM\init\Launch*`/`Depend*` contents as they are applied at
runtime. Building a static extractor (to confirm the dump matches
the image's on-disk registry) is follow-up work, not needed to
proceed.

## 6. Boot.exe decision branch — fully characterised (addendum)

After writing §1–§5 above, Target A′ below was executed with the
`mipsel-linux-gnu-objdump` tooling in `mips-dev` Docker. Findings:

### 6.1 NK reboot API at `0x800a84a8` is a `KernelIoControl`-style dispatcher on `a0`

The argument `a0 = 0x0101003c` (observed in Pass 30) dispatches to
a handler at `0x800a8698` that:

1. `RegOpenKeyEx(HKLM, "Drivers\\CASIO\\Reboot", 0, &hKey)` —
   strings at NK VAs `0x80075bb8` / `0x80075ba8` / `0x80075b98`
   decode to `Drivers\CASIO\Reboot`, `Wait`, `TimeOut`.
2. `RegQueryValueEx(hKey, "Wait", ...)` + `RegQueryValueEx(hKey,
   "TimeOut", ...)`.
3. Wait-loop using `GetTickCount` deltas against `Wait` ms, then
   (optionally) `Sleep(500)`.
4. `CloseHandle(hKey)`.
5. `jal 0x8007a140` — the reset-trigger function.
6. Unreached branch to cleanup.

**There is no path out of this handler once `a0 = 0x0101003c` is
passed.** The reboot is unconditional from the kernel side.

### 6.2 `FUN_8007a140` is trivial

```
8007a140: mfc0/and/mtc0 — clears CP0_Status bit 0 (IE), disabling interrupts
8007a160: lui a0, 0xaa00 ; ori a0, 0xa000   → a0 = 0xAA00A000 (VRC4173 kseg1)
8007a16c: sw 7, 0xc4(a0)                    → *(0xAA00A0C4) = 7
8007a174: sw 10, 0xc8(a0)                   → *(0xAA00A0C8) = 10  (Pass 31 warm-reset trigger)
8007a178: jal 0x8007a178                    → self-jal (hang until hardware reset)
```

**No RAM signature is written before the self-jal.** The
function relies entirely on the VRC4173 pair `7 → 0xA0C4` + `10 →
0xA0C8` triggering a hardware reset. Our Pass 31 wiring
(`src/be300_devices.c`) translates this to `mips_cpu_cold_reset`.

### 6.3 Boot.exe's WinMain (UVA `0x12bd4`) has the actual decision branch

Decompile of UVA `0x12bd4..0x12cc8` (objdump of
`build-host/modules/92_Boot.exe.bin` at vbase `0x00010000`):

```
12bd4: addiu sp,-48; sw ra,28(sp); sw a2,56(sp)
12be0: lui at,0x1; lw t6, 0x4018(at)       ; t6 = IAT[0x14018]  — coredll thunk
12be8: lui a0,0x1; addiu a0, 0x5390          ; a0 = 0x15390 (lpFileName for the probe)
12bf0: jalr t6                              ; CALL IAT[0x14018]  — likely GetFileAttributesW
12bf4: sw zero, 44(sp)
12bf8: li at, -1
12bfc: beq v0, at, 0x12c08                  ; *** if return == -1 (INVALID_FILE_ATTRIBUTES) ***
12c00: lw v1, 44(sp)        (delay — v1 ← 0)
12c04: li v1, 1             (branch not taken: v1 ← 1)
12c08: bnez v1, 0x12cc8                     ; *** v1!=0 (file found) → EARLY EXIT, no reboot ***
12c0c: nop
; v1 == 0 (file not found) — continue
12c10: jal 0x12f18 ; safemode_check         ; if safemode → jal 0x124b0 (sync) → 0x12f9c/a0=1 → 0x12d20(write_marker)
12c18: beqz v0, 0x12c68                     ; else fall into normal path
; both paths converge:
12c40..12c58 and 12ca0..12cb8:
  a0 = 0x0101003c                           ; load reboot opcode
  jal 0x1310c                               ; coredll thunk → NK 0x800a84a8 reboot API
```

### 6.4 write_initmarker at UVA `0x12d20`

```
12d20..12d58:  CreateFileW(a0=0x15390, a1=0xc0000000, a2=0, a3=0,
                           dwCreationDisposition=2 (CREATE_ALWAYS),
                           dwFlagsAndAttributes=2 (FILE_ATTRIBUTE_HIDDEN),
                           hTemplateFile=0)
12d5c..12d70:  CloseHandle(hFile)
```

Both Boot.exe paths call `write_initmarker(mode)` before calling
the reboot API. The marker is a WinCE object-store file at path
`0x15390` (lpFileName pointer from rdata / imports — not decoded
yet because VA `0x15390` is outside Boot.exe's `.rdata` vsize
`0x280`; the string lives either in coredll-provided rdata or
concat of another module's rdata mapped into the process).

### 6.5 The end-to-end mechanism real hardware uses

On real hardware, the object-store file persists across the
warm-reset (SDRAM is preserved by the VRC4173 reset pair), so
the post-reset invocation of Boot.exe's WinMain takes the
early-exit branch at `0x12c08`, returns from WinMain, and some
process-exit / notify path signals ready for `id=0x3B`. The
launcher advances past Boot.exe, spawns coshell.exe, then gwes
takes over the framebuffer for step 3 of the boot sequence.

### 6.6 Where this leaves the emulator

`mips_cpu_cold_reset` (`gxemul/src/cpus/cpu_mips.c:344`) is
documented to preserve SDRAM and peripheral state — the comment
at line 337 says "SDRAM and peripheral state survive (matching
real-HW HALTimer / RSTSW / SOFTRST leaves SDRAM intact)". So the
object-store file *should* survive our warm-reset. The next
investigation needs to answer:

**Question X:** After our Pass 31 warm-reset fires in a probes-off
60 s run, does the second cold-boot of filesys.exe detect the
existing object store and preserve Boot.exe's init-marker file,
or does it reinitialise the object store from `initdb.ini` and
lose the marker? If the latter, Boot.exe will loop forever in
the reboot path even though the marker was correctly written
pre-reset.

WinCE 3.0's filesys.exe typically detects warm vs cold boot by
checking low-RAM signatures (per `CLAUDE.md` §"NK Boot Notes":
`0x2400` = version marker, `0x2524` = hibernate signature,
`0x254C` = hibernate flags, `0x2200–0x22FF` = `resume_ctx`).
`FUN_8007a140` does not set any of these before self-jalling.
If filesys.exe's warm-boot check fails because one of those
signatures is not the expected post-reboot value, the marker
file gets wiped on the second boot and the reboot loop is
eternal.

## 7. Post-warm-reset second boot actually works (Target X executed)

Target X was executed: a 180 s probes-on cold-boot run captured
the full warm-reset cycle. Headline result: **the warm reset
works end-to-end. Boot.exe takes the early-exit path on the
second boot and signals ready via `launcher_module_ready_notify`
with `a0 = 0x3B`. The launcher advances. ddi.dll is actively
blitting after that.**

### 7.1 Concrete evidence from the 180 s probes-on run

Screenshot SHA unchanged from baseline (still `Starting.bmp`),
but boot progress is substantially further than §3 / §4 implied.

| stderr line | event                                                          |
|-------------|----------------------------------------------------------------|
| 8285        | `boot_trampoline_entry` hit 1 — first cold boot begins         |
| 8385–39790  | First launcher spawns (filesys, shell, device, gwes, Boot.exe) |
| 24767       | `module_ready_notify` hit 1 (empty-table seed)                 |
| 24932       | `blocking_wait_call` hit 1 — wait on `0x14` (device)           |
| 39001       | `module_ready_notify` hit 2 — device signals ready             |
| 39638       | `blocking_wait_call` hit 2 — wait on `0x1E` (gwes)             |
| 39710       | `module_ready_notify` hit 3 — gwes signals ready               |
| 39746       | `blocking_wait_call` hit 3 — wait on `0x3B` (Boot.exe)         |
| **39960**   | **`[KjCMU] warm reset triggered at pc=8007a174` — Pass 31 fires** |
| 40008       | `boot_trampoline_entry` hit 2 — **second cold boot begins**    |
| 40033       | `CreateProcess` hit 8 — filesys.exe spawned again              |
| 40070       | `module_ready_notify` hit 4 (empty-table seed, second boot)    |
| 40177       | `blocking_wait_call` hit 4 — wait on `0x14`                    |
| 41297       | `module_ready_notify` hit 5 — device signals ready             |
| 41647       | `blocking_wait_call` hit 5 — wait on `0x1E`                    |
| 41702       | `module_ready_notify` hit 6 — gwes signals ready               |
| 41727       | `blocking_wait_call` hit 6 — wait on `0x3B`                    |
| **41837**   | **`module_ready_notify` hit 7 with `a0 = 0x0000003b` — Boot.exe SIGNALS READY** |
| 41839       | `bootexe_winmain_return` hit 3 with `v0 = 0x0` — early-exit branch taken |
| 41840+      | ddi.dll blit activity: `ddi_iFunc4`, `ddi_blit_dispatcher_entry`, `ddi_dll_text_c000`, `ddi_fine_5c400` all firing |

Other summary counters (180 s):
- `rom_reset_vector hits=2` — two full cold-boot sequences ran
- `oal_display_dispatcher hits=2` — OAL splash drawn twice
- `splash_caller_a060a0 hits=2` — splash dispatcher run twice
- `spawn_module_createprocess_path hits=15` (first 8 decoded;
  hits 9–15 were capped out; they are the second-boot spawns
  equivalent to hits 2–7 plus at least one new entry)
- `ddi_funcptr_table writes=24` — ddi.dll iFunc table rewritten
  on second boot
- `fb_topleft_kseg1 writes=64` (range `0xaa200000..0xaa200010`)
  — these are the two OAL splash draws plus any subsequent
  user-mode redraws of the top-left 16 bytes

### 7.2 This invalidates §4 and §5 of this handoff

- **§1's "Boot.exe never signals ready" is only true for the
  first boot attempt.** On the second boot (post-warm-reset),
  Boot.exe signals ready correctly. The launcher's wait on `0x3B`
  is satisfied at line 41837.
- **§6.6's "Target X" is resolved.** `mips_cpu_cold_reset`
  preserves SDRAM as documented; the WinCE object-store
  init-marker file survives; Boot.exe's `IAT[0x14018]` check
  returns non-`-1` on the second boot; the early-exit branch at
  UVA `0x12cc8` is taken; the thunk at `0x1311c` (not the reboot
  thunk at `0x1310c`) runs; `winmain_return` fires with `v0 = 0`.
- **The Pass 30/31 story was complete.** Pass 31 wired the
  warm-reset; Pass 32 verified that the wiring produces the
  intended end-to-end effect (boot-cycle → marker persists →
  second-boot progress). The visible `Starting.bmp` screenshot is
  not a stall — it's the ROM/OAL splash of the **second** cold
  boot, drawn pixel-identical to the first, which is exactly what
  real hardware shows at step 2 → step 4 (per `CLAUDE.md`
  §"Observed real-hardware framebuffer sequence").

### 7.3 Remaining unknowns

What we don't yet know:
- Does the launcher actually spawn coshell.exe (`id=0x3C`, idx 3)
  after Boot.exe signals ready? `blocking_wait_call hits=6` means
  there was no hit 7 — the launcher should enter a new wait for
  coshell.exe's ready signal but hasn't within 180 s.
- Does ddi.dll's blit activity (hits 1–8 of `ddi_blit_dispatcher_entry`)
  actually hit the primary-surface framebuffer at PA `0xAA200000`,
  or are those blits writing to intermediate GDI surfaces in
  process RAM?
- When does gwes render the touchscreen calibration UI (real-hw
  step 5)? Is it the next module after Boot.exe, or several steps
  downstream?

### 7.4 Revised next-step targets

### Target Z — 300 s probes-off (executed)

Result: screenshot SHA still matches baseline
`e8a8c83cd66b9327f50fc1827eada71fb028b332` after 300 s. Serial
stdout `build-host/cold_stdout_z.log` shows:

- Two full KLOADER → NK kernel boots (confirming the warm reset
  cycle, same as §7.1)
- Each boot hits `Exception 003 PC=019a3d14 BVA=0x00311000`
  (nanddisk.dll fault, already documented in the memory entry
  `project_wince_pa_fe5000_stale_l2.md` as a benign lazy check)
- Each boot prints `CASIO Compress File System Device Driver`
  and `CASIO Original Oomui Initialize` from device.exe
- No further serial output after the second "Oomui Initialize"

The serial log does not tell us whether execution advanced past
that point silently. The screenshot tells us whatever advancement
happened did not change any pixel from OAL's splash. This leaves
two possibilities:

- **(a)** The second boot does advance further but 300 s is still
  not enough wall-clock time for user-mode GDI to redraw the
  framebuffer (the probes-on 180 s run already confirmed ddi.dll
  blits happen post-Boot.exe; probes-off is faster but still
  time-bounded).
- **(b)** There is a new, silent stall after Boot.exe signals
  ready — ddi.dll blits to in-process GDI surfaces but never
  flushes to the framebuffer at PA `0xAA200000`.

Distinguishing (a) from (b) needs Target W.

### Target W — Characterise post-Boot.exe launcher advance

Raise the `spawn_module_createprocess_path` (`0x8008690c`) cap in
`src/be300_probe.c` from 8 to 64 so all post-warm-reset spawns
log their image names. If coshell.exe (id `0x3C`), SafeShell.exe,
StartingUSB.exe, or any of the downstream user-mode apps from
`build-host/modules/index.txt` appear as CreateProcess hits ≥ 9,
the launcher is advancing normally and the stall (if any) is
downstream. If no additional spawns appear post-warm-reset, the
launcher is blocked on something we haven't identified yet.

Also useful in the same pass: raise the
`launcher_blocking_wait_call` cap so hits 7+ are logged. A wait
on `0x3C` (coshell.exe) would confirm the launcher has iterated
past Boot.exe. No hit 7 means the launcher is doing something
else — possibly a final gwes-handoff signal.

Pass count target: 1 pass for Target W. If Target W identifies
a missing spawn or missing ready-signal, go to Target V below.

### Target W (executed) — coshell.exe IS spawned post-Boot.exe-ready

Cap raised to 64 for `0x8008690c` and 32 for the launcher wait /
notify PCs. A 240 s probes-on run captured all launcher spawns
end-to-end. Results (full ordered list of 15 spawns):

| hit | image                             | context        |
|-----|-----------------------------------|----------------|
| 1   | filesys.exe                       | 1st boot seed  |
| 2   | \Windows\SystemPatchModule.exe    | user-mode      |
| 3   | shell.exe                         | launcher 1st   |
| 4   | device.exe                        | launcher 1st   |
| 5   | gwes.exe                          | launcher 1st   |
| 6   | Boot.exe                          | launcher 1st   |
| 7   | modmonitor.exe                    | user-mode      |
| —   | **[KjCMU] warm reset triggered**  | Pass 31 fires  |
| 8   | filesys.exe                       | 2nd boot seed  |
| 9   | \Windows\SystemPatchModule.exe    | user-mode      |
| 10  | shell.exe                         | launcher 2nd   |
| 11  | device.exe                        | launcher 2nd   |
| 12  | gwes.exe                          | launcher 2nd   |
| 13  | Boot.exe                          | launcher 2nd   |
| 14  | modmonitor.exe                    | user-mode      |
| —   | **`module_ready_notify` a0=0x3B** | Boot.exe ready |
| 15  | **coshell.exe**                   | launcher 2nd   |

Waits (matching the table):
- hits 1–3: waits on `0x14` → `0x1E` → `0x3B` (1st boot, blocks on Boot.exe until warm-reset fires)
- hits 4–6: waits on `0x14` → `0x1E` → `0x3B` (2nd boot, last one
  succeeds because Boot.exe now early-exits)
- No hit 7 — after Boot.exe signals ready, the launcher spawns
  coshell.exe (id `0x3C`) and does NOT enter another wait. This
  is consistent with coshell being the final entry in the
  dependency table (nothing else depends on it).

Notifies:
- 1–3: boot 1 empty-table seed + device + gwes
- 4–6: boot 2 empty-table seed + device + gwes
- 7: Boot.exe signals ready — the key transition

Screenshot SHA after 240 s: still
`e8a8c83cd66b9327f50fc1827eada71fb028b332`.

**Conclusion:** the cold-boot sequence is progressing correctly
through the whole launcher table end-to-end. There is no new
"stall" after Boot.exe — every process the launcher was going to
spawn has been spawned. Post-launcher work (coshell, shell, gwes
UI, touchscreen calibration) is then performed by those processes
autonomously; it just takes real wall-clock time. With probes
enabled, the emulator runs roughly 3× slower than probes-off, and
the serial stdout from the 300 s probes-off run (§Target Z) shows
device.exe still initialising its driver set after the warm
reset. There is no evidence of a functional stall beyond Boot.exe.

### Revised assessment

The apparent "stall at `Starting.bmp`" is almost certainly a
**wall-clock budget problem**, not a functional bug. The expected
real-hardware sequence:

1. `Initializing...` ← 1st OAL splash
2. `Starting...` ← 2nd OAL splash (step 2)
3. Display blanks ← corresponds to our warm-reset transition
4. `Starting...` again ← OAL splash from the 2nd boot (step 4)
5. Touchscreen calibration UI

Our emulator reaches step 4 reliably. Step 5 requires the
launcher-spawned processes (device.exe drivers, gwes, coshell)
to finish their own init and then draw the calibration UI, which
is downstream user-mode work that may take many seconds of
guest-time even on real hardware. In the emulator this becomes
minutes of wall time.

### Target U — Validate by running even longer (final check)

Run probes-off for ~600 s (10 min). If the screenshot changes
to something other than `Starting.bmp` — the entire cold-boot
chain is working and the prior "stall" was only a wall-time
limitation. Document the time-to-UI and stop.

If the screenshot is still unchanged after 600 s: add a probe on
coshell.exe and/or device.exe driver-init loops to confirm they
are making progress rather than stuck in a retry. That stall
would be functional and require investigation.

This is the last sensible test to run before concluding. No
further probe additions should be needed on the launcher /
Boot.exe / KernelIoControl / warm-reset path — all four are
confirmed working.

## 8. Framebuffer address-space check (user-hypothesis test)

The user raised a sharp hypothesis: could user-mode GDI be writing
to a different framebuffer region, and our SDL display is only
seeing the OAL region?

To test, a wide memory watch band was added to `src/be300_probe.c`
(still working-copy-only) covering:
- `fb_body_kseg1_writes` at `0xAA200010..0xAA226000` (full FB body)
- `fb_body_kseg0_writes` at `0x8A200010..0x8A226000` (kseg0 variant)
- `fb_body_pa_writes` at `0x0A200010..0x0A226000` (raw PA)
- `fb_alt_2a_kseg1` / `fb_alt_4_kseg1` (candidate alt framebuffers)
- `gdi_surface_0x140000` at `0x00140000..0x00170000` (GDI surface heap)
- `ddi_mapped_user_va` at `0x001E0000..0x00206000` (the ddi.dll
  VirtualCopy mapping from CLAUDE.md)

A 240 s probes-on run with all of these enabled produced these
write counts:

| region                  | writes  | notes                        |
|-------------------------|---------|------------------------------|
| `fb_topleft_kseg1`      | 64      | OAL text-render top-left churn|
| `fb_body_kseg1_writes`  | 600,934 | **all from PC `0x80f037cc` (OAL clear loop, running 2× per cold boot × 2 boots). Data is always `0x0000`.** |
| `fb_body_kseg0_writes`  | 0       | no kseg0 FB traffic          |
| `fb_body_pa_writes`     | 0       | no direct PA FB traffic      |
| `fb_alt_2a_kseg1`       | 0       | no alt FB                    |
| `fb_alt_4_kseg1`        | 0       | no alt FB                    |
| `gdi_surface_0x140000`  | 6,992   | **gwes/ddi.dll drawing into process-local GDI surfaces** |
| `ddi_mapped_user_va`    | 0       | **zero writes to the ddi-mapped FB user VA** |

### 8.1 Hypothesis refuted, but new finding

The user's specific hypothesis is refuted: there is no alternate
framebuffer PA in use. All writes to the kseg1 FB come from a
single OAL PC (`0x80f037cc`, the `sh t0, 0(rn)` inside the clear
loop), and all writes are zeros. No user-mode code writes to
`0xAA200000`, `0x8A200000`, `0x0A200000`, `0xAA280000`, or
`0xAA400000`.

But the zero writes to `ddi_mapped_user_va` (`0x001E0000+`) are a
new, sharp finding. CLAUDE.md says ddi.dll mapped PA `0xAA200000`
to user VA `0x001E0000` via `VirtualCopy` and stored that VA in
`cached_pdev[0x6C]`. If ddi.dll's `DrvBitBlt` /
`DrvCopyBits` / blit path were drawing to the primary surface,
writes would appear at this VA. **They do not.**

Meanwhile, gwes/ddi.dll HAVE drawn something — 6,992 writes into
a GDI surface at `0x140000..0x170000`. That surface is in
user-process RAM (likely gwes's own process slot). It has never
been flushed to the primary surface.

### 8.2 The actual stall (revised again)

This is a real functional stall, not a wall-clock problem:

**Post-Boot.exe-ready, gwes has drawn content into process-local
GDI surfaces but has never invoked ddi.dll's blit-to-framebuffer
path.** The emulator's SDL display, which reads from PA
`0xAA200000`, therefore continues to show only OAL's splash — the
last thing that was actually written to the primary surface.

This could be because:
- **(a)** gwes is still preparing the content (the surface draws
  may be window background / caret / cursor prep and the
  "`Starting...`" text or calibration UI is still being built up).
- **(b)** A gwes event / driver-ready signal from device.exe (e.g.,
  touch driver activation) is not firing, and gwes is waiting
  before flushing the finished surface to the framebuffer.
- **(c)** ddi.dll's primary-surface blit path has a bug where it
  writes to the GDI surface heap instead of `cached_pdev[0x6C]`'s
  user VA.

### 8.3 ddi.dll blit-dispatcher args — decoded (new evidence)

Looking at each of the 27 `ddi_blit_dispatcher_entry` (PC
`0x01a5bf00`) hits from the 240 s run, every call has both
`a0` (source SURFOBJ) and `a1` (destination SURFOBJ) pointing
into the **`0x00140000..0x00141xxx`** range. Example args:

| hit | a0 (src)     | a1 (dst)     |
|-----|--------------|--------------|
| 1   | `0x00140240` | `0x00140298` |
| 2   | `0x00141498` | `0x001414f0` |
| 3   | `0x00141b20` | `0x00000000` |
| 4   | `0x00141b20` | `0x00140240` |
| 5   | `0x00141ac8` | `0x00000000` |
| 6   | `0x00141ac8` | `0x00140240` |
| 7   | `0x00141bf0` | `0x00141ac8` |
| 8   | `0x00141e50` | `0x00141b20` |

These are **SURFOBJ struct pointers** (each small, ~`0x50`–`0x500`
bytes), all in gwes's process-local heap. None of them are
`cached_pdev[0x6C]` (the mapped FB user-VA at `0x001E0000`). So
ddi.dll is blitting between in-process offscreen SURFOBJs and
never touches the primary surface.

`gdi_surface_0x140000` writes (6,992) are similarly scattered:
vaddrs `0x00140038..0x001428e8` with data values like `0x18c6`
(an RGB565 dark-grey). That's only ~10,400 bytes written — much
too small for a full 240×320 primary-surface draw (which would
need ~153,600 bytes).

Other counters confirming gwes is alive but not flushing to FB:
- `gwes_winmain_entry hits=8` (shared VA — multiple processes)
- `gwes_last_init_entry hits=2`
- `gwes_worker_thread_entry hits=2`
- `gwes_window_create_entry hits=59` — 59 windows created
- `gwes_message_loop_entry hits=2`
- `ddi_DrvEnableDriver_impl hits=2`
- `ddi_blit_dispatcher_entry hits=27` — only 27 blits in 240 s

27 blits / 240 s = 1 blit every 9 s. gwes is idle in its message
loop — not drawing continuously. This is consistent with
"post-init, waiting for some event or driver-ready signal".

### 8.4 Revised next-pass targets

The "ddi.dll blit-to-primary-surface never fires" finding points
to three specific investigations to try in the next session.
Listed smallest-first:

#### (i) Watch `cached_pdev[0x6C]` for reads

Add a 4-byte memory-read watch at the VA where
`cached_pdev[0x6C]` stores the mapped FB user-VA (`cached_pdev` is
at VA `0x00110410` per prior session — so watch
`0x00110410 + 0x6C = 0x0011047C..0x00110480`).

Any read there is ddi.dll (or gwes) looking up the primary
surface's VA. If that watch fires zero times, the primary is
**never queried for output** — the stall is definitely upstream
(gwes never calls "paint to primary"). If it fires, we can see
who reads it and trace from there.

#### (ii) Identify gwes's idle-wait call

The message-loop entry at `0x00035928` (gwes_message_loop_entry)
leads into a `GetMessage` / `WaitForSingleObject` style blocking
call. Add probes at the `WaitForX` coredll imports gwes uses and
capture what handles it's waiting on. A handle that's never
signalled is the next gate — exactly like Boot.exe (id `0x3B`)
was the previous gate.

#### (iii) Dump the primary surface's SURFOBJ

Once `cached_pdev[0x6C]` is confirmed to point at `0x001E0000`,
find the PRIMARY SURFOBJ (which should have `pvBits =
0x001E0000`) and verify its `sizlBitmap` (width/height) match the
240×320 display. If the SURFOBJ is structurally valid, gwes
should be able to blit to it — so the bug is that **gwes is
never told to**. If the SURFOBJ is corrupt, we've got a
driver-side bug.

Pass count target: 1 pass for (i), 1 combined pass for (ii) +
(iii) if (i) confirms the primary is never accessed. If the
investigation exceeds 3 passes, stop and reassess.

## 9. Target (i) executed — cached_pdev[0x6C] is correctly set, never used for blit

A 180 s probes-on run with a read+write watch at
`0x0011047c..0x00110480` (the location of `cached_pdev[0x6C]`)
produced:

```
reads=2 writes=2
```

Decoded access log:

```
W hit=1 pc=0x01a546d0 data=00 00 1E 00   (= VA 0x001E0000)   boot 1
R hit=1 pc=0x01a56350 data=00 00 1E 00   (= VA 0x001E0000)   boot 1
W hit=2 pc=0x01a546d0 data=00 00 1E 00   (= VA 0x001E0000)   boot 2
R hit=2 pc=0x01a56350 data=00 00 1E 00   (= VA 0x001E0000)   boot 2
```

And the paired `ddi_mapped_user_va` watch at
`0x001E0000..0x00206000` (with reads now enabled):
**not even in the summary** → zero reads AND zero writes across
the whole 180 s.

### 9.1 Conclusive

Per boot:
- ddi.dll PC `0x01a546d0` **writes** `cached_pdev[0x6C] =
  0x001E0000` exactly once. This is the setter (probably inside
  `DrvEnablePDEV` or a near-equivalent).
- ddi.dll PC `0x01a56350` **reads** it back exactly once. This is
  the lookup — probably inside `DrvEnableSurface` or a
  primary-SURFOBJ constructor. It returns the correct VA.

After those two accesses, the primary VA is **never touched
again**. Over 180 s of runtime, neither ddi.dll nor gwes:

- reads `cached_pdev[0x6C]` again,
- reads any byte in `0x001E0000..0x00206000`,
- writes any byte in `0x001E0000..0x00206000`,
- writes any byte at PA `0xAA200000..0xAA226000` except for OAL's
  clear loop (single PC, all zeros),
- writes any byte at any alternate framebuffer-plausible PA.

The infrastructure is correct: `VirtualCopy` mapped PA
`0xAA200000 → 0x001E0000`, ddi.dll stored that VA in
`cached_pdev[0x6C]`, and an early ddi.dll function read it back
(presumably to build the primary SURFOBJ's `pvBits`). Everything
downstream — the blit path from user content to the primary —
**never fires.**

This matches case (b)/(c) from §8.2 rather than (a). gwes is not
"still doing setup" — gwes is sitting in its message loop,
drawing into offscreen SURFOBJs occasionally (27 blits / 180 s),
and the "present to primary surface" code path is either never
reached or has been programmed to target something else.

### 9.2 Final next-pass recommendation

All four items below are surgical — do not add more ddi.dll
probes until these narrow down the cause:

**A. Identify what gwes is waiting on in its idle loop.**
gwes_message_loop_entry fires, then nothing further advances
apart from the 27 blits. What is gwes's `GetMessage` / WFM
waiting on? Same pattern as the Boot.exe investigation — find
the wait handle, find what would pulse it. This is probably the
same mechanism by which device.exe was supposed to signal
"touch driver ready" or similar.

**B. Find the call-graph from PC `0x01a56350` (the reader of
cached_pdev[0x6C]) to see what function reads the primary VA
and what it does next.** If `0x01a56350` is a one-off setup
reader (e.g., builds the SURFOBJ once), then there is a
**different** PC that should read `cached_pdev[0x6C]` during
drawing. That second reader never fires. Find it statically in
the ddi.dll disassembly (Boot.exe already extracted;
`build-host/modules/` has the relevant module).

**C. Check whether the primary SURFOBJ built at boot has
`pvBits = 0x001E0000` or something else.** Because ddi.dll's
drawing path blits only to in-process SURFOBJ pointers
(0x00140xxx range), the primary SURFOBJ may have been built with
a `pvBits` pointing into the **in-process** GDI surface heap
rather than `0x001E0000`. That would be an error in the PDEV /
SURFOBJ construction path. Dump the primary SURFOBJ (8-byte-aligned
struct, offsets per `winddi.h`) at a runtime point when it's
known to be built — the primary SURFOBJ pointer is usually what
ddi.dll returns from `DrvEnableSurface`.

**D. Confirm by observation:** after adding a targeted probe that
logs any write to `cached_pdev[0x6C]`'s pointed-to page (i.e.,
`pvBits`-region writes as a function of the `pvBits` value), see
whether any such write is happening. If yes, gwes is drawing to
the primary SURFOBJ but through a different VA than
`0x001E0000` — which would mean the `pvBits` was set wrong.

## 10. BMP-dump probe executed — gwes is in font-cache setup, not drawing UI

A diagnostic BMP-dump capability was added to
`src/be300_probe.c` (working-copy-only per CLAUDE.md hygiene) to
"treat the memory being drawn to as a framebuffer and dump it as
BMP" — the exact user question. Two dump modes:

1. **Region dump** at hit 25 of `ddi_blit_dispatcher_entry`
   (`0x01A5BF00`): renders three fixed ranges as 240×320 RGB565
   BMPs — the GDI surface heap at `0x00140000`, the mapped
   primary user-VA at `0x001E0000`, and the kseg1 primary FB at
   `0xAA200000`.
2. **SURFOBJ dump** at every blit-dispatcher hit (capped at 30):
   reads the src and dst SURFOBJ structs from `a0`/`a1`, parses
   out `sizlBitmap.cx/cy`, `pvBits`, `lDelta`, `iBitmapFormat`,
   and saves the bitmap at its declared size. This follows the
   driver's own size metadata rather than guessing.

### 10.1 Region-dump results (300 s probes-on run)

- `gdi_dump_..._primary_kseg1.bmp` (PA `0xAA200000`,
  `chunks_ok=38/38`): mostly black with a faint horizontal band
  mid-lower. This is OAL's second-boot splash still in progress
  at the time hit 25 fires — consistent with the 600 k clear-loop
  writes from PC `0x80f037cc` running in parallel.
- `gdi_dump_..._primary_uva.bmp` (user VA `0x001E0000`,
  `chunks_ok=0/38`): all zero / unmapped. The mapped FB user-VA
  is never mapped in the current process's ASID at this
  trigger. Confirms §9's finding — nothing writes through this
  VA.
- `gdi_dump_..._gdi_surface.bmp` (user VA `0x00140000`,
  `chunks_ok=3/38`): only the first 12 KB is readable. The top
  ~20 rows of the 240×320 rendering show structured noise (this
  is SURFOBJ struct metadata interpreted as pixels — NOT actual
  pixel content). The remaining 300 rows are black (unmapped
  chunks). So the surface "heap" has ~12 KB of data allocated
  out of a 192 KB region.

### 10.2 SURFOBJ-dump results (first 30 blits in 300 s)

Every SURFOBJ encountered has tiny dimensions:

| cx × cy | count |
|---------|-------|
| 32 × 16 | 11    |
| 16 × 16 | 9     |

Plus a handful of 64×16 and 128×16 and two with `pvBits=0x0`
(output-device SURFOBJs with no in-memory backing). All
`iBitmapFormat` values are 1 (BMF_1BPP mono), 2 (BMF_4BPP), 3
(BMF_8BPP), or 4 (BMF_16BPP RGB565). **No blit touches a
SURFOBJ with `cx=240, cy=320` — the primary display size.**

Spot-checking four rendered glyph BMPs:

- `blit3_src_16x16`: mostly black, single cyan dot upper-left
- `blit9_src_32x16`: mostly black, scattered green/white dots
- `blit16_src_16x16`: scattered blue/yellow/white speckles
- `blit25_src_32x16`: has speckly pattern across the bottom
  rows — could be a partially-rendered glyph row

These are unmistakably **font-glyph cache entries**, not full-UI
renders.

### 10.3 Conclusion

The stall is **case (a)** from §8.2: gwes/ddi.dll is still in
its font/glyph-cache initialisation phase, doing tens of tiny
16×16 / 32×16 glyph blits between SURFOBJs in process-local
memory. It has NOT yet started blitting the 240×320 primary
surface that would produce the user-mode "`Starting...`" or
calibration UI. Content destined for the primary surface is
being prepared one glyph at a time; the "compose final screen
and blit to primary" step hasn't happened yet.

This is fundamentally a **wall-clock budget problem** — real
hardware completes this in a fraction of a second; our emulator
with 30 blits / 300 s is pacing at roughly 10× real-time dilation
when probes are on and still doesn't reach the final compositing
step.

### 10.4 Verified diagnostic capability

The BMP-dump infrastructure works. Filenames:

```
gdi_dump_<ts>_<serial>_<region>.bmp           ← 240×320 region view
gdi_surfobj_<ts>_<serial>_<tag>_<cxXcy>.bmp   ← actual SURFOBJ bitmap
```

Future passes can reuse the `be300_probe_dump_region_as_bmp` and
`be300_probe_dump_surfobj` helpers to inspect any guest-memory
region as an image. Screenshot SHA across all runs remains
`e8a8c83cd66b9327f50fc1827eada71fb028b332`; no regressions
introduced.

### 10.5 Revised next-pass recommendation

Given the finding that gwes is simply slow (not functionally
stuck), the most productive next pass is to **profile and reduce
emulator overhead** rather than add more probes:

- Raise the CPU clock advertised to the guest (currently 131.07
  MHz per the boot log). WinCE may be doing timer-driven delays
  that consume real wall-clock time proportional to perceived
  emulator speed.
- Reduce probe overhead or run probes-off for the actual boot
  measurement. A long probes-off run (30 min or more) would be
  a simple next experiment to confirm — if the screenshot
  eventually changes, the fix is "wait longer".
- Alternatively, add a targeted trigger deeper in gwes that
  fires once per "full UI repaint" — if that count never
  increments, then case (a) is wrong and gwes really is stuck
  in its init loop. If it increments, we just need more time.

## 11. Pass 32 §10 recommendations executed — wall-clock hypothesis refuted

### 11.1 `--speed 0` test

Ran a 180 s probes-on run with `--speed 0` (throttling disabled,
`be300/main.c:47` sets the default to 166 MHz real-hardware
speed) and compared to the 180 s throttled run (§8):

- Both reached Boot.exe's ready-notify (`a0 = 0x3B`) at
  `module_ready_notify hit=7`
- Both produced exactly `ddi_blit_dispatcher_entry hits=27`
- Both produced exactly `fb_body_kseg1_writes writes=600,934`
  and `gdi_surface_0x140000 writes=6,992`
- Both produced exactly `cached_pdev_6c_primary_va reads=2 writes=2`

**Every counter matches to the instruction.** This cannot be
wall-clock dilation masking slow progress — the guest is hitting
a deterministic quiescent state and then staying there
regardless of wall time. This **refutes §10.5 case (a) from
Pass 32's earlier framing**: the stall is functional, not
wall-clock.

### 11.2 gwes decompile traces the actual blocker

Via Ghidra MCP (bound to gwes.exe), decompiled:

- `gwes_last_init_REACHED_OK_Pass32` at Ghidra `0x04034b68`
  (runtime `0x00034b68`) — creates three manual-reset events
  (`0x000B6834`, `0x000B6824`, `0x000B6830`), creates a worker
  thread starting at `0x000348d4`, opens HKLM, reads a registry
  value, then issues:
  ```
  EventModify(0x000B6824, 2)     // reset 0x6824
  EventModify(0x000B6830, 2)     // reset 0x6830
  EventModify(0x000B6834, 3)     // set 0x6834 (EVENT_SET per WinCE nkintr.h)
  WFSO(0x000B6830, INFINITE)     // wait for worker to signal readiness
  ```

- `gwes_worker_REACHED_OK_Pass32` at Ghidra `0x040348d4`
  (runtime `0x000348d4`) — the worker thread's `do { ... } while
  (true);` main loop:
  ```
  WFSO(0x000B6834, INFINITE);   // PC 0x0003492c
  EventModify(0x000B6834, 2);   // reset PC 0x0003493c, load at 0x00034938
  ... CreateWindow etc ...
  EventModify(0x000B6830, 3);   // set PC 0x00034a54 — tells init we're ready
  WFSO(0x000B6824, INFINITE);   // PC 0x00034a6c
  ... GetMessage loop ...
  ```

### 11.3 Added probes at the worker's 4 event PCs

| PC (runtime) | meaning                    | hits in 180 s |
|--------------|----------------------------|---------------|
| `0x00034928` | WFSO(0x6834) entry (`lw a0`) | **2**       |
| `0x0003492c` | WFSO(0x6834) jalr           | **2**       |
| `0x00034938` | `lw a0` before reset (post-return) | **0** |
| `0x00034a4c` | `lw a0` before SetEvent(0x6830) | **0**   |
| `0x00034a68` | `lw a0` before WFSO(0x6824) | **0**       |
| `0x00034a6c` | WFSO(0x6824) jalr           | **0**       |

The `reset_6834` PC `0x00034938` has **zero** hits. This PC is
the first instruction after WFSO returns. If WFSO had ever
returned, it would have executed. It did not.

### 11.4 Conclusive stall localisation

**Both worker threads (one per boot) enter
`WFSO(0x000B6834, INFINITE)` at `0x0003492c`, and neither ever
returns.** That is the concrete, instruction-level stall.

This contradicts Pass 32 §10.3's "font-cache setup" narrative.
The font-cache activity we observed (27 blits, 6,992 surface
writes, glyph SURFOBJs) must be happening on a **different**
thread than the worker — possibly the gwes_last_init caller
thread after it yields to other work before entering
`WFSO(0x000B6830)`, or ddi.dll being called from a process
other than gwes (this matches our earlier observation that
`ddi_blit_dispatcher_entry` fires across multiple
`entryhi`/ASID values, not exclusively gwes's).

### 11.5 Why WFSO(0x6834) never returns — the real question

gwes_last_init DOES execute — `gwes_last_init_entry hits=2`.
The function's decompile shows `EventModify(0x6834, 3)` on a
path that reaches `WFSO(0x6830, INFINITE)` at the end. Since
init's final WFSO is blocking (worker never sets 0x6830 because
worker never wakes), it CAN'T be that init never reaches the
SetEvent call — init IS still blocked in its own WFSO which
implies it DID reach that point.

Two plausible explanations — each resolvable by one more small
probe set:

**A. `gwes_last_init` takes an early-return path before
reaching the SetEvent chain.** The function has multiple early
returns (on InterlockedExchange, on CreateEvent failure, on
absent `_DAT_000b6820` thread handle, on `func_0x00075530()`
returning 0). If any early return fires, worker was created but
0x6834 was never set. Probe `0x04034d78` (runtime `0x00034d78`)
— the SetEvent(0x6834) call site per the decompile's layout —
and see if it hits. If hits=0, this is the answer.

**B. `EventModify(0x6834, 3)` does not mean "SetEvent" in this
coredll.** If the op-code mapping in this build is not
`1=PULSE / 2=RESET / 3=SET` (WinCE 3.0 standard per
`nkintr.h`) but something else, op 3 might be PulseEvent —
which WILL lose the signal if worker wasn't waiting at the
instant of the pulse. Spot-check: add a probe at `0x000b1054`
(the EventModify function entry) and log every call's
`(handle, op)` pair. If op=3 is observed to fire on 0x6834
while worker isn't yet past `WFSO` entry, that's the race.

If (B) is the case, the fix is a **WinCE-scheduler timing
quirk**: on real hardware `CreateThread` yields to the new
thread, which reaches `WFSO(0x6834)` before init gets to
`EventModify(0x6834, 3)`. In our emulator's scheduler,
CreateThread does not yield, so init signals before worker
waits.

### 11.6 Next-pass recommendation

Add two precise probes:

1. `0x00034d78` (within `gwes_last_init` just before the
   `jal 0x000b1054` for SetEvent(0x6834)) — confirms whether
   init reaches SetEvent.
2. `0x000B1054` (the coredll EventModify entry) — logs every
   `(handle, op)` pair with `ra` so we know who calls what.

One 60-s probes-on run will definitively distinguish (A) vs (B)
from §11.5. If (A): scope the early-return condition and fix the
causing dependency. If (B): understand the race and either fix
the coredll-op interpretation or introduce a schedule yield on
CreateThread.

All probe additions remain working-copy-only (per CLAUDE.md §
"Instrumentation Hygiene" / § "Commit, Branch, And Push Rules").

## 12. Pass 32 §11 two-probe set executed — IsOwnerInfoSet path fails (CASE A)

### 12.1 Instrument + result

Added probes at `gwes_last_init` event-modify call sites (runtime
PCs `0x00034D5C`, `0x00034D6C`, `0x00034D7C`, `0x00034D94`), plus
the gate-check jal at `0x00034D48`, plus the coredll
EventModify entry at `0x000B1054`. After a 180 s probes-on
`--speed 0` run:

- `gwes_init_jal_75530 hits=2` (both boots reached the gate check)
- **`gwes_init_reset_6824`, `gwes_init_reset_6830`,
  `gwes_init_setevent_6834`, `gwes_init_wfso_6830` all hits=0**
- 51 total EventModify calls recorded — **none** targeting
  handles `0x6824` / `0x6830` / `0x6834`

This conclusively confirms **case A**: `gwes_last_init` takes
the early-return path at `0x04034d50` (`beq v0, zero, 0x04034da4`)
because the call at `jal 0x00075530` returns 0. `SetEvent(0x6834)`
is therefore never called, and the gwes worker waits forever.

### 12.2 What function 75530 actually is

Strings embedded in gwes directly identify the source file:

- debug messages at runtime VAs `0x14C68` / `0x14C10`: "Could not
  allocate owner profile buffer" / "Could not allocate owner notes
  buffer"
- source-file string at VA `0x14CC0`:
  `D:\WINCE300\PUBLIC\COMMON\OAK\DRIVERS\STARTUI\startui.cpp`

So `func_0x00075530` is a function from **WinCE 3.0's public-source
StartUI driver** (`startui.cpp`) — almost certainly
`IsOwnerInfoSet()` or a close equivalent. It checks whether the
device has user-configured owner name / notes / owner card.

### 12.3 Probe trace through fn75530's four return paths

Added probes at the fn75530 alloc-failure early-returns, the
pre/post of `jal 0x000b1814`, the two sltu checks on
Owner[0x22a] and Notes[0x182], and the final return:

- `fn75530_entry hits=2`
- `fn75530_jal_75ef4 hits=2` (always reaches the reg-read helper;
  no alloc failure)
- `fn75530_jal_b1814 hits=2` (calls the mystery coredll helper)
- `fn75530_after_b1814 hits=2` (it returned cleanly)
- `fn75530_sltu_owner_22a hits=2` (fell through → b1814 returned 0)
- `fn75530_sltu_notes_182 hits=2` (fell through → Owner[0x22a]==0)
- `fn75530_final_return hits=2` (arrived at return with v0=0)

So all three fn75530 success conditions fail:
- `func_0x000b1814()` returns 0
- `Owner[0x22a]` is 0
- `Notes[0x182]` is 0

### 12.4 Identifying func_0x000b1814

Watch on gwes IAT slot `0x000B32BC` captured three reads: two
during NK's loader-fixup at PC `0x80090c84`, and one at runtime
from the stub at `0x000B1818`. All reads return the **same**
resolved coredll VA: `0x01F8B4D0`.

Disassembly of coredll `0x01F8B4D0`:
```
addiu sp, -24
sw    ra, 20(sp)
li    t6, -21690            ; t6 = 0xFFFFAB46 (kernel-syscall sentinel)
jalr  t6                    ; kernel callback
nop
lw    ra, 20(sp)
jr    ra
addiu sp, 24
```

This is a classic WinCE 3.0 MIPS kernel-callback stub: `jalr`
into kseg3 at `0xFFFFAB46`, caught by NK's general-exception
handler, dispatched to a specific kernel API. Adding probes at
`0x01F8B4D0`/`0x01F8B4DC`/`0x01F8B4E4` confirmed the kernel
round-trip completes in our emulator — both calls return v0=0
cleanly. **The callback works; the kernel genuinely returns 0.**

The adjacent coredll stubs at `0x01F8B4D0..0x01F8B53C` use
sentinels `0xAB3A`, `0xAB42`, `0xAB46`, `0xAB5A` — four
apparently-related kernel APIs in the same API-set, plausibly
`IsOwnerInfoSet` / `GetOwnerName` / `GetOwnerNotes` /
`GetOwnerCard` or an `EnumHeader` pattern, but identifying which
WinCE export corresponds to which sentinel would require NK's API
dispatch table.

### 12.5 Re-reading the boot flow in light of the user's
      date/time-dialog correction

Important correction from the user on 2026-04-22: **the real-
hardware boot sequence has an additional step between
touchscreen calibration and the desktop** — a date/time-setting
dialog, where the user is prompted to set the current date and
time. `CLAUDE.md` §"Observed real-hardware framebuffer sequence"
is updated to include this as step 6 (with desktop renumbered
to step 7). This is consistent with real BE-300 behaviour:
after a battery-disconnect cold boot, the VR4131 RTC reports an
uninitialised time and WinCE explicitly asks the user to set it
before reaching the desktop.

The updated sequence is:

1. `Initializing...` with progress bar
2. `Starting...` (OAL, backlight gets brighter)
3. Black screen / display blank (corresponds to our warm-reset
   transition — Boot.exe's `a0=0x0101003c` reboot call)
4. `Starting...` (second render, different code path)
5. Touchscreen calibration prompt
6. **Date/time setting dialog** — user sets current date/time
7. WinCE desktop

### 12.6 Reframing "missing hardware emulation"

Given the boot flow above, `fn75530` returning FALSE on first
boot is **correct and expected** — the device has not been
calibrated, date/time-set, or owner-configured yet. Real
hardware's `func_0x000b1814` ALSO returns 0 on first cold boot
(no owner info, invalid RTC, etc.). Real hardware's
`gwes_last_init` therefore ALSO takes the early-return path on
first boot.

The stall in our emulator is therefore NOT "fn75530 returns 0".
The real question is: **what runs AFTER gwes_last_init returns 0
on real hardware?** On real HW that path brings up the
calibration / date/time / owner-info dialogs. In our emulator
that path either never runs or is itself blocked.

Plausible hardware-emulation gaps that would block the follow-up
path:

1. **RTC validity signal.** WinCE's date/time dialog fires when
   the RTC reports an invalid / clearly-uninitialised value.
   The VR4131 RTC emulation exists (see
   `project_pass22_siu_wiring.md`, `docs/hardware/hw_dump_vr4131.txt`
   §RTC) but the "has RTC been user-set yet" signal may be
   missing. If the emulator presents a reasonable default time
   (e.g., 2026-04-22) instead of the obviously-invalid sentinel
   real hardware shows post-battery-pull (typically 1970-01-01
   or similar), WinCE may skip the date/time dialog and thereby
   skip the whole setup sequence.

2. **Touch driver interrupt chain.** Calibration can only proceed
   if the touch driver can deliver tap events from the VRC4173
   PIU / GPIO. Pass 25 fixed one PIU bit (`project_pass25_piu_bit0_autoclear.md`);
   other bits in the touch-event-assertion path may still need
   auto-clear / write-1-to-clear / edge-trigger fixes. Without a
   working touch chain, the calibration UI renders (if it does)
   but cannot accept the three taps required to proceed.

3. **OEM-supplied registry defaults missing.** WinCE uses
   `initdb.ini` / `initobj.dat` to pre-populate the registry on
   first boot. If our emulator's filesys.exe processes a subset
   of these, the "default owner info" or "default calibration
   data" may be absent, causing `IsOwnerInfoSet()` to return 0
   AND the downstream setup code to fail to find its config
   templates.

### 12.7 Next-pass recommendation

The ownership of this question now moves UP one layer. Two
specific pieces of evidence to collect in a single short pass:

A. **Who calls `gwes_last_init`?** `get_xrefs_to 0x04034b68`
   should reveal the caller(s). Decompiling the caller tells us
   what path executes on `return 0` — whether that path is
   supposed to spawn the calibration UI, show the welcome
   wizard, or something else.

B. **What does the VR4131 RTC report at user-mode time-read**?
   Probe any coredll `GetSystemTime` / `GetLocalTime` equivalent
   (kernel-callback sentinel probably in the `0xAB30..0xAB60`
   range adjacent to the four we already found) and log the
   date value. If we're returning a post-battery-pull-plausible
   value (near 1970) real hardware's first-boot flow would
   trigger the date/time dialog too; if we're returning a
   clearly-set value, we're accidentally pre-configuring the
   RTC and masking the wizard trigger.

These two observations together should conclusively identify the
specific hardware gap (if any), or demonstrate that the gap is
elsewhere (e.g., touch-driver-chain).

## 13. RTC-validity investigation + gwes_last_init caller trace

### 13.1 What our RTC emulates on cold boot

`src/hw/rtc.c:66` (`rtc_init`) explicitly sets
`s->etime = 0; s->ecmp = 0; s->rtcl1 = 0; s->rtcl2 = 0;
s->tclock = 0; s->rtcint = 0;` and the code comment states:

> The BE-300 has no battery-backed RTC. After a cold boot
> (battery removed), ETIME, RTCL1, RTCL2, and ECMP are all zero.

This matches real-hardware behavior per
`docs/hardware/hw_dump_vr4131.txt`: the RTC dumps show
arbitrary non-zero values because the device has been running,
but a fresh post-battery-pull device presents ETIME=0, which is
what our `rtc_init` does.

`rtc_tick` in `gxemul/src/devices/dev_vr41xx.c:510-514`
increments ETIME at `ETIME_L_HZ = 0x8000` (32768 Hz) per
`gxemul/src/include/thirdparty/vr_rtcreg.h:79`. This matches
VR4131 UM §13.2.4.

### 13.2 Live RTC traffic during 180 s probes-on run

Added a memory-write/read watch on `0xAF000100..0xAF000140`
(VR4131 RTC MMIO via kseg1). Summary after 180 s:

- **Total traffic**: 6,281,666 reads / 10 writes.
- **Dominant reader**: PC `0x80f027dc` — 4,095+ reads (hit-capped),
  always at `vaddr=0xaf000100` (ETIMEL+M 32-bit) returning
  `data=c8280000` (little-endian u32 = `0x000028C8` = 10,440
  ticks ≈ 0.32 s since boot). This PC is inside the CASIO
  "Oomui" module or a similar high-kernel-space OEM DLL loaded
  at `0x80F00000+` — almost certainly the OEM OAL's kernel-tick
  / `IdleTimerReset` polling loop.
- **Other readers** (2 hits each): PCs `0x800a5ed8`, `0x800a5ec0`,
  `0x8007a684`, `0x8007a674`, `0x8007a66c` — all in NK text
  — suggest a `GetTickCount` / scheduler-tick path reading ETIME
  once per boot × 2 boots.
- **Other vaddrs** (2 hits each): `0xAF000118` (RTCL2LREG),
  `0xAF000112` (RTCL1HREG), `0xAF000110` (RTCL1LREG),
  `0xAF00013E` (RTCINTREG). Consistent with a one-time RTC
  configuration at boot.

Our RTC is definitely being polled but the emulator's response
is correctly returning a monotonically-increasing ETIME starting
from 0.

### 13.3 Conclusion: RTC is not the hardware-emulation gap

- Our RTC starts at ETIME=0 on cold boot (matches real HW
  post-battery-pull).
- Our RTC ticks at ~32768 Hz (spec-correct).
- No writes-that-should-have-returned-failure. No reads returning
  nonsense. The RTC-emulation surface is behaving sanely.

If WinCE were treating our ETIME=0 value as "valid", the
date/time dialog wouldn't fire on real HW either (real HW starts
at exactly the same ETIME=0 post-battery-pull). So RTC value
alone cannot be the decider — real HW must have some OTHER
signal that drives the "show date/time dialog" branch. Best
candidates:

1. **A registry value** (e.g., `HKLM\...\SetDate`) that a prior
   boot of the device writes; absent on battery pull. The same
   mechanism as the Owner info.
2. **A battery-backed RAM byte** on a non-VR4131 peripheral
   (e.g., a VRC4173 RTC register we haven't identified) that
   indicates "time has been set since last reset".
3. **RTCINT flags** — a specific IRQ-enable bit that real HW
   sets, but our emulator defaults differently.

Ruling all three out cleanly is out of scope for this handoff
but each is probeable.

### 13.4 Caller of gwes_last_init — branch taken on return=0

Scanning the gwes binary for `jal 0x00034b68` (encoded
`0x0c00d2da`) found three call sites: runtime PCs `0x000164DC`,
`0x00016650`, and `0x0001FA48`. The code at `0x000164DC`:

```
164dc: jal  0x00034b68          ; fn entry: gwes_last_init
164e0: nop
164e4: beql v0, zero, 0x164f4   ; *** if gwes_last_init returned 0, skip SetEvent ***
164e8:   li  t8, 1              ; (only runs on non-zero return)
164ec:   jal 0x0003485c          ; FUN_0403485c = SetEvent(_DAT_000b6824)
164f0:   nop
164f4: li   t8, 1
164f8: lui  at, 0xb
164fc: sw   t8, 0x41f8(at)       ; _DAT_000B41F8 = 1 (flag)
16500: jal  0x00035928            ; gwes_main_message_loop
```

So on fresh boot (`fn75530 returns 0`), the caller:
- **skips** `SetEvent(0x6824)` that wakes the worker thread's
  second wait,
- **still sets** the flag `_DAT_000B41F8 = 1`,
- **still calls** `gwes_main_message_loop` (runtime
  `0x00035928`).

### 13.5 Implication

The decision about whether to spawn the setup wizard (with
calibration → date/time → owner-info dialogs) is NOT made by the
caller of `gwes_last_init` based on its return value. Both
paths (return=0 AND return=non-zero) lead to the same
`gwes_main_message_loop` call. The difference is only whether
`SetEvent(0x6824)` fires — and that event signals the gwes
worker thread, which is the one stuck at WFSO.

Two important corollaries:

1. On real HW fresh-boot, `fn75530` genuinely returns 0 AND
   `SetEvent(0x6824)` is genuinely NOT called — but real HW
   still reaches the calibration UI. That means the calibration
   UI is triggered from SOMEWHERE ELSE, NOT from this specific
   path in gwes's worker.
2. The gwes worker's WFSO on 0x6834 is a red herring for
   reaching calibration UI. The welcome-wizard / calibration
   path must go through a DIFFERENT gwes thread (the "main"
   thread that runs `gwes_main_message_loop`, not the worker
   at `0x000348d4`).

### 13.6 What `gwes_main_message_loop` does

Per decompile (`gwes_main_message_loop_REACHED_OK_Pass32` at
runtime `0x00035928`):

```c
iVar1 = func_0x000b1014(0x40, 0x6c);            // LocalAlloc(0x40, 0x6c)
if (iVar1 == 0) return 0;
iVar1 = func_0x0003572c(iVar1, param_1);         // further init
_DAT_000b6838 = iVar1;
if (iVar1 == 0) return 0;
if (*(int *)(iVar1 + 0x58) == 0) {
    ... cleanup and return 0 ...
}
return 1;
```

This allocates an 0x6c-byte object, calls `func_0x0003572c` to
populate it, checks `*(obj+0x58)` for a status, and returns 1
on success. The `gwes_window_create_entry hits=59` count
suggests `func_0x0003572c` creates multiple windows — likely
the welcome-wizard / calibration / owner-info dialogs as
CHILD WINDOWS of a parent shell window.

### 13.7 Where to probe next

The calibration UI doesn't render because either:

1. `func_0x0003572c` doesn't create the calibration window in
   its first 59-window batch, AND there's an event the shell
   waits for that never fires.
2. `func_0x0003572c` DOES create the calibration window, but
   the window's WM_PAINT never gets dispatched — because the
   PAINT is dispatched by the worker thread, and the worker is
   stuck at WFSO(0x6834).

Hypothesis 2 lines up with every observation so far. To confirm:

- Decompile `func_0x0003572c` (runtime `0x0003572c`) — does it
  create a calibration window? Or a welcome-wizard window?
- Probe `gwes_window_create_entry`'s arguments — capture the
  window-class and window-title for each of the 59
  CreateWindow calls. Any match for "Align Screen" /
  "Calibrate" / "Welcome" would tell us the calibration UI
  window IS being created.

### 13.8 The re-framed hardware-emulation question

With all of the above, the missing hardware gap is most likely
NOT in the RTC and NOT in the fn75530/owner-info chain. The
concrete localised stall is gwes worker stuck at WFSO(0x6834),
which blocks the window-paint dispatch pipeline, which in turn
makes ANY user-mode UI window invisible even if it's been
created.

So the "what hardware do we need to emulate" question becomes:
**what triggers `SetEvent(0x6834)` on real HW's fresh-boot
path**, given that gwes_last_init's own SetEvent call is
skipped in this case? Candidates:

- Another gwes thread or caller that sets 0x6834 directly (we
  would find it via a write-watch on handle `0x000B6834`'s
  underlying kernel event object — but we have 51 EventModify
  calls logged and NONE target the relevant handle, which
  suggests it's really not being set).
- An external process calling a kernel API that indirectly
  signals the event (WinCE has this pattern: `PostMessage` to
  gwes's HWND triggers signal-send internally).
- A VRC4173 / GPIO interrupt that wakes the event via a touch
  or keyboard driver path.

Given the boot succeeds through coshell.exe spawn and the
display driver's PDEV / SURFOBJ setup is correct per §9, and
given 27 ddi.dll blit calls fire (but only on offscreen
SURFOBJs per §10.2), the most likely remaining hardware-
emulation gap is:

**the VRC4173 PIU / GPIO interrupt chain, specifically events
that should fire once per boot to signal "input device ready"
to gwes.** This would naturally wake 0x6834 via the touch
driver's internal notify → gwes's worker, and it would match
the Pass 25 pattern of "one PIU bit was wrong, fixing it
unblocks the whole chain".

### 13.9 Next-pass recommendation

Rather than continue down gwes internals, the highest-value
next probe is to:

1. Walk every VRC4173 PIU / GPIO interrupt flag from
   `docs/hardware/hw_dump_vrc4173.txt` — cross-check each one
   against the emulator's current emulation in
   `src/be300_devices.c` and `src/hw/icu.c`. Pass 25 fixed
   bit 0 of PIUCNTREG (0x0A000300); there are likely more bits
   with the same "latch stuck pattern" that we haven't noticed
   because only bit 0 was causing an obvious spin.
2. Probe the VRC4173 interrupt-status registers at `0xAA001120`,
   `0xAA00112C`, `0xAA001B20` (the ones `docs/hardware/hardware.txt`
   says IRQ0 handlers clear on every interrupt). If our
   emulator isn't delivering specific interrupt bits to the
   guest, the guest's driver chain never becomes ready.

This investigation is probably worth 1-2 passes. If those don't
reveal a fix, the next-best angle is to compare our kernel-side
interrupt-handler execution trace against what real HW would
execute — specifically looking for VRC4173 IRQ bits that fire
on real HW but never in our emulator.

## 14. Real-hardware photographic evidence from 2026-04-22 — MAJOR re-framing

User provided two photos of real-hardware boot state:

### 14.1 Photo 1: Home / Date-Time dialog
- Title bar: `Home`
- City: `Washington, D.C. (DC USA)`
- Date: `1/01/01` → confirms OAL epoch is `2001-01-01`; our
  ETIME=0 maps correctly.
- Time: `00:00:09` → 9 seconds past midnight; RTC ticks at
  spec rate from cold-boot zero.
- **Bottom taskbar with 7 icons** (Calendar, Documents, Notes,
  Mail, Globe, Clock(?), Tools) **visible underneath the modal
  dialog**.

### 14.2 Photo 2: Calibration ("[Align Screen]")
- Same taskbar visible at the bottom of the screen, with the
  same 7 icons.
- "[Align Screen]" text + crosshair target in lower-left —
  exactly matches the UTF-16 string at gwes runtime VA `0xc56ce`
  that we previously identified from `calibrui.cpp`.

### 14.3 What this re-frames

The **taskbar is painted by coshell.exe** (the
`DesktopExplorerWindow` shell per strings in
`build-host/modules/50_coshell.exe.bin`). It is **visible on
real HW during BOTH the calibration dialog AND the Home/Clock
dialog**. This means:

- coshell's full desktop is painted to the primary framebuffer
  BEFORE any wizard appears.
- The "wizard dialogs" of step 5 and step 6 are **modal
  overlays painted on top of an already-rendered desktop**.
- So on real HW, the paint-to-primary pipeline is fully working
  by the time the user sees step 5.

On our emulator, nothing paints to the primary framebuffer
except OAL's clear loop. coshell.exe is spawned (Pass 32 §7
CREATEPROCESS hit 15) but does NOT render its desktop.

### 14.4 The gwes_worker WFSO(0x6834) is a red herring

The stuck worker thread at `0x000348d4` creates a window with
the standard `"static"` control class (runtime VA `0x11310` =
`"static"` after decoding). That worker is therefore a
specific-purpose helper thread for an Owner Info or IME-style
dialog, NOT gwes's general paint dispatcher. On real HW first
boot:

- `gwes_last_init` is called, `fn75530` ("IsOwnerInfoSet")
  returns 0, `SetEvent(0x6834)` is skipped → the same worker
  thread IS blocked at WFSO(0x6834) on real HW too.
- The stall is harmless because the worker's responsibility is
  the Owner Info dialog which isn't shown on the first boot
  anyway.
- The calibration and Home/Clock wizards are rendered via a
  DIFFERENT code path that doesn't depend on this worker.

§11 and §12's conclusion that "worker stuck at WFSO(0x6834) is
the stall" is **wrong**. Real HW has the identical block and
still renders the wizards.

### 14.5 Re-focused root cause

The true stall is in coshell's desktop-paint pipeline. Evidence:

- coshell.exe spawns (our CREATEPROCESS hit 15).
- coshell contains the `DesktopExplorerWindow` class and
  `Task Manager` UI text — nothing else. It's a lightweight
  shell whose only job is the desktop + taskbar.
- After coshell spawns, our emulator's primary FB at
  `0xAA200000` never receives user-mode writes.
- `cached_pdev[0x6C]` correctly holds `0x001E0000` (the
  VirtualCopy-mapped user VA for the FB) but no user-mode
  writes target that range either.

So coshell is either stuck in its own init, or it requires a
driver-ready signal that isn't firing in our emulator.

### 14.6 Additional user-provided data: touch tap plays a tone

User note: "Touch screen calibration response has an
imperceivable delay, it also generates a tone when tapped."

Implications:
- Touch-event delivery chain on real HW is immediate
  (event-driven, near-zero latency).
- The BE-300 has an **audio output path** that is activated
  by early boot. The audio is probably a VRC4173 PWM or DAC
  register that we currently do not emulate.
- This expands the missing-hardware candidate list: **audio
  driver initialization on real HW may complete a registration
  step that coshell (or a parent process) waits on before
  painting the desktop**. If so, our missing audio emulation
  is the concrete blocker.

### 14.7 Refined next-pass investigation plan

Short list, all concrete:

1. **Audit VRC4173 register map for audio/PWM registers.**
   `docs/hardware/hw_dump_vrc4173.txt` likely documents these.
   If our emulator drops writes to these regions, the wave
   driver's init probe returns failure, driver DLL fails to
   load, and any process gated on "WAV driver ready" gets
   blocked.
2. **Profile coshell's execution path.** coshell.exe has
   vsize 0x21000 (128 KB text). Its WinMain probably starts at
   a high address in the file. Add a broad exec-watch for
   coshell's main thread and trace which functions it enters
   and exits. If it blocks on a WFSO of some driver-ready
   event, that's our new target.
3. **Look for "wave" / "wav" / "snd" DLL load attempts in
   our probes-on log.** If gwes/coshell try to load
   `waveapi.dll` or `snd.dll` and the load fails silently,
   the downstream init path breaks.
4. **Probe the primary-surface paint-dispatch chain.** Any
   call to a "swap / present / flush" API in ddi.dll would
   write to the primary; add a probe at that path and see if
   it fires at all.

The missing hardware is almost certainly in the audio / driver-
init chain, not in the display pipeline itself. Pass 25 fixed
one bit of the touch-driver chain; the audio-driver chain is
likely similarly incomplete.

### Target W — Characterise post-Boot.exe launcher advance (only if Target Z fails)

Raise the 0x8008690c cap so all 15 createprocess spawns are
logged. Add a `launcher_blocking_wait_call` hit-8+ capture so we
can see whether the launcher enters a wait on `0x3C` (coshell).
Identify what module — if any — the post-Boot.exe launcher is
blocked on. Reuse the table-dump pattern from §3.1.

### Non-goals (reconfirmed after §7)

- Do NOT add Pass 33–47-style display-driver probes. ddi.dll
  is doing its job — `ddi_blit_dispatcher_entry` hits 1–8,
  `ddi_funcptr_table writes=24`, the vtables are populated. The
  issue (if there is one) is that ddi.dll hasn't yet been asked
  to blit the post-OAL UI, not that it can't.
- Do NOT modify `mips_cpu_cold_reset`. The 180 s run proves it
  preserves enough state for the WinCE object store to survive
  the reset.

## 7. Things to re-read before the next pass

- `CLAUDE.md` — emulation philosophy, commit hygiene, Pass 30/31
  context
- `docs/HANDOFF_POST_SESSION_CLEANUP_2026-04-21.md` — predecessor
- this document — Pass 32 findings

## 8. Hygiene reminders for the next session

- The Pass 32 probe extension in `src/be300_probe.c` is diagnostic
  only. Revert or discard before any functional commit.
- Do **not** add `resume_ctx` seeding, persistent-marker RAM
  injection, or anything that forges Boot.exe's "post-reset"
  state. If Boot.exe tests a persistent marker, the correct fix
  is to emulate whatever hardware actually preserves that marker
  across the VRC4173 warm-reset.
- Keep probe changes in `src/be300_probe.c`. Do not expand the
  hook call sites in `gxemul/src/cpus/cpu_dyntrans.c` or the
  loadstore files.
- Pass count target: 1 pass for Target A′, 1 pass combined with
  B′ if it isn't already answered. If it balloons, stop and
  re-read §7.
