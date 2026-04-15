# WinCE Cold Boot Session 2026-04-14 Phase BF

## Goal

Diagnose the next blocker to a full WinCE 3.0 cold boot from a fresh emulator run, without relying on older session notes.

## Fresh Repro

Built and ran:

```bash
cd build-host
make -j8
gtimeout 60s ./be300 --nand ../ce/restore_images/All_nand_300.bin \
  > /tmp/phase_bf3_stdout.log 2> /tmp/phase_bf3_stderr.log
```

Result:

- exit `124` from `gtimeout`
- final summary:

```text
[WINCE_SEC0_SUMMARY] class=slot0_callback_page_missing_at_process_switch reason=sec0_switch_missing_dll7f8 ...
[WINCE_EXC_SUMMARY] class=slot0_callback_page_missing_at_process_switch reason=sec0_switch_missing_dll7f8 ...
```

## What The Fresh Run Shows

1. The first bad event is a process `sec0` switch, not a generic TLB failure.

```text
[WINCE_SEC0_TRANSITION] kind=l2_pointer_missing_after_switch
  old_table=0x80668CC0 new_table=0x80FF8000
  old_l2=0x80FFC1C8 new_l2=0x00000000
  pc=0x8008B594 ra=0x8008B52C asid=1
```

At the moment ASID 1 switches into `0x80FF8000`, the new table does not yet have a `dll7f8` pointer for the `0x01FE6000` page.

2. The scheduler switch is consuming a preselected child process section table.

```text
[WINCE_SEC0_SRC] label=scheduler_switch pc=0x8008B594 ... ctx+0c=04000000 src_idx=2 src_slot=80FF8000
```

That means the switch path is not inventing `0x80FF8000`; it is reading a child context that already points at that process table.

3. By the first slot-0 callback fault, the child table has gained an L2 pointer, but the PTE is still invalid.

```text
[WINCE_PAGE_VERDICT] tag=first_exception exc=3 probe=0x01FE6550
  sec[0]=0x80FF8000 l2=0x80FF7654 lo0=0x00000000 lo1=0x00000000 valid=0
```

So the failure sequence is:

- switch to `0x80FF8000` while `dll7f8` is zero
- later `dll7f8` becomes `0x80FF7654`
- but `pte+0x18` is still zero when coredll callback code executes

4. The hot PTE eventually appears, but too late for the first callback dispatch.

```text
[WINCE_TLB_POST] ... fault=0x01FE6550 sec[0]=0x80FF8000
  l2=0x80FF7654 pte_off=0x18 lo0=0x000FF61E
```

Later still, the same mapping is fully established:

```text
[WINCE_SEC0_HOT] ... table=0x80FF8000 l2=0x80FF7654
  lo0=0x40011E1A lo1=0x40011F1A lo0_valid=1 lo1_valid=1
```

5. The new per-table L1/PTE write hooks did not log any post-track writes for `0x80FF8000`.

That strongly suggests the `dll7f8` leaf and/or `pte+0x18` are being prepared before the table is first selected and tracked, rather than being repaired later by a simple write after the switch.

## Diagnosis

The next blocker is the ordering of child process page-table publication for the slot-0 callback page:

- the scheduler/thread switch path at `0x8008B594` / `0x8008B0DC`
- consumes child `sec0` tables such as `0x80FF8000`
- before the `0x01FE6000` leaf and selected PTE are ready for execution

This is more specific than the earlier generic bucket:

- not a raw TLB refill bug
- not primarily an ASID mismatch
- not “page never exists”

The failure is that the child process enters coredll callback code before the slot-0 callback mapping is fully published.

## Next Step

Trace the producer side before the first bad switch:

- identify where `0x80FF8000 + 0x7F8` first becomes `0x80FF7654`
- identify where `0x80FF7654 + 0x18` first becomes non-zero
- tie that back to the NK publish path around `0x800984B4` / `0x800984CC`
- compare that ordering against the consumer switch at `0x8008B594`

The fix target should be whichever emulated MMU/page-table lifecycle rule allows the child process table to be selected before that slot-0 callback mapping is ready.
