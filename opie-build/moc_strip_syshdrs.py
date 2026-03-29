#!/usr/bin/env python3
"""Strip system header content from CXX -E output for Qt 2.x moc compatibility.

The GCC 14 preprocessor expands system headers (musl, libstdc++) containing
'inline namespace', __attribute__ syntax, and C++11/14/17 constructs that
Qt 2.x moc cannot parse. This script removes content from system headers
while preserving all Qt/Opie content that moc needs for signal/slot generation.
"""
import re
import sys

def main():
    infile = sys.argv[1]
    with open(infile, 'r') as f:
        lines = f.readlines()

    out = []
    in_system_header = False

    # System header paths to strip (buildroot sysroot and toolchain headers)
    system_markers = ('buildroot/output/', '/usr/include/', '<built-in>', '<command-line>')

    for line in lines:
        # Track which file we're in via # line-number "filename" markers
        m = re.match(r'^#\s+\d+\s+"([^"]+)"', line)
        if m:
            filepath = m.group(1)
            in_system_header = any(marker in filepath for marker in system_markers)
            out.append(line)
            continue

        if in_system_header:
            # Replace system header content with empty lines to preserve line numbers
            out.append('\n')
        else:
            out.append(line)

    with open(infile, 'w') as f:
        f.writelines(out)

if __name__ == '__main__':
    main()
