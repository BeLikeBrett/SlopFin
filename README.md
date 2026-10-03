# SlopFin

**Jellyfin for your PlayStation 5, built around the DualSense.**

[![Build](https://github.com/BeLikeBrett/SlopFin/actions/workflows/tooling.yml/badge.svg)](https://github.com/BeLikeBrett/SlopFin/actions/workflows/tooling.yml)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)](LICENSE)

A native movie and TV client for **jailbroken PS5 consoles**, using the PS5's
H.264/HEVC hardware decoder. Firmware **8.20** is the tested setup.

[Downloads](https://github.com/BeLikeBrett/SlopFin/releases) ·
[Install](docs/GETTING_STARTED.md) · [Controls](docs/CONTROLS.md) ·
[Compatibility](docs/COMPATIBILITY.md) ·
[Report a bug](https://github.com/BeLikeBrett/SlopFin/issues/new/choose)

![SlopFin on PS5](docs/images/home-console.png)

## A player that uses the controller

- **Adaptive triggers and pressure-sensitive seeking.** L2 rewinds and R2 moves forward. Tap for a one-second
  adjustment; hold and press deeper to move faster. Adaptive trigger resistance
  gives the seek control a physical feel. D-pad taps move ten seconds, and
  L1/R1 jump two minutes. Trigger feedback can be switched off.
- **Autoplay per show.** Keep one series rolling and turn autoplay off for
  another, without changing the global default. An Up Next card appears over
  the credits, and configurable “Are you still watching?” prompts stop
  unattended playback.
- **Search that keeps shows visible.** Movies, series and episodes have separate
  rows and result limits. A matching series stays easy to find even when its
  episodes also match. Text entry uses the PlayStation keyboard.
- **Server controls from the couch.** Administrator accounts can view sessions,
  devices, libraries and tasks, with server actions available from the
  controller. [Profile and dashboard guide](docs/PROFILE_AND_DASHBOARD.md).

Resume playback, audio/subtitle selection, subtitle styling and timing,
quality limits, and Skip Intro are included. Intro skipping uses Jellyfin's
Intro segments or named chapters; [server setup](docs/INTRO_SKIPPING.md)
determines which episodes have a skip button.

## Playback support

| Path | Formats | Status |
| --- | --- | --- |
| PS5 hardware video | H.264, HEVC Main/Main10, including tested 4K sources | Console tested |
| Console audio decoding | AAC, MP3, AC-3 to PCM | Supported rates/layouts; server fallback for others |
| HDMI audio passthrough | AC-3, E-AC-3, DTS core | Experimental; depends on your TV/receiver |
| Optional FFmpeg audio decoding | E-AC-3, DTS core, TrueHD to 48 kHz PCM | Included in builds, opt-in experiment |

Jellyfin remuxes compatible video into the delivery stream and transcodes
unsupported media. It can convert audio while preserving the original video.
4K sources currently render through a 1920-wide presentation path.

Native HDR10 output remains experimental. Some Dolby Vision sources can use
an HDR10-compatible base layer; there is no native Dolby Vision output.
TrueHD PCM does not preserve Atmos objects, and DTS passthrough carries the
core rather than DTS-HD/DTS:X. See [compatibility](docs/COMPATIBILITY.md)
for output limits and experimental setup.

## Install

1. Download the **folder ZIP** from [Releases](https://github.com/BeLikeBrett/SlopFin/releases).
2. Transfer the complete `PPSA99001` folder to your homebrew loader's directory,
   commonly `/data/homebrew/`, and let the loader register it.
3. Open SlopFin, enter your Jellyfin server address, and sign in using Quick
   Connect or your username and password. `media.example.com` uses HTTPS;
   an explicit URL with a port or base path works too.

You need a jailbroken PS5 and your own Jellyfin server. The folder build is
recommended. **Native FPKG is experimental and currently fails to launch on
the test console during package mounting.**
[Setup details](docs/GETTING_STARTED.md) · [Help](docs/TROUBLESHOOTING.md)

Music, audiobooks, photo libraries and live TV are not implemented. IPv6 and
self-signed HTTPS certificates are unsupported. Other firmware and output
devices need independent testing.

## Next preview

Circular avatar cropping and **Settings → Updates** are in the next preview.
The GitHub updater still needs its final console download/install test;
01.000.002 remains a draft until that passes. Updates require elfldr and apply
to folder installs. [Update instructions and status](docs/UPDATES.md).

## Contribute

Start with [contributing](CONTRIBUTING.md) and the
[developer guide](docs/development/README.md) for builds, architecture,
playback internals and validation. Player instructions are in the
[help index](docs/README.md).

## License and credits

GPL-3.0-or-later. Built on the native-app boilerplate and ProsperoTV's
hardware-decode work. [NOTICE](NOTICE.md) records dependencies, fonts,
artwork and their licenses.

Unofficial; not affiliated with Jellyfin or Sony.
