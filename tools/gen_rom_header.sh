#!/bin/sh
# Generate a C header from a binary file using xxd -i
# Usage: gen_rom_header.sh <input.bin> <output.h>
set -e
INPUT="$1"
OUTPUT="$2"
{
    echo "/* Auto-generated from docs/hardware/be300_boot_rom.bin */"
    echo "static const"
    xxd -i -n be300_boot_rom < "$INPUT"
} | sed 's/^unsigned int/static const unsigned int/' > "$OUTPUT"
