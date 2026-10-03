#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
# SlopFin - make sure the app is running, relaunching it if it has died.
# Prints the state it found. Used by the other tools so a crash mid-session
# does not need a person at the console.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/console-env.sh"
python3 - "$host" <<'PY'
import io, sys, time
from ftplib import FTP
host = sys.argv[1]

def alive():
    """The app rewrites its trace on every launch; a responding marker proves
    the render loop is still turning."""
    try:
        f = FTP(); f.connect(host, 2121, timeout=15); f.login()
        f.storbinary('STOR /data/slopfin-alive', io.BytesIO(b'1'))
        f.quit()
    except Exception:
        return False
    time.sleep(4)
    try:
        f = FTP(); f.connect(host, 2121, timeout=15); f.login()
        names = []
        f.retrlines('LIST /data', names.append)
        f.quit()
    except Exception:
        return False
    # The app deletes the marker when it sees it.
    return not any('slopfin-alive' in n for n in names)

if alive():
    print("app is running")
    raise SystemExit(0)

print("app is not responding, relaunching")
import subprocess, os
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) if "__file__" in dir() else "."
result = subprocess.run(["bash", "tools/launch.sh"], capture_output=True, text=True)
if result.returncode != 0:
    print(result.stdout + result.stderr)
    raise SystemExit(1)
for attempt in range(8):
    time.sleep(4)
    if alive():
        print(f"app came back after {(attempt + 1) * 4}s")
        raise SystemExit(0)
print("app did not come back")
raise SystemExit(1)
PY
