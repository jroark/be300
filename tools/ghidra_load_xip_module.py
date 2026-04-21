#@menupath File.BE-300.Load WinCE XIP Module
#@category BE-300
#@keybinding
#@toolbar
#@description Load a WinCE XIP module extracted by tools/extract_xip_modules.py
# Prompts for the module's .bin file, reads the .txt sidecar next to it, and
# creates per-section memory blocks with correct permissions at the module's
# preferred vbase.
#
# Jython 2.7 (Ghidra default). Run this INSIDE an already-imported program —
# it doesn't create a new program. If you need a fresh program, first do
# File -> Import File -> select the .bin as Raw Binary (language
# MIPS:LE:32:default, base address 0x0 — we'll relocate), then run this script.
#
# Or: start from an EMPTY program (File -> New -> MIPS:LE:32:default), then
# run this script; it will create blocks and load bytes from the .bin into them.

from ghidra.program.model.mem import MemoryConflictException
from java.io import File
import os


def parse_sidecar(path):
    """Parse the .txt sidecar produced by tools/extract_xip_modules.py.
    Returns (name, vbase, sections[]). Each section is a dict with keys
    block_name, perm, vstart, vsize, psize, file_off, src.
    """
    header = {}
    sections = []
    in_table = False
    with open(path, 'r') as f:
        for line in f:
            line = line.rstrip()
            if not line:
                continue
            if line.startswith('---'):
                in_table = True
                continue
            if line.startswith('idx') and 'name' in line:
                continue  # column header
            if not in_table:
                if ':' in line:
                    k, v = [p.strip() for p in line.split(':', 1)]
                    header[k] = v
                continue
            # Table row: "  0 .text  r-x  0x00011000 0x002BA6 0x002C00 ..."
            parts = line.split()
            if len(parts) < 9:
                continue
            sections.append({
                'idx':       int(parts[0]),
                'block_name': parts[1],
                'perm':       parts[2],
                'vstart':     int(parts[3], 16),
                'vsize':      int(parts[4], 16),
                'psize':      int(parts[5], 16),
                'file_off':   int(parts[6], 16),
                'flags':      int(parts[7], 16),
                'src':        parts[8],
            })
    return (
        header.get('module', 'unknown'),
        int(header['vbase'], 16),
        int(header['vsize'], 16),
        sections,
    )


def main():
    bin_file = askFile("Select the module .bin file", "Load")
    bin_path = bin_file.getAbsolutePath()
    sidecar_path = os.path.splitext(bin_path)[0] + '.txt'
    if not os.path.isfile(sidecar_path):
        printerr("Sidecar not found: " + sidecar_path)
        printerr("Run tools/extract_xip_modules.py first to regenerate it.")
        return

    name, vbase, vsize, sections = parse_sidecar(sidecar_path)
    print("Loading %s at 0x%08X (vsize=0x%06X, %d sections)"
          % (name, vbase, vsize, len(sections)))

    with open(bin_path, 'rb') as f:
        bin_data = f.read()
    if len(bin_data) < vsize:
        printerr("WARNING: .bin is smaller than vsize (0x%X < 0x%X); "
                 "tail will be zero-filled." % (len(bin_data), vsize))

    memory = currentProgram.getMemory()
    addr_space = currentProgram.getAddressFactory().getDefaultAddressSpace()

    for s in sections:
        if s['vsize'] == 0:
            continue
        name_suffix = "_%s_%d" % (s['block_name'].lstrip('.'), s['idx'])
        block_name = name + name_suffix
        start_addr = addr_space.getAddress(s['vstart'])
        blob = bin_data[s['file_off']:s['file_off'] + s['vsize']]
        if len(blob) < s['vsize']:
            blob = blob + b'\x00' * (s['vsize'] - len(blob))
        try:
            blk = memory.createInitializedBlock(
                block_name, start_addr, len(blob), 0, monitor, False,
            )
        except MemoryConflictException as e:
            printerr("Conflict creating block %s: %s — skipping."
                     % (block_name, e))
            continue
        # Write bytes
        blk.putBytes(start_addr, bytes(blob) if isinstance(blob, bytearray) else blob)
        blk.setRead('r' in s['perm'])
        blk.setWrite('w' in s['perm'])
        blk.setExecute('x' in s['perm'])
        print("  [%d] %s  %s  0x%08X..0x%08X  (%s)"
              % (s['idx'], block_name, s['perm'],
                 s['vstart'], s['vstart'] + s['vsize'] - 1, s['src']))

    print("Done. Next: Analysis -> Auto Analyze to let Ghidra resolve "
          "functions and references.")


if __name__ == '__main__':
    main()
