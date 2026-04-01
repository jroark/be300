# BE-300 Emulator Bug: EntryLo1 Ignored in TLB Lookups

## Summary

The emulator appears to ignore the EntryLo1 register when translating virtual addresses via the TLB. Only EntryLo0 is used for both even and odd pages in a TLB entry pair. This causes any user-space memory access to an odd-numbered virtual page (VA bit 12 = 1) to return incorrect data (zeros or the even page's data).

## Background: MIPS TLB Page Pairs

On MIPS, each TLB entry maps a **pair** of adjacent pages:

- **EntryHi** contains VPN2 (the virtual page number divided by 2) and ASID
- **EntryLo0** maps the **even** page (VA bit 12 = 0)
- **EntryLo1** maps the **odd** page (VA bit 12 = 1)
- **PageMask** determines the page size (0x1800 for 4KB on VR41xx)

When the CPU translates a virtual address:
1. It finds the TLB entry where `VPN2` and `ASID` match
2. It checks VA bit 12:
   - If **0** (even page): use **EntryLo0** for the physical address
   - If **1** (odd page): use **EntryLo1** for the physical address
3. It checks the Valid bit in the selected EntryLo
4. It extracts the PFN and cache attributes to form the physical address

## The Bug

The emulator uses EntryLo0 for **both** even and odd pages, ignoring EntryLo1 entirely. When user-space code accesses an odd virtual page, the TLB returns EntryLo0's PFN (which maps to the even page's physical memory), not EntryLo1's PFN.

In practice, odd pages typically have EntryLo1 populated but EntryLo0 may be zero (the even page hasn't been faulted in yet). This causes odd-page reads to return all zeros.

## Evidence

Tested with Linux 4.2.9 on the emulator vs real BE-300 hardware:

| Test binary | Emulator | Real HW |
|---|---|---|
| Single-page init (text+data fit in one 4KB even page) | Works | Works |
| Multi-page init (assembly, .data on separate page from .text) | **Fails** | Works |
| uClibc C program (GOT on separate page) | Fails* | Fails* |

\* The uClibc failure on both platforms has a separate root cause (D-cache), but the emulator-specific failure of the multi-page assembly init is exclusively this EntryLo1 bug.

### Detailed observations

The kernel's `__update_tlb` debug output shows the TLB entry being written correctly with both EntryLo0 and EntryLo1 populated:

```
TLB_UPD: addr=0049e000 pte0=00000000 pte1=01e80097
```

Here `pte1` (odd page 0x49f000) has a valid PTE, which converts to a non-zero EntryLo1. But user-space reads from 0x49f000 return zeros — consistent with the CPU using EntryLo0 (which is zero, since the even page 0x49e000 hasn't been faulted in yet).

The GOT (Global Offset Table) of ELF binaries frequently lands on an odd page. When the dynamic linker or CRT startup code reads the GOT, it gets zeros, computes a null `$gp`, and crashes:

```
do_page_fault(): sending SIGSEGV to init for invalid read access from 00000000
epc = 00000000 in init[400000+8b000]
```

## How to Reproduce

1. Build a statically-linked MIPS ELF binary with text and data on **separate pages** (default gcc behavior, no `-Wl,-N`)
2. The .data section must land on an **odd** page (VA bit 12 = 1)
3. Run it on the emulator — reads from .data return zeros
4. Same binary works correctly on real VR4131 hardware

A minimal reproducer: any statically-linked C program with `printf("hello\n")` will crash because the C runtime's GOT is on an odd page.

## Suggested Fix

In the TLB lookup / address translation code, when a TLB hit occurs:

```
if (va_bit_12 == 0)
    use EntryLo0  // even page
else
    use EntryLo1  // odd page  ← this path is broken
```

Ensure the `else` branch actually reads from the EntryLo1 register/field of the matching TLB entry, not from EntryLo0.

## VR41xx TLB Specifics

The VR4131 TLB differs from standard R4000 in several ways that may be relevant:

- **PageMask for 4KB**: 0x1800 (not standard 0x0000)
- **EntryLo PFN position**: bit 8 (not standard bit 6) — the PFN field is 2 bits higher
- **Context.BadVPN2**: starts at bit 6 (not standard bit 4)
- **32 TLB entries** total

The even/odd page selection based on VA bit 12 should be the same as standard MIPS, but verify against the VR4131 User's Manual (Section 4, "Translation Lookaside Buffer").
