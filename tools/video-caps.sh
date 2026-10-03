#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
# SlopFin - report which decoder configurations the console accepts.
# The app must be running; the probe allocates a compute queue, so do not run it
# during playback.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/console-env.sh"
python3 - "$host" <<'PY'
import io, sys, time
from ftplib import FTP
host = sys.argv[1]
f = FTP(); f.connect(host, 2121, timeout=25); f.login()
f.storbinary('STOR /data/slopfin-video-caps', io.BytesIO(b'1'))
f.quit()
print("probing...")
time.sleep(12)
buf = io.BytesIO()
f = FTP(); f.connect(host, 2121, timeout=40); f.login()
f.retrbinary('RETR /data/slopfin-video-caps.txt', buf.write)
f.quit()
print(buf.getvalue().decode())
PY
