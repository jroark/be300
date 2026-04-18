# Phase BE: Make TLB Diagnostics ASID-Aware and Reclassify the Next Blocker

Date: 2026-04-14

## Bottom Line

Phase BE implemented the planned TLB-diagnostic tightening in both
`src/wince_boot.c` and the existing `gxemul` MMU trace hooks. The
important result is that the shutdown summary no longer ends in the
generic `page_present_but_faulting` bucket.

After a fresh 60-second WinCE cold-boot run, the current blocker now
classifies as:

```text
[WINCE_EXC_SUMMARY] class=missing_or_stale_page_tables
reason=01fe6550_page_invalid
```

That is a real improvement in specificity. The main blocker is now the
slot-0 callback/control page around `0x01FE6550`, which eventually
ends up with an active TLB entry whose selected `lo0` is invalid
(`0x00000000`) and later with page-table state whose selected PTE is
also zero.

Separately, the `0x0204F000` page now has explicit proof of the
ASID-mismatch sidecar that the plan was targeting:

```text
[VR41_V2P] no-match va=0x0204FE48 ... candidate_reason=vpn2_match_asid_mismatch
[WINCE_TLB] match fault_va va=0x0204FE48 status=VA_MATCH_INACTIVE ...
```

So the current picture is:

- Primary blocker: `0x01FE6550` page production / install becomes invalid.
- Secondary MMU issue: `0x0204F000` faults often have a valid page with the
  wrong ASID still resident in the TLB.

## What Changed

- `src/wince_boot.c`
  - Added an ASID/global-aware TLB lookup helper for diagnostics.
  - Updated `dump_tlb_match_for_va()` so `ACTIVE_MATCH`,
    `ACTIVE_MATCH_INVALID`, and `VA_MATCH_INACTIVE` are distinguished
    explicitly instead of treating any VA-range overlap as “present”.
  - Extended hot-page verdict tracking to the current hot pages:
    `0x01FE6550` and `0x0204FE48`.
  - Stored TLB-match truth in the hot-page verdicts:
    found, active, ASID match, global, valid, entry index/page, and
    matching `hi/lo`.
  - Reworked the shutdown summary classification so:
    - `page_present_but_faulting` only applies when there is an active
      valid TLB match,
    - ASID-inactive candidates are classified as
      `wrong_asid_or_process_context`,
    - active-but-invalid matches are separated from generic page
      presence,
    - the slot-0 and `0x0204F000` watch pages participate in the
      summary.

- `gxemul/src/cpus/memory_mips_v2p.c`
  - Expanded the existing VR41 trace window to cover the
    `0x0204F000` and `0x01FE6000` pages, not just a few exact
    addresses.
  - Added candidate no-match reporting so a watched no-match now says
    whether the best candidate was:
    - `vpn2_match_asid_mismatch`,
    - `active_entry_invalid`,
    - `active_entry_readonly`, or
    - no VPN2 candidate at all.

- `gxemul/src/cpus/cpu_mips.c` and `cpu_mips_coproc.c`
  - Added `0x01FE6000` to the existing hot `EntryHi` trace set so the
    slot-0 callback page shows up in the same exception / TLBWI
    tracing pipeline as the other watched user pages.

## Verification

Build:

```bash
cd build-host
make -j8
```

Run:

```bash
gtimeout 60s ./be300 --nand ../ce/restore_images/All_nand_300.bin \
  > phase_be_stdout.log 2> phase_be_stderr.log
```

Result:

- Exit `124` (`gtimeout`) as expected.
- Build succeeded cleanly.
- Cold boot ran to completion of the timeout window.
- Final summary moved from the old generic bucket to:
  `class=missing_or_stale_page_tables reason=01fe6550_page_invalid`.

Key supporting evidence from `build-host/phase_be_stderr.log`:

- `0x0204FE48` repeatedly shows:
  - valid selected page-table entry (`lo1 != 0`, valid),
  - matching VA-range TLB entry,
  - but inactive because the ASID does not match.
- `0x01FE6550` shows a more serious progression:
  1. valid page-table entry but no TLB match,
  2. later an active TLB entry with `lo0 = 0x00000000` (installed invalid),
  3. later page tables themselves produce `lo0 = lo1 = 0`.

## What Was Learned

- The original `page_present_but_faulting` summary was too broad. It
  hid two distinct behaviors:
  - real page-install invalidation on the slot-0 callback page,
  - ASID mismatch on the `0x0204F000` page.
- The `0x0204F000` faults are real ASID/context evidence now, but they
  are not the most immediate blocker toward progress. The slot-0 page
  invalidation is more direct because it produces an actually invalid
  selected PTE/TLB entry for `0x01FE6550`.

## Next Step

Phase BF should focus on the producer path for the slot-0 callback page
table around `0x01FE6000` / `0x01FE6550`:

1. Trace writes that create and later zero the selected PTE for
   `0x01FE6550` (`l2` values observed this phase include
   `0x80FFC1C8`, `0x80FF7654`, `0x80FE460C`, `0x80FD517C`).
2. Identify the NK function(s) that transition the slot-0 callback page
   from valid to invalid.
3. Treat the `0x0204F000` ASID-mismatch evidence as the secondary audit
   once the slot-0 invalidation path is understood.

Reference artifact:

- `build-host/phase_be_stderr.log`
