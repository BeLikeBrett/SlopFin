#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
# SlopFin - record a frame-timing trace from the running app.
# Usage: trace.sh [seconds] [output.csv]
# Both the trace and the screenshot cost several vertical blanks to write, so
# the app only produces them when asked.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/console-env.sh"
seconds=${1:-20}
out=${2:-/tmp/slopfin-frames.csv}
python3 - "$host" "$seconds" "$out" <<'PY'
import csv, io, sys, time
from ftplib import FTP, error_perm
host, seconds, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]

def wait_consumed(path):
    for _ in range(40):
        f = FTP(); f.connect(host, 2121, timeout=10); f.login()
        names = []
        f.retrlines('LIST /data', names.append)
        f.quit()
        if not any(row.split()[-1] == path.rsplit('/', 1)[-1] for row in names):
            return
        time.sleep(0.25)
    raise SystemExit(f'App did not consume {path}; playback may not be running. No trace retrieved.')

def build_stamp(ftp):
    data = bytearray()
    ftp.retrbinary('RETR /data/slopfin-trace.txt', data.extend)
    return data.decode(errors='replace').splitlines()[0]

f = FTP(); f.connect(host, 2121, timeout=25); f.login()
initial_build = build_stamp(f)
# Remove an earlier recording so it can never masquerade as this run.
for path in ('/data/slopfin-frames.csv', '/data/slopfin-trace-dump'):
    try:
        f.sendcmd('DELE ' + path)
    except error_perm as exc:
        if not str(exc).startswith('550'):
            raise
f.storbinary('STOR /data/slopfin-trace-on', io.BytesIO(b'1'))
f.quit()
wait_consumed('/data/slopfin-trace-on')
print(f"recording for {seconds:.0f}s")
time.sleep(seconds)

f = FTP(); f.connect(host, 2121, timeout=25); f.login()
f.storbinary('STOR /data/slopfin-trace-dump', io.BytesIO(b'1'))
f.quit()
wait_consumed('/data/slopfin-trace-dump')
time.sleep(2)

f = FTP(); f.connect(host, 2121, timeout=40); f.login()
final_build = build_stamp(f)
if final_build != initial_build:
    f.quit()
    raise SystemExit(f'Console build changed during recording ({initial_build} -> {final_build}); trace is invalid.')
# Fetch before opening the local output, so failures cannot destroy a prior file.
data = bytearray()
try:
    f.retrbinary('RETR /data/slopfin-frames.csv', data.extend)
except error_perm as exc:
    f.quit()
    raise SystemExit(f'No completed trace available; the app may have restarted during recording: {exc}')
with open(out, 'wb') as handle:
    handle.write(data)
f.quit()
print(f"wrote {out}")
rows = list(csv.DictReader(io.StringIO(data.decode())))
if len(rows) > 1:
    span = (int(rows[-1]['at_us']) - int(rows[0]['at_us'])) / 1_000_000
    print(f"Captured span: {span:.2f}s ({len(rows)} samples), requested {seconds:.0f}s")
    if span < seconds * 0.9:
        print("Only the retained tail is available; the bounded trace ring may have wrapped. Do not report the requested duration as measured.")
PY
