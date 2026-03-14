#!/usr/bin/env python3
"""Scan a flat NK binary for MIPS store instructions targeting callback global VAs.

Searches for `lui rX, 0x8067` followed by `sw rY, offset(rX)` where offset
matches one of the known callback globals, and also for direct `sw` with
matching low-16 offsets regardless of `lui` context.

Usage:
    python3 tools/scan_nk_producers.py build-docker/nk_code_dump.bin --base 0x80060000
"""

import argparse
import struct
import sys

# Target callback global VAs and their signed low-16 offsets
TARGETS = {
    0x806794EC: "a3_arg",
    0x806794F0: "v0_callback_ptr",
    0x80679508: "a1_arg",
    0x80679510: "a2_arg",
}

# For lui+sw pattern: the upper half we look for
TARGET_HI = 0x8067

# Compute signed 16-bit offsets for each target
TARGET_OFFSETS = {}
for va, name in TARGETS.items():
    lo16 = va & 0xFFFF
    if lo16 >= 0x8000:
        # lui loads hi+1 when offset is negative
        adj_hi = TARGET_HI + 1
        signed_off = lo16 - 0x10000
    else:
        adj_hi = TARGET_HI
        signed_off = lo16
    TARGET_OFFSETS[lo16] = (va, name, adj_hi)

MIPS_REGS = [
    "$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3",
    "$t0", "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7",
    "$s0", "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
    "$t8", "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra",
]


def decode_insn(word):
    op = (word >> 26) & 0x3F
    rs = (word >> 21) & 0x1F
    rt = (word >> 16) & 0x1F
    imm = word & 0xFFFF
    return op, rs, rt, imm


def scan_lui_sw(data, base_va):
    """Find lui rX, hi16 followed within N insns by sw rY, offset(rX)."""
    results = []
    n_insns = len(data) // 4
    WINDOW = 8  # how many instructions after lui to search

    for i in range(n_insns):
        word = struct.unpack_from("<I", data, i * 4)[0]
        op, rs, rt, imm = decode_insn(word)

        # lui is opcode 0x0F
        if op != 0x0F:
            continue

        lui_reg = rt
        lui_hi = imm

        # Check if this lui loads one of the adjusted hi values we care about
        matching_targets = []
        for lo16, (va, name, adj_hi) in TARGET_OFFSETS.items():
            if lui_hi == adj_hi:
                matching_targets.append((lo16, va, name))

        if not matching_targets:
            continue

        # Search next WINDOW instructions for sw using lui_reg as base
        for j in range(i + 1, min(i + 1 + WINDOW, n_insns)):
            sw_word = struct.unpack_from("<I", data, j * 4)[0]
            sw_op, sw_rs, sw_rt, sw_imm = decode_insn(sw_word)

            # sw is opcode 0x2B
            if sw_op != 0x2B:
                continue
            if sw_rs != lui_reg:
                continue

            for lo16, va, name in matching_targets:
                if sw_imm == lo16:
                    lui_va = base_va + i * 4
                    sw_va = base_va + j * 4
                    results.append({
                        "type": "lui_sw",
                        "lui_va": lui_va,
                        "sw_va": sw_va,
                        "lui_insn": word,
                        "sw_insn": sw_word,
                        "target_va": va,
                        "target_name": name,
                        "base_reg": MIPS_REGS[lui_reg],
                        "store_reg": MIPS_REGS[sw_rt],
                        "distance": j - i,
                    })

    return results


def scan_direct_sw(data, base_va):
    """Find any sw with a matching low-16 offset, regardless of lui context."""
    results = []
    n_insns = len(data) // 4
    target_lo16s = {lo16: (va, name) for lo16, (va, name, _) in TARGET_OFFSETS.items()}

    for i in range(n_insns):
        word = struct.unpack_from("<I", data, i * 4)[0]
        op, rs, rt, imm = decode_insn(word)

        if op != 0x2B:  # sw
            continue

        if imm in target_lo16s:
            va, name = target_lo16s[imm]
            insn_va = base_va + i * 4
            results.append({
                "type": "direct_sw",
                "sw_va": insn_va,
                "sw_insn": word,
                "target_va": va,
                "target_name": name,
                "base_reg": MIPS_REGS[rs],
                "store_reg": MIPS_REGS[rt],
                "offset": imm,
            })

    return results


def main():
    parser = argparse.ArgumentParser(description="Scan NK binary for callback global producers")
    parser.add_argument("binary", help="Path to flat NK binary dump")
    parser.add_argument("--base", type=lambda x: int(x, 0), default=0x80060000,
                        help="Base VA of the binary (default: 0x80060000)")
    args = parser.parse_args()

    with open(args.binary, "rb") as f:
        data = f.read()

    print(f"Loaded {len(data)} bytes, base VA=0x{args.base:08X}")
    print(f"Searching for stores to: {', '.join(f'0x{va:08X} ({name})' for va, name in TARGETS.items())}")
    print()

    # lui+sw pattern matches
    lui_results = scan_lui_sw(data, args.base)
    if lui_results:
        print(f"=== LUI+SW pattern matches ({len(lui_results)}) ===")
        for r in lui_results:
            print(f"[MATCH] lui @ VA=0x{r['lui_va']:08X} (0x{r['lui_insn']:08X})"
                  f"  sw @ VA=0x{r['sw_va']:08X} (0x{r['sw_insn']:08X})"
                  f"  -> target 0x{r['target_va']:08X} ({r['target_name']})"
                  f"  base={r['base_reg']} src={r['store_reg']} dist={r['distance']}")
    else:
        print("=== No LUI+SW pattern matches ===")
    print()

    # Direct sw offset matches
    direct_results = scan_direct_sw(data, args.base)
    if direct_results:
        print(f"=== Direct SW offset matches ({len(direct_results)}) ===")
        for r in direct_results:
            print(f"[MATCH] sw @ VA=0x{r['sw_va']:08X} (0x{r['sw_insn']:08X})"
                  f"  -> target 0x{r['target_va']:08X} ({r['target_name']})"
                  f"  base={r['base_reg']} src={r['store_reg']}"
                  f"  offset=0x{r['offset']:04X}")
    else:
        print("=== No direct SW offset matches ===")
    print()

    # Also scan for sh (opcode 0x29) targeting same offsets — halfword stores
    sh_results = []
    n_insns = len(data) // 4
    target_lo16s = {lo16: (va, name) for lo16, (va, name, _) in TARGET_OFFSETS.items()}
    for i in range(n_insns):
        word = struct.unpack_from("<I", data, i * 4)[0]
        op = (word >> 26) & 0x3F
        if op != 0x29:  # sh
            continue
        rs = (word >> 21) & 0x1F
        rt = (word >> 16) & 0x1F
        imm = word & 0xFFFF
        if imm in target_lo16s:
            va, name = target_lo16s[imm]
            insn_va = args.base + i * 4
            sh_results.append({
                "sw_va": insn_va,
                "sw_insn": word,
                "target_va": va,
                "target_name": name,
                "base_reg": MIPS_REGS[rs],
                "store_reg": MIPS_REGS[rt],
                "offset": imm,
            })

    if sh_results:
        print(f"=== SH (halfword store) offset matches ({len(sh_results)}) ===")
        for r in sh_results:
            print(f"[MATCH] sh @ VA=0x{r['sw_va']:08X} (0x{r['sw_insn']:08X})"
                  f"  -> target 0x{r['target_va']:08X} ({r['target_name']})"
                  f"  base={r['base_reg']} src={r['store_reg']}"
                  f"  offset=0x{r['offset']:04X}")
        print()

    # Summary
    total = len(lui_results) + len(direct_results) + len(sh_results)
    print(f"Total candidate producers: {total}")

    # Deduplicate: show unique sw VAs from lui+sw that also appear in direct_sw
    lui_sw_vas = {r["sw_va"] for r in lui_results}
    direct_only = [r for r in direct_results if r["sw_va"] not in lui_sw_vas]
    if direct_only:
        print(f"Direct-only (no preceding lui): {len(direct_only)}")
        for r in direct_only:
            print(f"  VA=0x{r['sw_va']:08X} base={r['base_reg']} -> 0x{r['target_va']:08X} ({r['target_name']})")


if __name__ == "__main__":
    main()
