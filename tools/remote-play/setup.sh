#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
# SlopFin dev tooling - build the Remote Play client environment in .local/rp-venv.
# pyremoteplay 0.7.6 is unmaintained; three local fixes make it work against a
# PS5 on firmware 8.20 with current PyAV.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
venv="$root/.local/rp-venv"
python3 -m venv "$venv"
"$venv/bin/pip" install -q pyremoteplay av pillow async_timeout 'pyee<9'
"$venv/bin/python" - <<'PY'
import pathlib, re, pyremoteplay
base = pathlib.Path(pyremoteplay.__file__).parent

# 1. The console hands back a 7-byte registration key; the session request
#    encrypts it padded to 16 bytes (chiaki-ng does the same). The library padded
#    to 15, and the console reset the connection.
session = base / "session.py"
text = session.read_text()
text = text.replace('regist_key = b"".join([bytes.fromhex(self._regist_key), bytes(8)])',
                    'regist_key = bytes.fromhex(self._regist_key).ljust(16, b"\\x00")')
session.write_text(text)

# 2. PyAV renamed its flag enums to lower case and dropped ThreadType.AUTO.
receiver = base / "receiver" / "__init__.py"
text = receiver.read_text()
text = text.replace("Flags.LOW_DELAY", "Flags.low_delay").replace("Flags2.FAST", "Flags2.fast")
text = re.sub(r"codec_ctx\.thread_type = .*", "codec_ctx.thread_type = av.codec.context.ThreadType.FRAME", text)
receiver.write_text(text)
print("patched", base)
PY
echo "ready: $venv/bin/python tools/remote-play/rp-control.py serve"
