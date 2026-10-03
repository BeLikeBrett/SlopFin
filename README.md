# SlopFin

**Your Jellyfin library, on PlayStation 5.**

[![Build](https://github.com/BeLikeBrett/SlopFin/actions/workflows/tooling.yml/badge.svg)](https://github.com/BeLikeBrett/SlopFin/actions/workflows/tooling.yml)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)](LICENSE)

An unofficial native client for **jailbroken PS5 consoles**, built around a
controller interface and the console's hardware video decoder.

[Download the preview](https://github.com/BeLikeBrett/SlopFin/releases/tag/01.000.001) ·
[Installation](docs/GETTING_STARTED.md) ·
[Compatibility](docs/COMPATIBILITY.md) ·
[Report a bug](https://github.com/BeLikeBrett/SlopFin/issues/new/choose)

![SlopFin Home with movie artwork and controller controls](docs/images/home-console.png)

*Home captured directly from SlopFin on a PS5 running firmware 8.20, using a
real Jellyfin library. Library names, artwork and recommendations come from
your server.*

## Choose your download

| Download | How it runs | Current status |
| --- | --- | --- |
| **[App folder ZIP](https://github.com/BeLikeBrett/SlopFin/releases/download/01.000.001/SlopFin-01.000.001-folder.zip)** | Extract and transfer the complete PPSA99001 folder to your homebrew loader | **Recommended.** Launch/playback tested on firmware 8.20 |
| **[Native FPKG](https://github.com/BeLikeBrett/SlopFin/releases/download/01.000.001/SlopFin-01.000.001-experimental.pkg)** | Install through a compatible PS5 package installer | **Experimental.** Contents verified and test installation passed; launch currently stalls at the package mount on the test console |

Both contain SlopFin. Native packaging makes distribution easier; it does not
change video quality or playback performance. Choose one method: both release
formats use PPSA99001. See [native package requirements](docs/NATIVE_FPKG.md).

## Made for the controller

- Browse movies and TV libraries from your own Jellyfin server.
- Search with separate **Movies**, **Series** and **Episodes** rows, so episode
  matches do not bury the show you are looking for.
- Enter addresses and credentials with the **PlayStation system keyboard**,
  with editable fields, visible search text and optional password visibility.
  Search reserves space below its field while the keyboard is open; results
  return when you finish typing.
- Crop your profile picture in a circular preview: move with the D-pad, zoom
  with L1/R1 or the triggers, reset with Triangle and save with Cross.
- Download verified GitHub releases through **Settings → Updates** and install
  them without a computer. Folder builds keep your sign-in and preferences;
  the console needs elfldr running. See [updating](docs/UPDATES.md).
- Resume playback, switch audio/subtitles and continue to the next episode.
- **Skip Intro** with server media segments or validated chapter markers.
  The button remains available over playback controls, with retry feedback
  when a seek fails. Tested on *South Park* and *The Office*.
- Smooth shared focus transitions across browsing, Settings, profiles and the
  administrator dashboard, with stable labels and consistent button surfaces.
- Select subtitle tracks and adjust text size, background and timing.
- Set playback quality limits and control episode autoplay per series.

## Video and audio

SlopFin uses the PS5 **Videodec2 hardware decoder** for H.264 and HEVC. Jellyfin
can copy compatible video, remux it into the MPEG-TS delivery stream, or convert
unsupported media. An audio conversion can leave the video untouched.

| Format or output | SlopFin path | Status / limits |
| --- | --- | --- |
| H.264 / AVC | PS5 hardware decode | Console tested |
| HEVC Main / Main10 | PS5 hardware decode, including tested 4K sources | Decode resolution does not guarantee full-detail 4K presentation |
| SDR / PQ content | SDR rendering and PQ tone mapping | Native ten-bit HDR10 output and live switching remain experimental |
| Dolby Vision sources | Selected HDR10-compatible PQ base layers | No native Dolby Vision HDMI output; incompatible profiles need server conversion |
| AAC / MP3 / AC-3 | Console decoder to PCM | Unsupported rates or output layouts need fallback |
| AC-3 / E-AC-3 / DTS core | Compressed HDMI output | Experimental; receiver/display compatibility and lip sync vary |
| E-AC-3 / DTS core / TrueHD | Optional FFmpeg CPU decoder to 48 kHz S16 PCM | Included in downloads, opt-in; known 48 kHz sources and supported channel layouts only |
| Other media | Jellyfin conversion to a supported stream | Server transcoding resources required |

TrueHD decoding includes tests of real 7.1 input, but the PCM output does not
preserve a 24-bit source or Atmos objects. DTS passthrough carries the core,
not DTS-HD lossless or DTS:X. TrueHD HDMI passthrough is unavailable. HDR10,
HLG, HDR10+ and Dolby Vision are distinct formats; this client does not claim
universal HDR support.

See [compatibility](docs/COMPATIBILITY.md), [audio paths](docs/AUDIO.md),
[software audio setup](docs/SOFTWARE_AUDIO.md) and [HDR evidence](docs/HDR.md)
for tested behavior and output limitations. The software decoder is enabled
for movie trials with the documented console marker; installing this build
alone does not enable that experimental path.

## What makes the playback work

This is a native C++ application built with public homebrew tools. The player
coordinates network delivery, transport parsing, asynchronous hardware decode,
frame presentation and audio output within the console runtime's memory and
threading constraints. Large buffers use flexible memory, and background work
runs away from the render thread.

**Decode stays pipelined.** Three frames can be in flight, avoiding the
serialization that limited difficult HEVC streams. Presentation follows the
source cadence; tested 23.976 fps content uses alternating two/three-refresh
holds on a 59.94 Hz output. Buffering gates pause the picture when delivery
falls behind so audio can refill. They cannot eliminate server or network gaps.

**Audio paths have independent checks.** AC-3, E-AC-3 and DTS burst packing was
compared byte for byte with FFmpeg's SPDIF output. Software-decoder tests cover
fragmented input, channel identity, sample counts and PCM comparisons against
an independent reference. Those checks catch framing and mapping errors;
physical speakers and receivers still need listening tests.

**Intro skipping uses real timing metadata.** Jellyfin Intro segments take
priority over conservative named-chapter fallback. It seeks to the detected
end instead of guessing a fixed duration for every episode. Grouped search
likewise gives movies, series and episodes their own result budgets, keeping
a long episode list from crowding out a matching show.

## First launch

You need a compatible jailbroken PS5 environment, a Jellyfin server and an
account with access to movie or TV libraries. For the folder download:

1. Extract the ZIP. Transfer the **whole `PPSA99001` folder** into the homebrew
   directory used by your loader, commonly `/data/homebrew/`.
2. Let the loader register the title, then launch **SlopFin** from the PS5 menu.
3. Enter your server address and choose Continue. A hostname such as
   `media.example.com` defaults to HTTPS; `https://media.example.com` works too.
   A bare LAN address such as `192.168.1.20` uses Jellyfin's HTTP port 8096.
4. Sign in with Quick Connect or your username and password.

No bundled account, developer server or reporting service is required. Saved
settings and sign-in stay on your console. Reports are local unless you
explicitly configure a receiver and choose to send one.

The [setup guide](docs/GETTING_STARTED.md) covers loader requirements, server
base paths, HTTPS behavior and building from source. For intro detection setup,
read [intro skipping](docs/INTRO_SKIPPING.md).

## What to expect

This is an early public preview. Firmware **8.20** is the main console test
environment; other firmware, loaders, servers and displays need independent
testing. H.264/HEVC playback works on tested files. Native HDR10 and HDMI
compressed audio remain experimental. The preview downloads include the
optional software-audio decoder; server fallback is still available.

Music albums, audiobooks, photos and live TV are not implemented. Self-signed
HTTPS certificates and IPv6 server addresses are not supported. The
[compatibility table](docs/COMPATIBILITY.md) separates implemented behavior
from tested hardware results and remaining limits.

## Build and contribute

```sh
git clone https://github.com/BeLikeBrett/SlopFin.git
cd SlopFin
make doctor
make test
make host
make PS5_CLANG=/usr/bin/clang
```

The build downloads pinned public dependencies; no proprietary publishing SDK
is required. `make host` previews the interface on Linux without video decoding
or a replacement for the PlayStation keyboard. See [Linux preview](docs/HOST_BUILD.md).
For optional software audio and native packages, follow the
[build guide](docs/GETTING_STARTED.md) and [package guide](docs/NATIVE_FPKG.md).

Read [contributing](CONTRIBUTING.md), [architecture](ARCHITECTURE.md) and
[testing](docs/TESTING.md). Bug reports should include firmware, loader,
Jellyfin version and relevant media details; redact credentials and private
addresses from logs. The [documentation index](docs/README.md) links the
technical references and dated validation history.

## License and credits

GPL-3.0-or-later. SlopFin builds on the native-app boilerplate and ProsperoTV
hardware-decode work. Attribution, public SDK dependencies, fonts, artwork and
linked-library licenses are recorded in [NOTICE.md](NOTICE.md).

SlopFin is not affiliated with Jellyfin or Sony.
