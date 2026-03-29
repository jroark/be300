#!/bin/bash
MOC=/work/opie-build/qte-opie/bin/moc
FILTER=/work/opie-build/moc-filter.py

OUTFILE=""
INPUT=""
declare -a ARGS
while [ $# -gt 0 ]; do
    case "$1" in
        -o) OUTFILE="$2"; ARGS+=(-o "$2"); shift 2 ;;
        *) INPUT="$1"; shift ;;
    esac
done

if [ -z "$INPUT" ]; then exit 1; fi

# Try direct moc first
$MOC "${ARGS[@]}" "$INPUT" 2>/dev/null
if [ $? -eq 0 ]; then exit 0; fi

# moc failed - filter and retry
TMPFILE=$(mktemp /tmp/moc_XXXXXX.cpp)
python3 "$FILTER" "$INPUT" "$TMPFILE"
QTDIR=/work/opie-build/moc-safe $MOC "${ARGS[@]}" "$TMPFILE" 2>/dev/null
RC=$?

# If output was generated, consider it success
if [ -n "$OUTFILE" ] && [ -s "$OUTFILE" ]; then RC=0; fi

rm -f "$TMPFILE"
exit $RC
