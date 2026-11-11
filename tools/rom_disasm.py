#!/usr/bin/env python3
"""Disassemble the BE-300 boot ROM (docs/hardware/be300_boot_rom.bin).

The ROM is MIPS32 from 0x0100 to 0x0C1F and MIPS16 from 0x0C20 to 0x219B.
The MIPS16 part is decoded here (enough of the instruction set for this
ROM; PC-relative loads show the literal). The MIPS32 part uses capstone
when it is installed and is listed as words otherwise.

    tools/rom_disasm.py                 # whole ROM
    tools/rom_disasm.py 1088 10b8       # a range of ROM offsets (hex)
"""
import os, struct, sys

ROM = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..',
                   'docs', 'hardware', 'be300_boot_rom.bin')
d = open(ROM, 'rb').read()
MIPS16_START, MIPS16_END = 0x0C20, 0x219C

R=['s0','s1','v0','v1','a0','a1','a2','a3']
def dec(pc,w,ext):
    op=w>>11; rx=(w>>8)&7; ry=(w>>5)&7; rz=(w>>2)&7; imm8=w&0xff; imm5=w&0x1f
    def e(val):
        v=((ext&0x1f)<<11)|(((ext>>5)&0x3f)<<5)|(val&0x1f)
        return v-0x10000 if v&0x8000 else v
    X=ext is not None
    s8=(imm8^0x80)-0x80
    if op==0b10110:
        a=((pc-2)&~3)+e(imm8) if X else (pc&~3)+imm8*4
        return 'lw %s,[%05x]=%08x'%(R[rx],a,struct.unpack_from('<I',d,a)[0])
    if op==0b10011: return 'lw %s,%d(%s)'%(R[ry],e(imm5) if X else imm5*4,R[rx])
    if op==0b10001: return 'lh %s,%d(%s)'%(R[ry],e(imm5) if X else imm5*2,R[rx])
    if op==0b10101: return 'lhu %s,%d(%s)'%(R[ry],e(imm5) if X else imm5*2,R[rx])
    if op==0b10100: return 'lbu %s,%d(%s)'%(R[ry],e(imm5) if X else imm5,R[rx])
    if op==0b10000: return 'lb %s,%d(%s)'%(R[ry],e(imm5) if X else imm5,R[rx])
    if op==0b11011: return 'sw %s,%d(%s)'%(R[ry],e(imm5) if X else imm5*4,R[rx])
    if op==0b11001: return 'sh %s,%d(%s)'%(R[ry],e(imm5) if X else imm5*2,R[rx])
    if op==0b11000: return 'sb %s,%d(%s)'%(R[ry],e(imm5) if X else imm5,R[rx])
    if op==0b10010: return 'lw %s,%d(sp)'%(R[rx],imm8*4)
    if op==0b11010: return 'sw %s,%d(sp)'%(R[rx],imm8*4)
    if op==0b01101: return 'li %s,0x%x'%(R[rx],(e(imm8)&0xffff) if X else imm8)
    if op==0b01110: return 'cmpi %s,0x%x'%(R[rx],(e(imm8)&0xffff) if X else imm8)
    if op==0b00100: return 'beqz %s,%05x'%(R[rx],pc+2+(e(imm8) if X else s8)*2)
    if op==0b00101: return 'bnez %s,%05x'%(R[rx],pc+2+(e(imm8) if X else s8)*2)
    if op==0b00010:
        o=w&0x7ff; o=o-0x800 if o&0x400 else o
        return 'b %05x'%(pc+2+o*2)
    if op==0b01100:
        n=['bteqz','btnez','sw ra,sp','adjsp','svrs','move32','?','move']
        if rx<2: return '%s %05x'%(n[rx],pc+2+(e(imm8) if X else s8)*2)
        return '%s 0x%x'%(n[rx],imm8)
    if op==0b11101:
        fn=w&0x1f; n={0:'jr/jalr',2:'slt',3:'sltu',4:'sllv',6:'srlv',7:'srav',10:'cmp',11:'neg',12:'and',13:'or',14:'xor',15:'not',16:'mfhi',18:'mflo',24:'mult',25:'multu',26:'div',27:'divu'}.get(fn,'rr%d'%fn)
        return '%s %s,%s'%(n,R[rx],R[ry])
    if op==0b01001: return 'addiu %s,%d'%(R[rx],e(imm8) if X else s8)
    if op==0b01000: return 'addiu %s,%s,%d'%(R[ry],R[rx],e(w&0xf) if X else ((w&0xf)^8)-8)
    if op==0b11100: return '%s %s,%s,%s'%('subu' if (w&3)==3 else 'addu',R[rz],R[rx],R[ry])
    if op==0b00110: return '%s %s,%s,%d'%({0:'sll',2:'srl',3:'sra'}.get(w&3,'sh?'),R[rx],R[ry],((w>>2)&7) or 8)
    if op==0b01010: return 'slti %s,0x%x'%(R[rx],imm8)
    if op==0b01011: return 'sltiu %s,0x%x'%(R[rx],imm8)
    if op==0b00000: return 'addiu %s,sp,%d'%(R[rx],imm8*4)
    if op==0b00001: return 'addiu %s,pc,%d'%(R[rx],imm8*4)
    return '?? op=%s' % bin(op)

def mips16(a, b):
    pc = a; ext = None
    while pc < b:
        w = struct.unpack_from('<H', d, pc)[0]
        if (w >> 11) == 0b11110:
            ext = w; pc += 2; continue
        if (w >> 11) == 0b00011:
            w2 = struct.unpack_from('<H', d, pc + 2)[0]
            t = (((w & 0x1f) << 21) | (((w >> 5) & 0x1f) << 16) | w2) << 2
            print('9fc%05x: %04x %04x  jal%s 0x9fc%05x' % (pc, w, w2, 'x' if w & 0x400 else '', t & 0xfffff))
            pc += 4; continue
        print('9fc%05x: %04x       %s%s' % (pc, w, '(ext) ' if ext is not None else '', dec(pc, w, ext)))
        ext = None; pc += 2


def mips32(a, b):
    try:
        from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN
    except ImportError:
        for pc in range(a, b, 4):
            print('9fc%05x: %08x' % (pc, struct.unpack_from('<I', d, pc)[0]))
        return
    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)
    md.skipdata = True
    for i in md.disasm(d[a:b], 0x9fc00000 + a):
        print('%08x: %-9s %-8s %s' % (i.address, i.bytes[::-1].hex(), i.mnemonic, i.op_str))


def main():
    if len(sys.argv) == 3:
        a, b = int(sys.argv[1], 16), int(sys.argv[2], 16)
    else:
        a, b = 0, 0x2250
    if a < MIPS16_START:
        mips32(a, min(b, MIPS16_START))
    if b > MIPS16_START and a < MIPS16_END:
        mips16(max(a, MIPS16_START), min(b, MIPS16_END))
    for pc in range(max(a, MIPS16_END), b, 4):
        print('9fc%05x: %08x  .word' % (pc, struct.unpack_from('<I', d, pc)[0]))


if __name__ == '__main__':
    main()
