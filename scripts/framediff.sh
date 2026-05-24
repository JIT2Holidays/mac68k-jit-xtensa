#!/bin/sh
# Compare two 512x342 1bpp Mac framebuffer BMPs and report byte-level
# difference percentage. Used by the bench-scripting probe pipeline to
# detect when a click / keypress actually changed the screen.
#
# Usage: scripts/framediff.sh frame_a.bmp frame_b.bmp
# Output: diff_bytes=<count> / <total> (<percent>%)
set -e
[ -f "$1" ] || { echo "missing $1" >&2; exit 1; }
[ -f "$2" ] || { echo "missing $2" >&2; exit 1; }
SZ=$(wc -c < "$1")
DIFFS=$(cmp -l "$1" "$2" 2>/dev/null | wc -l || true)
PCT=$(echo "scale=2; $DIFFS * 100 / $SZ" | bc)
echo "diff_bytes=$DIFFS / $SZ ($PCT%)"
