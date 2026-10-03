#!/usr/bin/env bash
# SlopFin - drive the Linux preview headless and collect captures.
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# One command between a question about how something looks and a PNG that
# answers it. The script language is host_main.cpp's:
#
#   wait 240; left; down x2; cross; wait 180; shot movies.png
#
# Presses are injected one per frame, exactly as the console's press.sh does,
# so a capture taken here and one taken there are the same sequence.
#
#   tools/capture.sh out/ "wait 240; shot home.png"
#   tools/capture.sh out/ scripts/grid.txt          # a file of steps
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd -- "$here"

if [[ $# -lt 2 ]]; then
    printf '%s\n' "usage: tools/capture.sh <out-dir> <script|script-file> [extra slopfin args]" >&2
    exit 2
fi

out="$1"
script="$2"
shift 2

if [[ -f "$script" ]]; then
    # A file of steps, comments and blank lines allowed.
    script="$(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$script" | tr '\n' ';')"
fi

binary="build/host/slopfin"
if [[ ! -x "$binary" ]] || [[ -n "$(find src host -newer "$binary" -name '*.cpp' -o -newer "$binary" -name '*.hpp' 2>/dev/null | head -1)" ]]; then
    printf '%s\n' "==> [capture] rebuilding the preview" >&2
    make host >/dev/null
fi

mkdir -p -- "$out"
printf '%s\n' "==> [capture] $script" >&2
# A generous timeout: artwork comes off a real server, and a script that waits
# for it is doing the right thing. Killing the run is still better than a
# headless process nobody notices.
timeout 300 "$binary" --headless --exit-after-script --out "$out" --script "$script" "$@"
printf '%s\n' "==> [capture] $(ls -1 "$out" | wc -l) file(s) in $out" >&2
