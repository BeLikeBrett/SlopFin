#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
# SlopFin - send button presses to the running app, then screenshot.
# Usage: press.sh "left down cross" [output.png] [settle seconds]
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/console-env.sh"
keys=${1:?buttons required}
shot=${2:-/tmp/slopfin-shot.png}
# How long to let the interface settle before the frame is pulled. The playback
# controls retire after four seconds, so testing them needs a shorter wait.
settle=${3:-4}
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
python3 - "$host" "$keys" "$settle" <<'PY'
import io, sys, time
from ftplib import FTP
host, keys, settle = sys.argv[1], sys.argv[2], float(sys.argv[3])
# One key per write: everything in a single file lands on the same frame,
# which is not a sequence.
for key in keys.split():
    f = FTP(); f.connect(host, 2121, timeout=25); f.login()
    f.storbinary('STOR /data/slopfin-input', io.BytesIO(key.encode()))
    f.quit()
    # The app reads the input file every twenty frames, so presses sent
    # closer together than that overwrite each other before it looks.
    time.sleep(1.6 if settle >= 4 else 0.7)
time.sleep(settle)
PY
bash "$root/tools/shot.sh" "$shot"
