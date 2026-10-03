#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
# SlopFin - report the measured refresh rate and the television's own limits.
# The app must be running. Pulls the EDID as well when the console hands one over.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/console-env.sh"
out=${1:-/tmp/slopfin-display.txt}
edid=${2:-/tmp/slopfin-edid.bin}
python3 - "$host" "$out" "$edid" <<'PY'
import io, sys, time
from ftplib import FTP, error_perm
host, out, edid = sys.argv[1], sys.argv[2], sys.argv[3]
f = FTP(); f.connect(host, 2121, timeout=25); f.login()
f.storbinary('STOR /data/slopfin-display', io.BytesIO(b'1'))
f.quit()
print("probing...")
time.sleep(8)
f = FTP(); f.connect(host, 2121, timeout=40); f.login()
buf = io.BytesIO()
f.retrbinary('RETR /data/slopfin-display.txt', buf.write)
open(out, 'wb').write(buf.getvalue())
print(buf.getvalue().decode('utf-8', 'replace'))
try:
    raw = io.BytesIO()
    f.retrbinary('RETR /data/slopfin-edid.bin', raw.write)
    open(edid, 'wb').write(raw.getvalue())
    print(f"EDID: {len(raw.getvalue())} bytes -> {edid}")
except error_perm:
    print("EDID: the console did not hand one over")
f.quit()
PY
