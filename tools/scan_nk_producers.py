#!/usr/bin/env python3
"""Scan a flat NK binary for MIPS store instructions targeting callback global VAs,
JAL callers of the producer function at 0x800962B0, and producers of the callback
pointer at 0x806794F0.

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


def read_word(data, offset):
    """Read a 32-bit LE word from data at byte offset, or None if OOB."""
    if offset < 0 or offset + 4 > len(data):
        return None
    return struct.unpack_from("<I", data, offset)[0]


def dump_context(data, base_va, center_va, half_window=8):
    """Dump hex words around center_va. Returns list of (va, word_hex) strings."""
    lines = []
    for off in range(-half_window, half_window + 1):
        va = center_va + off * 4
        file_off = (va - base_va)
        w = read_word(data, file_off)
        if w is not None:
            marker = " <<" if off == 0 else ""
            lines.append(f"  0x{va:08X}: {w:08X}{marker}")
    return lines


def disasm_simple(word, va):
    """Minimal MIPS disassembly for display context."""
    op, rs, rt, imm = decode_insn(word)
    rd = (word >> 11) & 0x1F
    funct = word & 0x3F
    sa = (word >> 6) & 0x1F

    if op == 0:  # R-type
        if funct == 0x09:  # JALR
            return f"jalr    {MIPS_REGS[rd]},{MIPS_REGS[rs]}"
        elif funct == 0x08:  # JR
            return f"jr      {MIPS_REGS[rs]}"
        elif funct == 0x21:  # ADDU
            return f"addu    {MIPS_REGS[rd]},{MIPS_REGS[rs]},{MIPS_REGS[rt]}"
        elif funct == 0x25:  # OR
            return f"or      {MIPS_REGS[rd]},{MIPS_REGS[rs]},{MIPS_REGS[rt]}"
        elif funct == 0x00 and word == 0:
            return "nop"
        elif funct == 0x00:  # SLL
            return f"sll     {MIPS_REGS[rd]},{MIPS_REGS[rt]},{sa}"
        return f"r-type  funct=0x{funct:02X}"
    elif op == 0x03:  # JAL
        target = (word & 0x03FFFFFF) << 2
        target |= (va + 4) & 0xF0000000
        return f"jal     0x{target:08X}"
    elif op == 0x02:  # J
        target = (word & 0x03FFFFFF) << 2
        target |= (va + 4) & 0xF0000000
        return f"j       0x{target:08X}"
    elif op == 0x04:  # BEQ
        offset = imm if imm < 0x8000 else imm - 0x10000
        dest = va + 4 + offset * 4
        return f"beq     {MIPS_REGS[rs]},{MIPS_REGS[rt]},0x{dest:08X}"
    elif op == 0x05:  # BNE
        offset = imm if imm < 0x8000 else imm - 0x10000
        dest = va + 4 + offset * 4
        return f"bne     {MIPS_REGS[rs]},{MIPS_REGS[rt]},0x{dest:08X}"
    elif op == 0x01:  # REGIMM
        if rt == 0x01:  # BGEZ
            offset = imm if imm < 0x8000 else imm - 0x10000
            dest = va + 4 + offset * 4
            return f"bgez    {MIPS_REGS[rs]},0x{dest:08X}"
        elif rt == 0x11:  # BGEZAL
            offset = imm if imm < 0x8000 else imm - 0x10000
            dest = va + 4 + offset * 4
            return f"bgezal  {MIPS_REGS[rs]},0x{dest:08X}"
        elif rt == 0x00:  # BLTZ
            offset = imm if imm < 0x8000 else imm - 0x10000
            dest = va + 4 + offset * 4
            return f"bltz    {MIPS_REGS[rs]},0x{dest:08X}"
    elif op == 0x06:  # BLEZ
        offset = imm if imm < 0x8000 else imm - 0x10000
        dest = va + 4 + offset * 4
        return f"blez    {MIPS_REGS[rs]},0x{dest:08X}"
    elif op == 0x07:  # BGTZ
        offset = imm if imm < 0x8000 else imm - 0x10000
        dest = va + 4 + offset * 4
        return f"bgtz    {MIPS_REGS[rs]},0x{dest:08X}"
    elif op == 0x09:  # ADDIU
        simm = imm if imm < 0x8000 else imm - 0x10000
        return f"addiu   {MIPS_REGS[rt]},{MIPS_REGS[rs]},{simm}"
    elif op == 0x0D:  # ORI
        return f"ori     {MIPS_REGS[rt]},{MIPS_REGS[rs]},0x{imm:04X}"
    elif op == 0x0F:  # LUI
        return f"lui     {MIPS_REGS[rt]},0x{imm:04X}"
    elif op == 0x23:  # LW
        simm = imm if imm < 0x8000 else imm - 0x10000
        return f"lw      {MIPS_REGS[rt]},{simm}({MIPS_REGS[rs]})"
    elif op == 0x2B:  # SW
        simm = imm if imm < 0x8000 else imm - 0x10000
        return f"sw      {MIPS_REGS[rt]},{simm}({MIPS_REGS[rs]})"
    elif op == 0x29:  # SH
        simm = imm if imm < 0x8000 else imm - 0x10000
        return f"sh      {MIPS_REGS[rt]},{simm}({MIPS_REGS[rs]})"
    elif op == 0x28:  # SB
        simm = imm if imm < 0x8000 else imm - 0x10000
        return f"sb      {MIPS_REGS[rt]},{simm}({MIPS_REGS[rs]})"
    elif op == 0x14:  # BEQL
        offset = imm if imm < 0x8000 else imm - 0x10000
        dest = va + 4 + offset * 4
        return f"beql    {MIPS_REGS[rs]},{MIPS_REGS[rt]},0x{dest:08X}"
    elif op == 0x15:  # BNEL
        offset = imm if imm < 0x8000 else imm - 0x10000
        dest = va + 4 + offset * 4
        return f"bnel    {MIPS_REGS[rs]},{MIPS_REGS[rt]},0x{dest:08X}"

    return f"op=0x{op:02X} raw=0x{word:08X}"


def dump_context_disasm(data, base_va, center_va, half_window=8):
    """Dump disassembled context around center_va."""
    lines = []
    for off in range(-half_window, half_window + 1):
        va = center_va + off * 4
        file_off = (va - base_va)
        w = read_word(data, file_off)
        if w is not None:
            marker = " <<" if off == 0 else ""
            asm = disasm_simple(w, va)
            lines.append(f"  0x{va:08X}: {w:08X}  {asm}{marker}")
    return lines


def scan_lui_sw(data, base_va):
    """Find lui rX, hi16 followed within N insns by sw rY, offset(rX)."""
    results = []
    n_insns = len(data) // 4
    WINDOW = 8

    for i in range(n_insns):
        word = struct.unpack_from("<I", data, i * 4)[0]
        op, rs, rt, imm = decode_insn(word)

        if op != 0x0F:
            continue

        lui_reg = rt
        lui_hi = imm

        matching_targets = []
        for lo16, (va, name, adj_hi) in TARGET_OFFSETS.items():
            if lui_hi == adj_hi:
                matching_targets.append((lo16, va, name))

        if not matching_targets:
            continue

        for j in range(i + 1, min(i + 1 + WINDOW, n_insns)):
            sw_word = struct.unpack_from("<I", data, j * 4)[0]
            sw_op, sw_rs, sw_rt, sw_imm = decode_insn(sw_word)

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

        if op != 0x2B:
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


def scan_sh(data, base_va):
    """Scan for sh (halfword store) targeting same offsets."""
    results = []
    n_insns = len(data) // 4
    target_lo16s = {lo16: (va, name) for lo16, (va, name, _) in TARGET_OFFSETS.items()}

    for i in range(n_insns):
        word = struct.unpack_from("<I", data, i * 4)[0]
        op = (word >> 26) & 0x3F
        if op != 0x29:
            continue
        rs = (word >> 21) & 0x1F
        rt = (word >> 16) & 0x1F
        imm = word & 0xFFFF
        if imm in target_lo16s:
            va, name = target_lo16s[imm]
            insn_va = base_va + i * 4
            results.append({
                "sw_va": insn_va,
                "sw_insn": word,
                "target_va": va,
                "target_name": name,
                "base_reg": MIPS_REGS[rs],
                "store_reg": MIPS_REGS[rt],
                "offset": imm,
            })

    return results


def scan_jal_callers(data, base_va, target_va):
    """Find JAL instructions targeting target_va, and JALR patterns loading target_va."""
    results = []
    n_insns = len(data) // 4

    # JAL encoding: opcode 0x03, instr_index = target_va >> 2, 26-bit field
    instr_index = (target_va >> 2) & 0x03FFFFFF
    jal_word = (0x03 << 26) | instr_index

    for i in range(n_insns):
        word = struct.unpack_from("<I", data, i * 4)[0]
        caller_va = base_va + i * 4

        if word == jal_word:
            results.append({
                "type": "jal",
                "caller_va": caller_va,
                "insn": word,
                "target_va": target_va,
            })

        # Also check for JALR patterns: look for R-type funct=0x09
        op = (word >> 26) & 0x3F
        if op == 0 and (word & 0x3F) == 0x09:
            rs = (word >> 21) & 0x1F
            # Check preceding instructions for loading target_va into rs
            # lui rX, hi16 + ori rX, rX, lo16 or addiu rX, rX, lo16
            target_hi = (target_va >> 16) & 0xFFFF
            target_lo = target_va & 0xFFFF
            for k in range(max(0, i - 8), i):
                kw = struct.unpack_from("<I", data, k * 4)[0]
                kop, krs_f, krt, kimm = decode_insn(kw)
                if kop == 0x0F and krt == rs and kimm == target_hi:
                    # Found lui loading the right hi — check for ori/addiu with lo
                    for m in range(k + 1, i):
                        mw = struct.unpack_from("<I", data, m * 4)[0]
                        mop, mrs, mrt, mimm = decode_insn(mw)
                        if mrt == rs and mrs == rs:
                            if (mop == 0x0D and mimm == target_lo) or \
                               (mop == 0x09 and mimm == target_lo):
                                results.append({
                                    "type": "jalr_indirect",
                                    "caller_va": caller_va,
                                    "insn": word,
                                    "target_va": target_va,
                                    "lui_va": base_va + k * 4,
                                    "load_reg": MIPS_REGS[rs],
                                })

    return results


def scan_callback_ptr_producers(data, base_va, target_va=0x806794F0):
    """Extended scan for producers of the callback pointer 0x806794F0.

    Searches for:
    1. lui+sw with adjusted hi/lo (already covered by scan_lui_sw)
    2. lui 0x8067 + ori 0x94F0 (constructing address for indirect store)
    3. lui 0x8068 + addiu -0x6B10 (constructing address via sign-extended offset)
    4. Any addiu/ori producing 0x94F0 in lo16
    5. lui 0x8067 or lui 0x8068 paired with any store at matching offset
    """
    results = []
    n_insns = len(data) // 4

    target_hi = (target_va >> 16) & 0xFFFF  # 0x8067
    target_lo = target_va & 0xFFFF          # 0x94F0
    # Adjusted: since 0x94F0 >= 0x8000, lui loads 0x8068, signed offset = 0x94F0 - 0x10000 = -0x6B10
    adj_hi = target_hi + 1                  # 0x8068
    signed_lo = target_lo - 0x10000         # -0x6B10 = 0xFFFF94F0 -> as uint16: 0x94F0

    WINDOW = 16

    for i in range(n_insns):
        word = struct.unpack_from("<I", data, i * 4)[0]
        op, rs, rt, imm = decode_insn(word)

        if op != 0x0F:
            continue

        lui_reg = rt
        lui_hi = imm
        lui_va = base_va + i * 4

        # Match lui 0x8067 or lui 0x8068
        if lui_hi not in (target_hi, adj_hi):
            continue

        # Search forward for ori/addiu constructing the full address, or sw/sh at matching offset
        for j in range(i + 1, min(i + 1 + WINDOW, n_insns)):
            jw = struct.unpack_from("<I", data, j * 4)[0]
            jop, jrs, jrt, jimm = decode_insn(jw)
            j_va = base_va + j * 4

            # ORI rX, lui_reg, 0x94F0 (constructing full VA for indirect store)
            if jop == 0x0D and jrs == lui_reg and jimm == target_lo:
                results.append({
                    "type": "lui_ori_construct",
                    "lui_va": lui_va,
                    "ori_va": j_va,
                    "lui_hi": lui_hi,
                    "ori_lo": jimm,
                    "dest_reg": MIPS_REGS[jrt],
                    "constructed_va": (lui_hi << 16) | jimm,
                })

            # ADDIU rX, lui_reg, signed_offset (constructing full VA)
            if jop == 0x09 and jrs == lui_reg and jimm == target_lo:
                constructed = (lui_hi << 16) + (jimm if jimm < 0x8000 else jimm - 0x10000)
                constructed &= 0xFFFFFFFF
                results.append({
                    "type": "lui_addiu_construct",
                    "lui_va": lui_va,
                    "addiu_va": j_va,
                    "lui_hi": lui_hi,
                    "addiu_imm": jimm,
                    "dest_reg": MIPS_REGS[jrt],
                    "constructed_va": constructed,
                })

            # SW rY, offset(lui_reg) where offset matches
            if jop == 0x2B and jrs == lui_reg and jimm == target_lo:
                results.append({
                    "type": "lui_sw_cbptr",
                    "lui_va": lui_va,
                    "sw_va": j_va,
                    "lui_hi": lui_hi,
                    "sw_offset": jimm,
                    "store_reg": MIPS_REGS[jrt],
                    "base_reg": MIPS_REGS[jrs],
                })

    # Also look for any instruction that uses 0x94F0 as an ORI immediate
    # (could be building the address in a register for an indirect store)
    for i in range(n_insns):
        word = struct.unpack_from("<I", data, i * 4)[0]
        op, rs, rt, imm = decode_insn(word)
        insn_va = base_va + i * 4

        if op == 0x0D and imm == target_lo:  # ORI with 0x94F0
            # Check if preceded by lui 0x8067 or 0x8068
            already_found = any(r.get("ori_va") == insn_va for r in results)
            if not already_found:
                # Look back for a lui loading 0x8067/0x8068 into rs
                for k in range(max(0, i - 16), i):
                    kw = struct.unpack_from("<I", data, k * 4)[0]
                    kop, _, krt, kimm = decode_insn(kw)
                    if kop == 0x0F and krt == rs and kimm in (target_hi, adj_hi):
                        results.append({
                            "type": "ori_with_lui_backref",
                            "lui_va": base_va + k * 4,
                            "ori_va": insn_va,
                            "lui_hi": kimm,
                            "ori_lo": imm,
                            "dest_reg": MIPS_REGS[rt],
                            "constructed_va": (kimm << 16) | imm,
                        })
                        break

    return results


def scan_indexed_table_access(data, base_va, table_base=0x80075580, table_end=0x800755A0):
    """Search for classic C table[index] dispatch: SLL rX, rY, 2 + ADDU + LW + JALR.

    Also searches for the table base address being loaded from a structure field
    (LW rBase, offset(rStruct)) followed by indexed access.
    """
    results = []
    n_insns = len(data) // 4
    WINDOW = 6

    for i in range(n_insns):
        word = struct.unpack_from("<I", data, i * 4)[0]
        op, rs, rt, imm = decode_insn(word)
        rd = (word >> 11) & 0x1F
        funct = word & 0x3F
        sa = (word >> 6) & 0x1F

        # Look for SLL rX, rY, 2 (word index shift)
        if op != 0 or funct != 0x00 or sa != 2:
            continue
        if word == 0:  # skip NOP
            continue

        sll_rd = rd
        sll_va = base_va + i * 4

        # Search forward for ADDU rZ, rBase, sll_rd
        for j in range(i + 1, min(i + 1 + WINDOW, n_insns)):
            jw = struct.unpack_from("<I", data, j * 4)[0]
            jop = (jw >> 26) & 0x3F
            jrs = (jw >> 21) & 0x1F
            jrt = (jw >> 16) & 0x1F
            jrd = (jw >> 11) & 0x1F
            jfunct = jw & 0x3F

            if jop != 0 or jfunct != 0x21:  # not ADDU
                continue
            # One of the source regs must be sll_rd
            if jrs != sll_rd and jrt != sll_rd:
                continue

            addu_rd = jrd
            addu_va = base_va + j * 4

            # Search forward for LW rTarget, offset(addu_rd) with small offset
            for k in range(j + 1, min(j + 1 + WINDOW, n_insns)):
                kw = struct.unpack_from("<I", data, k * 4)[0]
                kop, krs, krt, kimm = decode_insn(kw)
                if kop != 0x23:  # not LW
                    continue
                if krs != addu_rd:
                    continue
                lw_offset = kimm if kimm < 0x8000 else kimm - 0x10000
                if abs(lw_offset) > 0x100:  # small offset expected for table[i]
                    continue

                lw_rd = krt
                lw_va = base_va + k * 4

                # Check for JALR following
                has_jalr = False
                jalr_va = 0
                for l in range(k + 1, min(k + 1 + 4, n_insns)):
                    lw2 = struct.unpack_from("<I", data, l * 4)[0]
                    lop = (lw2 >> 26) & 0x3F
                    lfunct = lw2 & 0x3F
                    lrs = (lw2 >> 21) & 0x1F
                    if lop == 0 and lfunct == 0x09 and lrs == lw_rd:
                        has_jalr = True
                        jalr_va = base_va + l * 4
                        break

                results.append({
                    "sll_va": sll_va,
                    "addu_va": addu_va,
                    "lw_va": lw_va,
                    "lw_offset": lw_offset,
                    "has_jalr": has_jalr,
                    "jalr_va": jalr_va,
                    "index_reg": MIPS_REGS[rt],  # SLL source
                    "dest_reg": MIPS_REGS[lw_rd],
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

    # ================================================================
    # Section 1: Original lui+sw / direct_sw / sh scans
    # ================================================================
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

    sh_results = scan_sh(data, args.base)
    if sh_results:
        print(f"=== SH (halfword store) offset matches ({len(sh_results)}) ===")
        for r in sh_results:
            print(f"[MATCH] sh @ VA=0x{r['sw_va']:08X} (0x{r['sw_insn']:08X})"
                  f"  -> target 0x{r['target_va']:08X} ({r['target_name']})"
                  f"  base={r['base_reg']} src={r['store_reg']}"
                  f"  offset=0x{r['offset']:04X}")
        print()

    total = len(lui_results) + len(direct_results) + len(sh_results)
    print(f"Total candidate store producers: {total}")

    lui_sw_vas = {r["sw_va"] for r in lui_results}
    direct_only = [r for r in direct_results if r["sw_va"] not in lui_sw_vas]
    if direct_only:
        print(f"Direct-only (no preceding lui): {len(direct_only)}")
        for r in direct_only:
            print(f"  VA=0x{r['sw_va']:08X} base={r['base_reg']} -> 0x{r['target_va']:08X} ({r['target_name']})")
    print()

    # ================================================================
    # Section 2: JAL caller scan for producer functions
    # ================================================================
    # 0x800962A0 is the actual function entry (not 0x800962B0 which is mid-block BNE)
    # 0x80096260 is the twin function writing to different globals
    PRODUCER_VAS = [0x800962A0, 0x80096260, 0x800962B0]

    for PRODUCER_VA in PRODUCER_VAS:
        print(f"{'='*60}")
        print(f"=== CALLER REPORT for 0x{PRODUCER_VA:08X} ===")
        print(f"{'='*60}")

        jal_results = scan_jal_callers(data, args.base, PRODUCER_VA)
        if jal_results:
            print(f"Found {len(jal_results)} caller(s):")
            for r in jal_results:
                print(f"  [{r['type']}] caller @ VA=0x{r['caller_va']:08X} insn=0x{r['insn']:08X}")
                if r['type'] == 'jalr_indirect':
                    print(f"    lui @ 0x{r['lui_va']:08X}, target loaded via {r['load_reg']}")

                print(f"  Context window:")
                for line in dump_context_disasm(data, args.base, r['caller_va'], half_window=12):
                    print(f"    {line}")
                print()
        else:
            print("  No JAL/JALR callers found!")
            print()

    # Dump the producer function block
    print(f"--- Producer functions 0x80096260..0x80096300 disassembly ---")
    for line in dump_context_disasm(data, args.base, 0x80096280, half_window=32):
        print(f"  {line}")
    print()

    # ================================================================
    # Section 2b: Function pointer table scan
    # ================================================================
    # Search for VAs stored as literal words in the binary (function pointer tables)
    FPTR_TARGETS = [0x800962A0, 0x80096260, 0x806794F0]
    print(f"{'='*60}")
    print(f"=== FUNCTION POINTER / VA REFERENCE SCAN ===")
    print(f"{'='*60}")

    n_words = len(data) // 4
    for target in FPTR_TARGETS:
        refs = []
        target_bytes = struct.pack("<I", target)
        # Search for exact word match
        for i in range(n_words):
            w = struct.unpack_from("<I", data, i * 4)[0]
            if w == target:
                ref_va = args.base + i * 4
                refs.append(ref_va)

        if refs:
            print(f"0x{target:08X} found as literal word at {len(refs)} location(s):")
            for ref_va in refs[:20]:
                print(f"  VA=0x{ref_va:08X}")
                # Show context (as data, not code — could be a table)
                for off in range(-4, 5):
                    ctx_va = ref_va + off * 4
                    ctx_off = ctx_va - args.base
                    cw = read_word(data, ctx_off)
                    if cw is not None:
                        marker = " <<" if off == 0 else ""
                        print(f"    0x{ctx_va:08X}: {cw:08X}{marker}")
                print()
        else:
            print(f"0x{target:08X} NOT found as literal word anywhere in binary")

    # ================================================================
    # Section 3: Extended callback pointer (0x806794F0) producer scan
    # ================================================================
    CBPTR_VA = 0x806794F0
    print(f"{'='*60}")
    print(f"=== PRODUCER REPORT for 0x{CBPTR_VA:08X} (callback ptr) ===")
    print(f"{'='*60}")

    cbptr_results = scan_callback_ptr_producers(data, args.base, CBPTR_VA)
    if cbptr_results:
        print(f"Found {len(cbptr_results)} candidate(s):")
        for r in cbptr_results:
            print(f"  [{r['type']}]", end="")
            if 'lui_va' in r:
                print(f" lui @ 0x{r['lui_va']:08X}", end="")
            if 'ori_va' in r:
                print(f" ori @ 0x{r['ori_va']:08X}", end="")
            if 'addiu_va' in r:
                print(f" addiu @ 0x{r['addiu_va']:08X}", end="")
            if 'sw_va' in r:
                print(f" sw @ 0x{r['sw_va']:08X}", end="")
            if 'constructed_va' in r:
                print(f" -> constructed=0x{r['constructed_va']:08X}", end="")
            if 'dest_reg' in r:
                print(f" reg={r['dest_reg']}", end="")
            if 'store_reg' in r:
                print(f" store_reg={r['store_reg']}", end="")
            print()

            # Context disassembly around the key instruction
            center = r.get('sw_va') or r.get('ori_va') or r.get('addiu_va') or r.get('lui_va')
            if center:
                print(f"  Context:")
                for line in dump_context_disasm(data, args.base, center, half_window=8):
                    print(f"    {line}")
                print()
    else:
        print("  No candidates found via lui+ori/addiu/sw patterns!")
        print()

    # ================================================================
    # Section 2c: Table dispatch analysis
    # ================================================================
    # The function pointer table at 0x80075580..0x8007559C contains 0x800962A0
    # Find who reads from this table region
    TABLE_BASE = 0x80075580
    TABLE_END = 0x800755A0
    print(f"\n{'='*60}")
    print(f"=== TABLE DISPATCH ANALYSIS (table @ 0x{TABLE_BASE:08X}) ===")
    print(f"{'='*60}")

    # Search for lui loading 0x8007 or 0x8008 (for negative offsets from 0x8008xxxx)
    # followed by lw with offset matching table region
    table_lo = TABLE_BASE & 0xFFFF  # 0x5580
    table_hi_part = (TABLE_BASE >> 16) & 0xFFFF  # 0x8007
    # Since 0x5580 < 0x8000, lui loads 0x8007 directly

    print(f"Table base: 0x{TABLE_BASE:08X}, lui=0x{table_hi_part:04X}, lo=0x{table_lo:04X}")
    print(f"Searching for lui 0x{table_hi_part:04X} + lw with offset 0x{table_lo:04X}..0x{TABLE_END & 0xFFFF:04X}")

    table_refs = []
    for i in range(n_words):
        word = struct.unpack_from("<I", data, i * 4)[0]
        op, rs_f, rt, imm = decode_insn(word)
        if op != 0x0F or imm != table_hi_part:
            continue
        lui_reg = rt
        lui_va = args.base + i * 4
        # Search forward for lw using this reg with offset in table range
        for j in range(i + 1, min(i + 1 + 16, n_words)):
            jw = struct.unpack_from("<I", data, j * 4)[0]
            jop, jrs, jrt, jimm = decode_insn(jw)
            if jop == 0x23 and jrs == lui_reg:  # lw
                effective_va = (table_hi_part << 16) + (jimm if jimm < 0x8000 else jimm - 0x10000)
                effective_va &= 0xFFFFFFFF
                if TABLE_BASE <= effective_va < TABLE_END:
                    lw_va = args.base + j * 4
                    table_refs.append({
                        "lui_va": lui_va,
                        "lw_va": lw_va,
                        "effective_va": effective_va,
                        "dest_reg": MIPS_REGS[jrt],
                    })

    # Also search for addiu constructing table base address
    for i in range(n_words):
        word = struct.unpack_from("<I", data, i * 4)[0]
        op, rs_f, rt, imm = decode_insn(word)
        if op != 0x0F or imm != table_hi_part:
            continue
        lui_reg = rt
        lui_va = args.base + i * 4
        for j in range(i + 1, min(i + 1 + 8, n_words)):
            jw = struct.unpack_from("<I", data, j * 4)[0]
            jop, jrs, jrt, jimm = decode_insn(jw)
            if jop == 0x09 and jrs == lui_reg and jrt == lui_reg:  # addiu
                constructed = (table_hi_part << 16) + (jimm if jimm < 0x8000 else jimm - 0x10000)
                constructed &= 0xFFFFFFFF
                if TABLE_BASE <= constructed < TABLE_END:
                    addiu_va = args.base + j * 4
                    table_refs.append({
                        "lui_va": lui_va,
                        "addiu_va": addiu_va,
                        "effective_va": constructed,
                        "dest_reg": MIPS_REGS[jrt],
                        "type": "addiu_base",
                    })

    if table_refs:
        print(f"Found {len(table_refs)} reference(s) to table region:")
        for r in table_refs:
            key_va = r.get('lw_va') or r.get('addiu_va')
            print(f"  lui @ 0x{r['lui_va']:08X}, ref @ 0x{key_va:08X}"
                  f" -> effective 0x{r['effective_va']:08X} dest={r['dest_reg']}")
            print(f"  Context:")
            for line in dump_context_disasm(data, args.base, key_va, half_window=12):
                print(f"    {line}")
            print()
    else:
        print("  No direct lui+lw references to table found")
        # Try broader search: any lui 0x8007 + addiu with offset near 0x5580
        # Search for construction of table base address 0x80075580
        # lui 0x8008 + addiu -0xAA80 or lui 0x8007 + addiu/ori 0x5580
        print("  Searching for address construction near 0x80075580...")
        tbl_constructs = []
        for i in range(n_words):
            word = struct.unpack_from("<I", data, i * 4)[0]
            op, rs_f, rt, imm = decode_insn(word)
            if op != 0x0F:
                continue
            lui_reg = rt
            lui_va = args.base + i * 4
            for j in range(i + 1, min(i + 1 + 12, n_words)):
                jw = struct.unpack_from("<I", data, j * 4)[0]
                jop, jrs, jrt, jimm = decode_insn(jw)
                if jrs != lui_reg:
                    continue
                if jop in (0x09, 0x0D):  # addiu or ori
                    constructed = (imm << 16)
                    if jop == 0x09:  # addiu (sign-extends)
                        constructed += (jimm if jimm < 0x8000 else jimm - 0x10000)
                    else:  # ori
                        constructed |= jimm
                    constructed &= 0xFFFFFFFF
                    if TABLE_BASE <= constructed <= TABLE_END:
                        ref_va = args.base + j * 4
                        tbl_constructs.append((lui_va, ref_va, constructed, MIPS_REGS[jrt]))
        if tbl_constructs:
            for lui_va, ref_va, cva, reg in tbl_constructs:
                print(f"    lui @ 0x{lui_va:08X}, ref @ 0x{ref_va:08X}"
                      f" -> 0x{cva:08X} in {reg}")
                for line in dump_context_disasm(data, args.base, ref_va, half_window=10):
                    print(f"      {line}")
                print()
        else:
            print("    No address constructions found for table base")
            # Last resort: search for any word containing 0x80075580
            # Also search for indexed access — SLL + ADDU + LW pattern
            # where the base could come from any register
            print("  Searching for 0x80075580 stored as immediate in any form...")
            # Check if table base is near $gp value
            # Also try: any LW that reads from the table slot 0x80075590
            slot_va = 0x80075590
            slot_hi = (slot_va >> 16) & 0xFFFF  # 0x8007
            slot_lo = slot_va & 0xFFFF  # 0x5590
            print(f"  Searching for lui 0x{slot_hi:04X} + lw offset 0x{slot_lo:04X} (direct slot read)...")
            for i in range(n_words):
                word = struct.unpack_from("<I", data, i * 4)[0]
                op, rs_f, rt, imm = decode_insn(word)
                if op != 0x0F or imm != slot_hi:
                    continue
                lui_reg = rt
                for j in range(i + 1, min(i + 1 + 8, n_words)):
                    jw = struct.unpack_from("<I", data, j * 4)[0]
                    jop, jrs, jrt, jimm = decode_insn(jw)
                    if jop == 0x23 and jrs == lui_reg and jimm == slot_lo:
                        ref_va = args.base + j * 4
                        print(f"    lui @ 0x{args.base + i*4:08X}, lw @ 0x{ref_va:08X}")
                        for line in dump_context_disasm(data, args.base, ref_va, half_window=8):
                            print(f"      {line}")

    # ================================================================
    # Section 2d: Indexed table access scan (SLL+ADDU+LW+JALR)
    # ================================================================
    print(f"\n{'='*60}")
    print(f"=== INDEXED TABLE ACCESS SCAN (SLL+ADDU+LW+JALR) ===")
    print(f"{'='*60}")

    idx_results = scan_indexed_table_access(data, args.base, TABLE_BASE, TABLE_END)
    if idx_results:
        jalr_count = sum(1 for r in idx_results if r['has_jalr'])
        print(f"Found {len(idx_results)} SLL+ADDU+LW sequences ({jalr_count} with JALR):")
        for r in idx_results:
            jalr_info = f" JALR @ 0x{r['jalr_va']:08X}" if r['has_jalr'] else " (no JALR)"
            print(f"  SLL @ 0x{r['sll_va']:08X}  ADDU @ 0x{r['addu_va']:08X}"
                  f"  LW @ 0x{r['lw_va']:08X} (offset={r['lw_offset']})"
                  f"  index={r['index_reg']} dest={r['dest_reg']}{jalr_info}")
            print(f"  Context:")
            for line in dump_context_disasm(data, args.base, r['sll_va'], half_window=10):
                print(f"    {line}")
            print()
    else:
        print("  No SLL+ADDU+LW indexed access patterns found")

    # ================================================================
    print(f"{'='*60}")
    print("=== INSTRUMENTATION TARGETS ===")
    print(f"{'='*60}")

    instr_targets = set()

    # Callback pointer producer PCs
    for r in cbptr_results:
        for key in ('sw_va', 'ori_va', 'addiu_va', 'lui_va'):
            if key in r:
                instr_targets.add(r[key])
                break

    # Function pointer references — instrument the functions themselves
    for target in FPTR_TARGETS:
        if target < 0x80680000:  # code addresses, not data globals
            instr_targets.add(target)

    if instr_targets:
        print("PCs to instrument at runtime:")
        for va in sorted(instr_targets):
            print(f"  0x{va:08X}")
    else:
        print("  No new instrumentation targets identified")

    # For C code generation
    if instr_targets:
        print()
        print("C code snippet for upstream PC list:")
        for va in sorted(instr_targets):
            print(f"            pc32 == 0x{va:08X}u ||")

    print()


if __name__ == "__main__":
    main()
