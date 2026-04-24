import sys

path = sys.argv[1]
start_va = int(sys.argv[2], 0)
end_va = int(sys.argv[3], 0)
base = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x80060000

nk = open(path, "rb").read()
for i in range(start_va - base, end_va - base, 4):
    if i < 0 or i + 4 > len(nk):
        continue
    w = int.from_bytes(nk[i:i + 4], "little")
    # addiu sp, sp, -N prologue
    if (w & 0xffff0000) == 0x27bd0000 and (w & 0xffff) >= 0x8000:
        print("prologue 0x%08x fs=%d" % (base + i, 0x10000 - (w & 0xffff)))
