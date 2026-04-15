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

## Post-Decoder Rerun

After the decoder-closure pass that:

- kept the gas-verified RRIA extended-immediate mapping
- tightened extended I8 branches to use only base-word `imm[4:0]`
- expanded the MIPS16 regression suite to cover RRIA width/sign edges and poisoned I8 branch bits

the same command was rerun from `build-host/` on 2026-04-15.

Result: `EXIT:124`

The late-kernel stop signature remained unchanged:

- `[PPSH_SUMMARY] class=poll_loop_with_boot_progress ... last_exit=0x80078600`
- `[WINCE_SEC0_SUMMARY] class=slot0_callback_page_missing_at_process_switch ... pc=0x8008B594 ra=0x8008B52C asid=1`
- `[WINCE_EXC_SUMMARY] class=slot0_callback_page_missing_at_process_switch ...`

The instruction count changed slightly to:

- `[BE300] Emulation stopped after 341502892 instructions`

This keeps the same kernel-side process-switch fault as the current baseline and confirms that the decoder-closure pass did not introduce a new WinCE regression.

## Early BEV Investigation

To test whether the emulator still depended on ROM BEV vector patching, the
boot ROM was rerun unmodified with the patch removed and with a single-shot
early-BEV tracer armed:

```bash
make -j4
BE300_STOP_ON_FIRST_EARLY_BEV=1 gtimeout 20s ./be300 --nand ../ce/restore_images/All_nand_300.bin \
  > early_bev_check.stdout 2> early_bev_check.stderr
gtimeout 60s ./be300 --nand ../ce/restore_images/All_nand_300.bin \
  > cold_stdout.log 2> cold_stderr.log
```

Results:

- no early BEV vector entry was logged at `0xBFC00200`, `0xBFC00280`, or
  `0xBFC00380`
- the unpatched ROM still reached SPL entry at `PC=0xA0F027A4`
- the unpatched ROM still reached NK handoff at `PA_24FC=0xA0060004`,
  `PC=0x80F027E8`
- the 60-second run kept the same late-kernel failure signature:
  - `[PPSH_SUMMARY] ... last_exit=0x80078600`
  - `[WINCE_SEC0_SUMMARY] class=slot0_callback_page_missing_at_process_switch ... pc=0x8008B594 ra=0x8008B52C asid=1`
  - `[WINCE_EXC_SUMMARY] class=slot0_callback_page_missing_at_process_switch ...`
  - `[BE300] Emulation stopped after 341507994 instructions`

Interpretation:

- the ROM BEV patch is not required on the current tree for the successful
  ROM -> SPL -> NK cold-boot path
- the earlier assumption that the boot needed synthetic BEV handlers no longer
  reproduces after the MIPS16/TLB cleanup work
- because the original `0xBFC00380` region overlaps normal ROM continuation,
  patching it was riskier than it looked; the project now relies on the
  captured ROM image verbatim and uses early-BEV logging only for diagnosis
