# Cold-Boot `All_nand_300.bin` to Touch Calibration

## Summary
Latest evidence shows the current cold-boot path reaches an exact `Initializing.bmp` match, then falls into a repeating exception loop with `EPC=0x8008C5C4` and `BadVA=0xFFFFD888`. The immediate plan is to keep the current pragmatic seeded cold-boot path, fix the reintroduced stack-TLB corruption, and only add a narrow cold-boot MMU helper if the corrected stack mapping still leaves the `0xFFFFD888/0xFFFFD8C0` alias unresolved.

## Implementation Changes
- Keep the existing `--wince-cold-boot` flow, seeded replay regions, and the current “skip entire `0x800A5C78`” OAL intercept in `src/machine_be300.c`. Do not expand this task into full ROM MIPS16 dispatcher emulation.
- Remove the `0x8007B4D4 -> li v0,0x5F` patch in `src/machine_be300.c`. Earlier history already established that this reuses `v0` as CP0 `Index`, corrupts it to `95`, and prevents the wired `0xFFFFD000` kernel-stack TLB entry from being written.
- Replace the current read-only TLB probe at the OAL intercept with a one-shot verification block that confirms a valid match for `0xFFFFD000` after the redirect and logs the actual entry fields once.
- If a post-fix run still shows `BadVA` in the `0xFFFFD888/0xFFFFD8C0` window or no live translation for that alias, install a cold-boot-only helper TLB entry locally in `src/machine_be300.c` using the same narrow pattern already used by replay:
  - VA base `0xFFFFD800`
  - even leaf -> PA `0x00001000`
  - odd leaf -> PA `0x00002000`
  - 4 KB pages, global, valid, dirty
  - written to a late free TLB slot, followed by translation-cache invalidation
- Gate any helper install so it runs once, only in cold boot, and only after the corrected wired-stack entry has been validated or proven insufficient.
- Keep diagnostics minimal after validation: one-shot logs for stack TLB presence and first `0xFFFFD888` fault only. Do not leave the run in a verbose tracing state.

## Interfaces
- No new CLI flags or config structs.
- User-visible behavior change only: existing `--wince-cold-boot --nand ce/restore_images/All_nand_300.bin` should advance to touch calibration or later instead of stalling on `Initializing...`.

## Test Plan
- Build in `build-host` and run a cold-boot diagnostic pass with captured stdout/stderr.
- Accept the MMU fix only if the log no longer shows the repeating `EPC=0x8008C5C4 / BadVA=0xFFFFD888` loop and the cold-boot probe shows a valid translation for `0xFFFFD000`.
- Run a visual validation pass with a clean exit so a screenshot is saved; then run `python3 tools/compare_screenshot.py` on the saved BMP.
- Final acceptance: screenshot is no longer an exact `Initializing.bmp` match, and the observed UI reaches touch calibration or any later WinCE screen.
- Regression-check Linux boot (`vmlinux-pgui-demo`) and the WinCE warm/replay path to confirm the new TLB/helper logic does not fire outside cold boot.

## Assumptions
- Success means touch calibration or later WinCE UI; `Starting...` alone is not sufficient.
- A pragmatic seeded cold-boot path is acceptable for this task; full ROM-dispatcher fidelity is deferred.
- The existing dirty edits in `src/machine_be300.c` are intentional and must be integrated with, not discarded.
