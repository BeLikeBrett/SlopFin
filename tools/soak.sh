#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
# SlopFin - watch the app until it dies, then capture why.
# Usage: soak.sh [minutes] [--play <itemId>]
# Polls liveness; on the first failure it records the process table, the tail of
# the app's own trace, and whether the process is gone or merely wedged.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/console-env.sh"
minutes=${1:-10}
shift || true
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
python3 - "$host" "$minutes" "$root" "$@" <<'PY'
import io, subprocess, sys, time
from ftplib import FTP

host, minutes, root = sys.argv[1], float(sys.argv[2]), sys.argv[3]
play = None
if "--play" in sys.argv:
    play = sys.argv[sys.argv.index("--play") + 1]

def ftp():
    f = FTP(); f.connect(host, 2121, timeout=12); f.login(); return f

def put(path, data=b"1"):
    f = ftp(); f.storbinary(f"STOR {path}", io.BytesIO(data)); f.quit()

def get(path):
    f = ftp(); buf = io.BytesIO()
    try:
        f.retrbinary(f"RETR {path}", buf.write); return buf.getvalue()
    except Exception:
        return None
    finally:
        f.quit()

def responding():
    """The app deletes this marker each pass through the render loop."""
    try:
        put("/data/slopfin-alive")
    except Exception:
        return None  # console unreachable, not necessarily the app
    time.sleep(3)
    try:
        f = ftp(); names = []; f.retrlines("LIST /data", names.append); f.quit()
    except Exception:
        return None
    return not any("slopfin-alive" in n for n in names)

def process_table():
    elf = f"{root}/.deps/native/ps5-payload-sdk/samples/ps/ps.elf"
    import socket
    try:
        payload = open(elf, "rb").read()
        s = socket.create_connection((host, 9021), timeout=20)
        s.sendall(payload); s.shutdown(socket.SHUT_WR); s.settimeout(15)
        out = b""
        while True:
            chunk = s.recv(8192)
            if not chunk: break
            out += chunk
        s.close()
        return "\n".join(l for l in out.decode(errors="replace").split("\n")
                         if "99001" in l or "PID" in l)
    except Exception as error:
        return f"(process table unavailable: {error})"

if play:
    put("/data/slopfin-input", f"play:{play}".encode())
    print(f"started playback of {play}")
    time.sleep(15)

deadline = time.time() + minutes * 60
checks = 0
print(f"watching for {minutes:.0f} minutes")
while time.time() < deadline:
    state = responding()
    checks += 1
    if state is False:
        elapsed = minutes * 60 - (deadline - time.time())
        print(f"\n*** app stopped responding after {elapsed:.0f}s ({checks} checks) ***")
        print("--- process table ---")
        print(process_table())
        trace = get("/data/slopfin-trace.txt")
        if trace:
            print("--- last 25 trace lines ---")
            print("\n".join(trace.decode(errors="replace").split("\n")[-25:]))
        raise SystemExit(2)
    if state is None:
        print("  (console unreachable this pass)")
    time.sleep(5)
print(f"survived {minutes:.0f} minutes, {checks} liveness checks")
PY
