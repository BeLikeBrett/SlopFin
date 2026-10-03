#!/usr/bin/env bash
# SlopFin - fetch whatever the console knows about the last failure.
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Prints the newest crash report, the session that was running, and says
# whether the previous run ended without shutting down. With --all it prints
# the whole history; with --clear it deletes the reports from the console.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/console-env.sh"
mode=${1:-latest}
python3 - "$host" "$mode" <<'PY'
import io, sys
from ftplib import FTP, error_perm
host, mode = sys.argv[1], sys.argv[2]

files = {
    'latest': '/data/slopfin-crash.txt',
    'all': '/data/slopfin-crashes.log',
    'session': '/data/slopfin-session.txt',
    'lastrun': '/data/slopfin-lastrun.txt',
}

def connect():
    f = FTP(); f.connect(host, 2121, timeout=20); f.login(); return f

def fetch(path):
    body = io.BytesIO()
    f = connect()
    try:
        f.retrbinary('RETR ' + path, body.write)
    except error_perm:
        return None
    finally:
        f.quit()
    return body.getvalue().decode(errors='replace')

if mode == '--clear':
    f = connect()
    for path in files.values():
        try:
            f.delete(path)
            print('removed', path)
        except error_perm:
            pass
    f.quit()
    raise SystemExit(0)

wanted = ['all'] if mode in ('--all', 'all') else ['latest', 'lastrun', 'session']
for key in wanted:
    body = fetch(files[key])
    if not body:
        print(f'--- {key}: nothing ---')
        continue
    print(f'--- {key} ({files[key]}) ---')
    print(body.rstrip())
    print()
PY
