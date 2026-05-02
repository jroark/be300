#!/bin/sh
# Generate a C header from a binary file using xxd -i.
# Usage: gen_embedded_header.sh <input> <output.h> <symbol>
#
# Produces a header that defines:
#   static const unsigned char <symbol>[]   = { ... };
#   static const unsigned int  <symbol>_len = N;
set -e
INPUT="$1"
OUTPUT="$2"
SYMBOL="$3"
if [ -z "$INPUT" ] || [ -z "$OUTPUT" ] || [ -z "$SYMBOL" ]; then
    echo "Usage: $0 <input> <output.h> <symbol>" >&2
    exit 1
fi
{
    echo "/* Auto-generated from $INPUT */"
    echo "static const"
    xxd -i -n "$SYMBOL" < "$INPUT"
} | sed 's/^unsigned int/static const unsigned int/' > "$OUTPUT"
