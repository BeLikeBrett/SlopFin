#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
# SlopFin - run the on-console audio capability probe and print its report.
# The app must be running and not playing; the probe opens the audio sink.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/console-env.sh"
python3 - "$host" <<'PY'
import io, sys, time
from ftplib import FTP
host = sys.argv[1]
f = FTP(); f.connect(host, 2121, timeout=25); f.login()
f.storbinary('STOR /data/slopfin-audio-caps', io.BytesIO(b'1'))
f.quit()
print("probing...")
time.sleep(30)
buf = io.BytesIO()
f = FTP(); f.connect(host, 2121, timeout=60); f.login()
f.retrbinary('RETR /data/slopfin-audio-caps.txt', buf.write)
f.quit()
print(buf.getvalue().decode())
PY
