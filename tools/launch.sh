#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
# SlopFin - start the app on the console without CheatRunner.
# Sends a small payload to elfldr, which asks the launcher to start the title.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/console-env.sh"
title=${1:-PPSA99001}
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
elf="$root/tools/launcher/slopfin-launch.elf"
[[ -f $elf ]] || PS5_PAYLOAD_SDK="$root/.deps/native/ps5-payload-sdk" make -C "$root/tools/launcher"
# The launcher kills any stale instance, which from the app's point of view is
# a run that never shut down. Clear its session file first so the next start
# does not report the deploy cycle as a crash.
python3 - "$host" <<'PY2'
import sys
from ftplib import FTP, error_perm
host = sys.argv[1]
try:
    f = FTP(); f.connect(host, 2121, timeout=15); f.login()
    try:
        f.delete('/data/slopfin-session.txt')
    except error_perm:
        pass
    f.quit()
except Exception:
    pass
PY2

python3 - "$host" "$elf" <<'PY'
import socket, sys
host, path = sys.argv[1], sys.argv[2]
payload = open(path, 'rb').read()
s = socket.create_connection((host, 9021), timeout=20)
s.sendall(payload)
s.shutdown(socket.SHUT_WR)
try:
    reply = s.recv(4096)
    if reply:
        print(reply.decode(errors='replace').strip())
except Exception:
    pass
s.close()
print(f"sent {len(payload)} byte launch payload")
PY
