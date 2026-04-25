# Current Status

Last updated: 2026-04-25

## Active Target

The primary target is accurate cold boot of Windows CE 3.0 from:

```bash
./be300 --nand ../ce/restore_images/All_nand_300.bin
```

The emulator should run the real masked ROM, load the SPL from NAND, decompress
NK.exe, start WinCE, and reach the first-boot UI without guest patches, RAM
seeds, or synthetic handoff state.

Secondary implemented paths, used for targeted investigation only:

- `--restore --cf` for the NANDWRITER/CompactFlash recovery path
- `--ppsh` for the WinCE debug-shell probe path

## Current Behavior

- Cold boot reaches the WinCE shell / first-boot UI path.
- User-mode display rendering is known to reach the VRC4173 framebuffer mapping.
- Touch calibration is the active validation area. The PIU model now follows
  the VRC4173 two-page coordinate buffer model, uses hardware-captured timing
  defaults from `docs/hardware/hw_dump_vrc4173.txt`, and scans continuously
  during a held touch.

## Current Investigation Focus

If touch calibration still needs multiple clicks or long presses per target,
focus on PIU hardware behavior and the WinCE touch driver handshake:

- PIUINTREG pending/mask/W1C semantics
- page 0/page 1 buffer validity and ordering
- PADSTATE transitions in WaitPenTouch, PenDataScan, and IntervalNextScan
- data-lost behavior only when both page buffers remain valid

Do not revisit older Welcome.exe, launcher dependency, VirtualCopy, or
framebuffer-alias hypotheses unless a new trace contradicts the current PIU
evidence. Those investigations are archived under `docs/archive/`.

## Authoritative References

- `docs/hardware/Vr4131-um_200203.pdf`
- `docs/hardware/U14579EJ2V0UM00.pdf`
- `docs/hardware/hardware.txt`
- `docs/hardware/hw_dump_vr4131.txt`
- `docs/hardware/hw_dump_vrc4173.txt`
- `docs/HARDWARE_GROUND_TRUTH.md`
- `docs/HARDWARE_SURVEY_SYNTHESIS.md`
- `docs/ROM_SPL_HANDOFF.md`
- `docs/LEGITIMATE_FIXES_NOT_APPLIED.md`
