# Handoff — Pass 43 launcher dependency probe

**Date:** 2026-04-24  
**Branch:** `investigate/pass38-gwes` local only  
**Run:** `BE300_LIFECYCLE_PROBE=1 gtimeout 120s build-host/be300 --nand ce/restore_images/All_nand_300.bin`  
**Logs:** `build-host/pass43_launcher_dep_stdout.log`, `build-host/pass43_launcher_dep_stderr.log`  
**Exit:** `124` from `gtimeout`, expected.

## Summary

Pass 43 implemented the Pass 38-42 probe plan: write watches on the five `HKLM\init` launcher `started` fields and dependency-failure logs at `launcher_dependencies_satisfied_for_init_entry` false-return PCs `0x80080638/0x8008063c`.

The result refutes the strict "welcome.exe never spawns" hypothesis. `Welcome.exe` does spawn in a 120 s run after the warm reset path completes:

- stdout: `SetupWizard :Welcome.exe (NULL) started.`
- stderr: `[BE300_LIFECYCLE_CREATEPROCESS] hit=16 ... image="Welcome.exe"`

The first-boot dependency blocker is real, but temporary: coshell is blocked on Boot (`dep_id=0x003b`) until Boot's post-reset invocation signals ready.

## Evidence

- Launcher table base remains `0x0203b4d0`, stride `0x250`, `started` offset `+0x04`.
- First pass:
  - `shell.exe`, `device.exe`, `gwes.exe`, `Boot.exe` are launched by the kernel launcher.
  - coshell entry blocks: `current_image="coshell.exe" dep_id=0x003b found_image="Boot.exe" found_started=0`.
- Warm-reset pass:
  - `Boot.exe` signals ready: `launcher_module_ready_notify hit=7 a0=0x0000003b`.
  - Boot's `started` slot is set: `launcher_entry3_started ... data=01000000`.
  - `coshell.exe` spawns at CreateProcess hit 15.
  - coshell signals ready: `launcher_module_ready_notify hit=8 a0=0x0000003c`.
  - `Welcome.exe` spawns at CreateProcess hit 16 with caller RA in coredll, not the kernel launcher path.

This means `Welcome.exe` is not an `HKLM\init\Launch<N>` entry parked forever behind `Depend<N>`. It is spawned later from user mode after coshell is up.

## Probe Changes

- `src/be300_probe.c` now has temporary diagnostic watches for:
  - `launcher_entry0_started` through `launcher_entry4_started`
  - `launcher_dep_check_return_zero_jr`
  - `launcher_dep_check_return_zero_delay`
- The dependency-failure helper decodes current entry, blocking dependency entry, IDs, `started` values, images, and launcher table base/count.
- These changes are investigation-only and should not be merged to `main`.
- Ghidra comments at `0x800805A8` and `0x8008690C` were updated with the Pass 43 interpretation.

## Next Target

The next blocker is no longer "why does welcome never spawn?" It is now the post-Welcome display/calibration path:

- The run was headless (`SDL_Init failed: The video driver did not add any displays`), so no final screenshot was saved.
- Probe summary now shows user-mode primary-surface activity: `ddi_mapped_user_va reads=557 writes=2367`, unlike earlier runs where no user-mode pixels reached the primary mapping.
- Next pass should run with a visible SDL display or a framebuffer dump focused after `Welcome.exe` hit 16, then compare against the real-hardware Align Screen stage.
- If visual output is still wrong, instrument the Welcome/coshell paint path rather than revisiting the `HKLM\init` dependency theory.
