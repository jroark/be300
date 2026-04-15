# WinCE Cold Boot Run - 2026-04-15

## Command

From `build-host/`:

```bash
make -j4
gtimeout 60s ./be300 --nand ../ce/restore_images/All_nand_300.bin \
  > cold_stdout.log 2> cold_stderr.log
```

Result: `EXIT:124`

## Fresh Artifacts

- `build-host/cold_stdout.log`
- `build-host/cold_stderr.log`
- No screenshot was produced:
  - `[UI] No valid frame — cannot save screenshot`

## Stdout Tail

The fresh run still reaches the NK boot path and PPSH timeout loop:

- `Kloader exit with 80060004.`
- `Windows CE Kernel for MIPS Built on Apr 11 2001 at 15:23:09`
- `InitDebugEther`
- `InitializeJit`
- repeated `PPFS:Time Outs`

## Stderr Summary

The current tree still stops with the same late-kernel process-switch signature:

- `[PPSH_SUMMARY] class=poll_loop_with_boot_progress ... last_exit=0x80078600`
- `[WINCE_TYPE4_SUMMARY] order=incomplete ...`
- `[WINCE_SEC0_SUMMARY] class=slot0_callback_page_missing_at_process_switch reason=sec0_switch_missing_dll7f8 ... pc=0x8008B594 ra=0x8008B52C asid=1`
- `[WINCE_EXC_SUMMARY] class=slot0_callback_page_missing_at_process_switch reason=sec0_switch_missing_dll7f8 ...`
- `[BE300] Emulation stopped after 341480976 instructions`

Additional fresh trace points visible in `cold_stderr.log`:

- the first logged TLB exception remains a TLBL at `pc=0x8008DC30`, `epc=0x800A6A58`, `badvaddr=0x02043000`
- a later TLBS appears at `pc=0x8008C5C4`, `epc=0x8008DC2C`, `badvaddr=0x03A04730`

## Interpretation

This run confirms that the current MIPS16 audit fixes and ROM BEV-vector patching do not regress the active WinCE cold-boot path. They also do not yet move the emulator past the existing slot-0 callback page miss during the process-switch path, so the next root-cause target remains the kernel-side TLB/process-switch failure rather than early ROM/SPL/MIPS16 decode behavior.
