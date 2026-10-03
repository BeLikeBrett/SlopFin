# Remote Play control (dev tooling)

Lets a script see the PS5's whole screen and press
controller buttons, including in the system menus that `tools/press.sh` cannot
reach (it only drives SlopFin).

```
tools/remote-play/setup.sh                                   # once: .local/rp-venv, patched pyremoteplay
make -C tools/remote-play PS5_PAYLOAD_SDK=$PWD/.deps/native/ps5-payload-sdk
.local/rp-venv/bin/python tools/remote-play/rp-control.py pair    # once per console
.local/rp-venv/bin/python tools/remote-play/rp-control.py serve   # hold a session
python3 tools/remote-play/rp-control.py press UP RIGHT CROSS
python3 tools/remote-play/rp-control.py shot /tmp/ps5.png
python3 tools/remote-play/rp-control.py quit
```

`pair.elf` is a headless version of ps5-payload-dev/linkdev: it reads the signed-in
user's account id from the registry, asks the Remote Play service for a PIN, and
writes both to `/data/slopfin-rp-pair.txt`, so no one has to read the TV. Set `PS5_HOST` in `.env` or the environment. `SLOPFIN_RP_PROFILE` selects
the profile name (default `slopfin`); existing registrations can keep their name
by setting that variable. The registration is saved to `~/.config/slopfin-rp/profiles.json`, outside the repo.

## What it cannot do (measured)

- **SlopFin streams as a black picture.** Frames arrive at 30 fps, about 145
  bytes each. Close SlopFin (`/data/slopfin-quit`) to see the system UI; use
  `tools/shot.sh` for SlopFin itself.
- **Settings > Screen and Video will not open during Remote Play**: "Can't use
  this feature while using Remote Play." The HDR setting is written through the
  registry instead (`tools/hdr-setting`).
- **Remote Play's HDR stream says nothing about the TV.** Asked for HEVC HDR, it
  was tagged PQ / BT.2020 both while SlopFin played HDR10 and while it showed its
  SDR home screen.
- **Sessions need a cool-down.** A session that ends abruptly, or a refused
  connection, blocks the next one ("Another Remote Play session is connected")
  for roughly two to three minutes. `quit` stops the session properly.
