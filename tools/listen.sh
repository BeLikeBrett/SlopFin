#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
# SlopFin - capture decoded audio from the running app and check it.
# Usage: listen.sh [seconds] [output.wav]
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/console-env.sh"
seconds=${1:-4}
out=${2:-/tmp/slopfin-audio.wav}
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
python3 - "$host" "$seconds" "$out" <<'PY'
import io, sys, time, wave
from ftplib import FTP, error_perm
host, seconds, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]

def wait_consumed(path):
    for _ in range(40):
        f = FTP(); f.connect(host, 2121, timeout=10); f.login()
        rows = []
        f.retrlines('LIST /data', rows.append)
        f.quit()
        if not any(row.split()[-1] == path.rsplit('/', 1)[-1] for row in rows):
            return
        time.sleep(0.25)
    raise SystemExit(f'App did not consume {path}; no new audio capture available.')

f = FTP(); f.connect(host, 2121, timeout=25); f.login()
# An empty capture writes no output. Never let an earlier successful recording
# stand in for a silent or failed decoder in this run.
for path in ('/data/slopfin-audio.raw', '/data/slopfin-audio.txt',
             '/data/slopfin-listen', '/data/slopfin-listen-dump'):
    try:
        f.sendcmd('DELE ' + path)
    except error_perm as exc:
        if not str(exc).startswith('550'):
            raise
f.storbinary('STOR /data/slopfin-listen', io.BytesIO(b'1'))
f.quit()
wait_consumed('/data/slopfin-listen')
print(f"capturing {seconds:.0f}s of decoded audio")
time.sleep(seconds)

f = FTP(); f.connect(host, 2121, timeout=25); f.login()
f.storbinary('STOR /data/slopfin-listen-dump', io.BytesIO(b'1'))
f.quit()
wait_consumed('/data/slopfin-listen-dump')
time.sleep(2)

raw = bytearray()
side = bytearray()
f = FTP(); f.connect(host, 2121, timeout=40); f.login()
try:
    f.retrbinary('RETR /data/slopfin-audio.raw', raw.extend)
    f.retrbinary('RETR /data/slopfin-audio.txt', side.extend)
except error_perm as exc:
    raise SystemExit(f'No complete new audio capture; playback may be silent: {exc}')
f.quit()

# The app states the layout it captured, because it is no longer always stereo.
meta = dict(line.split() for line in side.decode().splitlines() if len(line.split()) == 2)
try:
    channels, rate, frames = (int(meta[key]) for key in ('channels', 'rate', 'frames'))
except (KeyError, ValueError):
    raise SystemExit('Invalid audio capture metadata; no WAV written.')
if channels <= 0 or rate <= 0 or frames <= 0 or len(raw) != frames * channels * 2:
    raise SystemExit('Empty or incomplete audio capture; no WAV written.')

with wave.open(out, 'wb') as handle:
    handle.setnchannels(channels)
    handle.setsampwidth(2)
    handle.setframerate(rate)
    handle.writeframes(bytes(raw))
print(f"wrote {out}: {len(raw)//(2*channels)} frames, {channels} channels at {rate} Hz")
PY
python3 "$root/tools/analyse-audio.py" "$out"
