#!/usr/bin/env python3
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
"""SlopFin dev tooling: hold one Remote Play session to the PS5 and take commands.

Lets a script see the console's whole screen (system menus included, not just
SlopFin) and press controller buttons. Pair first with pair.elf (see README.md);
the registration lives in ~/.config/slopfin-rp/profiles.json, outside the repo.

    rp-control.py pair                  # one-time: pair.elf over elfldr, then register
    rp-control.py serve                 # connect and listen on 127.0.0.1:47800
    rp-control.py press RIGHT CROSS     # tap buttons in order
    rp-control.py hold PS 1.5           # hold one button for N seconds
    rp-control.py shot out.png          # save the latest frame

"""

import asyncio
import os
import sys

from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from console_config import console_host

PROFILE = os.environ.get("SLOPFIN_RP_PROFILE", "slopfin")
PORT = 47800
PROFILES = os.path.expanduser("~/.config/slopfin-rp/profiles.json")


async def serve():
    HOST = console_host()
    from pyremoteplay.device import RPDevice
    from pyremoteplay.profile import Profiles
    from pyremoteplay.receiver import QueueReceiver

    profiles = Profiles.load(PROFILES)
    device = RPDevice(HOST)
    await device.async_get_status()
    receiver = QueueReceiver(max_frames=4)
    device.create_session(PROFILE, profiles=profiles, receiver=receiver, resolution="1080p", fps="low",
                          codec="h264")
    if not await device.connect():
        print("connect failed:", device.session.error, flush=True)
        return 1
    if not await device.async_wait_for_session(timeout=30):
        print("session not ready:", device.session.error, flush=True)
        return 1
    device.controller.start()
    print("session ready", flush=True)

    async def handle(reader, writer):
        line = (await reader.readline()).decode().strip()
        words = line.split()
        reply = "ok"
        try:
            if not words:
                reply = "empty"
            elif words[0] == "press":
                for name in words[1:]:
                    await device.controller.async_button(name.upper(), "tap", delay=0.12)
                    await asyncio.sleep(0.35)
            elif words[0] == "hold":
                await device.controller.async_button(words[1].upper(), "tap", delay=float(words[2]))
            elif words[0] == "shot":
                frame = receiver.get_latest_video_frame()
                if frame is None:
                    reply = "no frame yet"
                else:
                    frame.to_image().save(words[1])
            elif words[0] == "status":
                reply = f"connected={device.connected} frames={len(receiver.video_frames)}"
            elif words[0] == "quit":
                device.disconnect()
                await asyncio.sleep(1.5)  # let the stop reach the console, or it refuses the next session
                writer.write(b"bye\n")
                await writer.drain()
                os._exit(0)
            else:
                reply = "unknown command"
        except Exception as error:  # report, keep serving
            reply = f"error: {error!r}"
        writer.write((reply + "\n").encode())
        await writer.drain()
        writer.close()

    server = await asyncio.start_server(handle, "127.0.0.1", PORT)
    async with server:
        while device.connected:
            await asyncio.sleep(1)
    print("session ended:", device.session.error if device.session else "", flush=True)
    return 2


def pair():
    """Send pair.elf, read the account id and PIN it writes, register, save the profile."""
    HOST = console_host()
    import io, json, socket, time
    from ftplib import FTP
    from pyremoteplay.ddp import get_status
    from pyremoteplay.register import register

    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "pair.elf"), "rb") as elf, socket.create_connection((HOST, 9021), timeout=20) as s:
        s.sendall(elf.read())
    text = ""
    for _ in range(20):
        time.sleep(1)
        ftp = FTP(); ftp.connect(HOST, 2121, timeout=20); ftp.login()
        buf = io.BytesIO()
        try:
            ftp.retrbinary("RETR /data/slopfin-rp-pair.txt", buf.write)
        except Exception:
            pass
        ftp.quit()
        text = buf.getvalue().decode(errors="replace")
        if "pin " in text or "error" in text:
            break
    fields = dict(line.split(" ", 1) for line in text.splitlines() if line.startswith(("account_id ", "pin ")))
    if "pin" not in fields:
        print(text or "pair.elf wrote nothing")
        return 1
    status = get_status(HOST)
    info = register(HOST, fields["account_id"], fields["pin"], timeout=10)
    if not info:
        print("registration refused")
        return 1
    data = {k.split("-", 1)[1] if k.startswith(status["host-type"] + "-") else k: v for k, v in info.items()}
    profiles = {PROFILE: {"id": fields["account_id"],
                          "hosts": {status["host-id"]: {"data": data, "type": status["host-type"]}}}}
    os.makedirs(os.path.dirname(PROFILES), exist_ok=True)
    with open(PROFILES, "w") as out:
        json.dump(profiles, out, indent=1)
    os.chmod(PROFILES, 0o600)
    print("paired with", status.get("host-name"), "->", PROFILES)
    return 0


def send(line):
    import socket
    with socket.create_connection(("127.0.0.1", PORT), timeout=60) as s:
        s.sendall((line + "\n").encode())
        return s.makefile().readline().strip()


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    if sys.argv[1] == "serve":
        sys.exit(asyncio.run(serve()))
    if sys.argv[1] == "pair":
        sys.exit(pair())
    print(send(" ".join(sys.argv[1:])))
