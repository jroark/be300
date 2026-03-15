## Plan File: JTAG ROM Dump Bring-Up Notes

### Summary
Create a new plan document at `/Users/jroark/src/be300-framebuffer/PLAN-JTAG-ROM-DUMP.md` that captures all current JTAG/VR4131 findings, board-orientation guidance, and a concrete ROM-dump execution checklist.

### Goal
- Dump boot ROM reliably via VR4131 debug interface, and prove whether true halt-on-reset ROM differs from post-boot ROM window.

### Confirmed Facts
- VR4131 BGA grid is 18x18 with columns `A B C D E F G H J K L M N P R T U V` (skip `I O Q S`) and rows `1..18`.
- Rev-2.0+ N-Wire/JTAG pin functions (Appendix A):
  - `J18 JTDO`, `K17 JTDI/RMODE#`, `L17 JTCK`, `M17 JTMS`, `N17 JTRST#`, `H17 BKTGIO#`, `P18 HLDAK#/NWIREEN`.
- Chapter 2 pin list is for rev 1.2 or earlier; Appendix A gives rev-2.0+ deltas.
- Top package mark (`D30131F1 / VR4131 / 0125K3003`) does not by itself prove rev 2.0+.

### Existing Hardware Evidence
- From `/Users/jroark/src/be300-framebuffer/hardware_survey/BE300BootROM_v1.txt`:
  - `0x0F000010 = 0x00005002` (stable across passes).
  - `PA 0x1FC00000..0x1FC03FFF` stable CRC across passes.
- Decode note:
  - `RID=5` identifies VR4131.
  - `MJREV/MNREV` values alone are not guaranteed to map cleanly to marketing "rev 1.2 vs 2.0" per manual caveat.

### IC5 Orientation + Mapping Procedure
- Provisional A1 cue: chamfer/triangle corner on IC5 silkscreen.
- Do not trust orientation until continuity proof is done.
- Required continuity proof (2 anchors minimum):
  - `A16 (RST#)` into reset network.
  - `B5/C6 (RTCX1/RTCX2)` into RTC crystal network.
- After A1 confirmation, place JTAG candidates by coordinate and ring out to accessible pads/test points.

### JTAG Bring-Up Procedure
- Hardware:
  - 3.3V probe, GND, Vref, reset, and candidate JTAG lines.
- Sequence:
  - Assert reset/TRST, release into halt-capable state.
  - Attempt TAP ID/read.
  - If TAP responds, proceed ROM dump from `0x1FC00000`.
- Dump targets:
  - First pass: `0x4000` bytes at `0x1FC00000`.
  - Then larger windows if mapping indicates bigger ROM.
- Verification:
  - Two reads per range, compare hash.
  - Compare JTAG dump against post-boot survey dump.

### Success Criteria
- Confirmed A1 orientation with continuity.
- Working TAP communication (ID/read succeeds).
- At least one stable ROM binary dump with repeatable hash.
- Difference/sameness report against `BE300BootROM_v1.txt`.

### Risks / Fallbacks
- If TAP never responds: likely no exposed/debug-routable N-Wire on board or wrong orientation.
- Fallback remains post-boot userspace ROM-window survey plus cold-boot repetition.

### Test Plan
- Document validation:
1. File exists at `/Users/jroark/src/be300-framebuffer/PLAN-JTAG-ROM-DUMP.md`.
2. Contains all seven sections above with exact pin list and evidence references.
3. Includes concrete pass/fail criteria and fallback path.

### Assumptions
- IC5 is VR4131 footprint.
- Current dumps in `hardware_survey` are from the same hardware family under investigation.
- No emulator code changes are part of this plan-file step.
