# Archived Investigation Notes

This directory holds session-scoped investigation notes from the WinCE cold-boot reverse-engineering work. They are preserved as a historical trail but are not current reference material — several reach conclusions that were later superseded (e.g., warm-boot `resume_ctx` seeding paths that the project explicitly forbids).

For **current** reference see:
- `../LEGITIMATE_FIXES_NOT_APPLIED.md` — upstream GXemul 0.7.0 bugs left unpatched, with warning signs that would justify re-applying
- `../HARDWARE_GROUND_TRUTH.md` — synthesized hardware-behavior notes
- `../HARDWARE_SURVEY_SYNTHESIS.md` — register survey synthesis
- `../ROM_SPL_HANDOFF.md` — ROM → SPL → NK handoff analysis
- `../GHIDRA_BE300_BOOT_ROM_SETUP.md` — how to load the ROM in Ghidra

## Contents

- `WINCE_COLD_BOOT_SESSION_2026-04-08.md` … `SESSION_2026-04-14_PHASE_B[B-F].md` — per-session cold-boot investigation dumps from early April 2026
- `WINCE_COLD_BOOT_RUN_2026-04-15.md` — single-run log
- `NK_MM_PHASE_AD/AF/AH/AI/AJ/X.md`, `NK_MM_GROUND_TRUTH_2026-04-13.md`, `NK_MM_INVESTIGATION_CORRECTIONS.md` — WinCE NK memory-management investigation phases
- `STATUS_AND_NEXT_STEPS.md` — stale status pointer from before the 2026-04-17 minimization
- `DEBUG_FLOW.md` — older debug-flow notes

Consult these only when looking at history for a specific prior investigation. Do not cite them as current guidance.
