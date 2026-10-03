#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
# SlopFin - relaunch the app on the console and collect a screenshot and trace.
# Needs CheatRunner resident on port 9999 as the launcher.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/console-env.sh"
shot=${1:-/tmp/slopfin-shot.png}
wait_seconds=${2:-14}
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

# Ask any running instance to close itself, wait for it to go, then clear the
# marker so the next launch does not immediately see it and close too.
python3 - "$host" <<'PY2'
import io, sys, time
from ftplib import FTP
host = sys.argv[1]
try:
    f = FTP(); f.connect(host, 2121, timeout=25); f.login()
    f.storbinary('STOR /data/slopfin-quit', io.BytesIO(b'1'))
    f.quit()
    time.sleep(4)
    f = FTP(); f.connect(host, 2121, timeout=25); f.login()
    try:
        f.delete('/data/slopfin-quit')
    except Exception:
        pass  # the app consumed it, which is the good case
    f.quit()
except Exception as error:
    print(f"(could not request close: {error})")
PY2
# CheatRunner is not always resident; the payload launcher always is.
bash "$root/tools/launch.sh" >/dev/null 2>&1
python3 - "$wait_seconds" <<'PY'
import sys, time
time.sleep(float(sys.argv[1]))
PY
bash "$root/tools/shot.sh" "$shot"
echo "--- trace ---"
python3 - "$host" <<'PY'
import sys
from ftplib import FTP
f = FTP(); f.connect(sys.argv[1], 2121, timeout=25); f.login()
buf = bytearray()
f.retrbinary('RETR /data/slopfin-trace.txt', buf.extend)
f.quit()
print(buf.decode('utf-8', 'replace').strip())
PY
