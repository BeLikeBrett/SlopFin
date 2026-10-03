#!/usr/bin/env python3
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn the addresses in a SlopFin crash report into functions and lines."""
import re
import subprocess
import sys
import pathlib

report = pathlib.Path(sys.argv[1]).read_text(errors="replace")
elf = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else "build/llvm-pie.elf")
if not elf.exists():
    raise SystemExit(f"no {elf}: build the same commit the report came from")

anchor = re.search(r"anchor\s+(\S+)\s+0x([0-9a-f]+)", report)
if not anchor:
    raise SystemExit("the report has no anchor line; it came from an older build")
symbol, runtime = anchor.group(1), int(anchor.group(2), 16)

listing = subprocess.run(["llvm-nm", str(elf)], capture_output=True, text=True).stdout
link = None
for line in listing.splitlines():
    parts = line.split()
    if len(parts) == 3 and parts[2] == symbol:
        link = int(parts[0], 16)
if link is None:
    raise SystemExit(f"{symbol} is not in {elf}: the report is from a different build")

base = runtime - link
print(f"load base 0x{base:x}  (anchor {symbol})\n")
wanted = []
for line in report.splitlines():
    found = re.match(r"\s*(?:rip\s+|\d+\s+)0x([0-9a-f]+)\s*$", line)
    if found:
        wanted.append((line.strip(), int(found.group(1), 16)))
for text, address in wanted:
    offset = address - base
    if offset < 0:
        print(f"{text}  (outside this module)")
        continue
    resolved = subprocess.run(["llvm-addr2line", "-f", "-C", "-e", str(elf), hex(offset)],
                              capture_output=True, text=True).stdout.strip().splitlines()
    name = resolved[0] if resolved else "?"
    where = resolved[1] if len(resolved) > 1 else "?"
    print(f"{text}  ->  {name}  ({where})")
