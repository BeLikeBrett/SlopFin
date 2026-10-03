#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
# SlopFin - pull the app's frame dump off the console as a PNG.
# Usage: shot.sh [output.png] ["x y w h" crop in physical pixels]
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/console-env.sh"
out=${1:-/tmp/slopfin-shot.png}
# With no crop the app dumps the whole surface halved, which is what a look at
# the layout needs. "x y w h" in physical pixels dumps that box at its true
# size, which is the only way to judge sharpness.
crop=${2:-}
python3 - "$host" "$out" "$crop" <<'PY'
import io, struct, sys, time
from ftplib import FTP
from PIL import Image
host, out, crop = sys.argv[1], sys.argv[2], sys.argv[3].strip()
# Ask the app for a frame, rather than having it write one on a timer: that
# write costs eight vertical blanks and shows up as a stutter in playback.
f = FTP(); f.connect(host, 2121, timeout=25); f.login()
# The old dump goes first. Without that, a read that arrives before the app has
# finished writing the new one succeeds against the previous frame and reports
# a screenshot of something that is no longer on screen.
try:
    f.delete('/data/slopfin-frame.bin')
except Exception:
    pass
f.storbinary('STOR /data/slopfin-shot', io.BytesIO((crop or '1').encode()))
f.quit()
time.sleep(1.5)

# The app writes the dump from the render thread, and a read that starts while
# it is still writing fails outright or stops short, so this waits it out
# rather than reporting a broken screenshot.
stamp = 'unknown'
buf = bytearray()
for attempt in range(8):
    f = FTP(); f.connect(host, 2121, timeout=120); f.login()
    rows = []
    f.retrlines('LIST /data', rows.append)
    for row in rows:
        if row.endswith('slopfin-frame.bin'):
            stamp = ' '.join(row.split()[5:8])
    buf = bytearray()
    last = 'short read'
    try:
        f.retrbinary('RETR /data/slopfin-frame.bin', buf.extend, blocksize=65536)
        f.quit()
        if len(buf) >= 16:
            magic, w, h = struct.unpack_from('<III', buf, 0)
            if magic == 0x46504c53 and len(buf) >= 16 + w * h * 4:
                break
            last = f'{len(buf)} bytes for {w}x{h}'
    except Exception as error:
        last = error
        try:
            f.quit()
        except Exception:
            pass
    time.sleep(1.5)
else:
    raise SystemExit(f"could not read the dump: {last}")
print(f"frame written at {stamp} (console clock, UTC)")
# 'SLPF', width, height, format (0=ARGB8, 1=HDR10), then pixels.
magic, w, h, pixel_format = struct.unpack_from('<IIII', buf, 0)
if magic != 0x46504c53:
    raise SystemExit(f"not a surface dump: magic {magic:#x}")
pixels = 16 + w * h * 4
if len(buf) < pixels:
    raise SystemExit(f"short frame: {len(buf)} bytes, wanted {pixels} for {w}x{h}")
if pixel_format == 1:
    import numpy as np
    packed = np.frombuffer(buf, dtype='<u4', count=w*h, offset=16).reshape(h, w)
    pq = np.stack([(packed >> s) & 1023 for s in (0, 10, 20)], axis=-1) / 1023.0
    p = pq ** (32 / 2523)
    light = (np.maximum(p - 3424/4096, 0) / (2413/128 - 2392/128*p)) ** (16384/2610) * 10000
    matrix = np.array([[1.660491, -0.587641, -0.072850],
                       [-0.124550, 1.132900, -0.008349],
                       [-0.018151, -0.100579, 1.118730]])
    rgb = np.clip(light @ matrix.T / 203, 0, 1)
    rgb = np.where(rgb <= 0.0031308, rgb * 12.92, 1.055 * rgb ** (1/2.4) - 0.055)
    im = Image.fromarray(np.uint8(np.rint(rgb * 255)), 'RGB')
    print('HDR10 diagnostic SDR preview: highlights above 203 nits clipped; not HDMI validation')
elif pixel_format == 0:
    im = Image.frombytes('RGBA', (w, h), bytes(buf[16:pixels]))
    b, g, r, a = im.split()
    im = Image.merge('RGB', (r, g, b))
else:
    raise SystemExit(f'unknown capture pixel format: {pixel_format}')
im.save(out)
print(f"wrote {out} ({im.width}x{im.height})")
PY
