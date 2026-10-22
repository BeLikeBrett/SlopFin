# Audio route and evidence ledger

Which path a sound track takes is decided by one function,
`audio_route::choose()` (`src/audio_route.hpp`). The server negotiation, the
direct-play check and the audio output all call it, so they cannot disagree.
`tests/test_audio_route.cpp` holds every row below as a test, against a real
TV's capability list.

## The table

The first row that applies wins. Every row except server conversion needs
48 kHz audio and no more channels than the limit.

| Track | 1st | 2nd | Last |
| --- | --- | --- | --- |
| AAC, MP3 (stereo) | PS5 decoder | | server → AAC |
| AC-3 (to 5.1) | Passthrough, if the TV lists AC-3 | PS5 decoder | server |
| E-AC-3, including Atmos | Passthrough, if the TV lists E-AC-3; the only path that keeps Atmos | PS5 CPU decodes to 7.1 | server |
| DTS, DTS-HD | Passthrough of the DTS core, if the TV lists DTS or DTS-HD | PS5 CPU, core profile only | server |
| TrueHD, including Atmos | PS5 CPU decodes to 7.1 PCM (16-bit, no Atmos objects) | | server |
| FLAC, Opus, PCM, MP2, other | | | server |

Server conversion asks for:

- **E-AC-3** when passthrough is on and the TV lists it;
- otherwise **AC-3** for surround, which the console decodes;
- **AAC** for stereo.

Dolby formats stop at 5.1. A format chosen in the Audio panel is always what
the server converts to.

**Settings → Audio/Video → Surround sound → Decode on PS5** removes the
passthrough rows, for receivers that take a format but play it badly; nothing
on the console can detect that. A failure removes its row for the rest of that title, and the title restarts
where it stopped (`player::recover()`):

- an HDMI port that refuses passthrough;
- the CPU decoder failing;
- the Dolby Vision base layer failing.

## What the TV accepts

`sceAudioOutExGetMonitorInfo(0, 0, buffer, 0x400)` returns the console's own
parse of the HDMI sink's audio list (CTA-861 short audio descriptors).
`audio_route::parse_monitor_info()` reads it:

- a count at `0x98`;
- 8-byte entries from `0xa0`: format, channels, third byte (16-bit), rate
  mask (32-bit).

Anything out of range makes the list unknown rather than half-read. With no
list, the passthrough setting is trusted, as it was before the list was read.

The layout comes from one capture: Settings → Diagnostics → Check display &
sound, firmware 8.20, a sink naming itself "Beyond TV", 2026-10-22. It is
the test fixture `tests/audio_route_beyond_tv.inc`. Its eight entries:

| Format | Channels | Third byte | Rates |
| --- | --- | --- | --- |
| LPCM | 2 | 16/20/24-bit | 32–48 kHz |
| AC-3 | 6 | 640 kbps | 32–48 kHz |
| E-AC-3 | 8 | Atmos | 32–48 kHz |
| MAT (TrueHD) | 8 | | 48 kHz |
| DTS | 6 | 1536 kbps | 32–48 kHz |
| DTS-HD | 8 | (two entries) | 44.1–192 kHz |
| AAC | 6 | 320 kbps | 32–48 kHz |

The AC-3 and DTS limits are exactly those formats' maximum bitrates, which
fixes the field order. A second display would confirm the layout on other
hardware.

## Evidence ledger

Each claim is marked by how it is known.

| Claim | Status | How |
| --- | --- | --- |
| CPU decoders (E-AC-3, DTS core, TrueHD) produce correct PCM | **Verified offline** | Channel identity and sample counts against an independent reference, on synthetic fixtures; output is 16-bit; [software audio](SOFTWARE_AUDIO.md) |
| Passthrough packing (IEC 61937) | **Proven** | Compared byte for byte with FFmpeg's SPDIF muxer; [audio evidence](AUDIO.md) |
| Passthrough heard on a TV | **Observed** | AC-3 5.1, E-AC-3 7.1 and DTS 5.1 test tones, 2026-09-15 |
| The audio route table | **Proven** | Every row a unit test (`test_audio_route.cpp`) |
| The TV capability layout | **Verified, one sample** | Field limits match CTA-861 exactly; one display captured |
| Routes taken in practice | **Observed** | 2026-10-22, firmware 8.20, a real library, with the list read live (8 formats); see below |
| Headset: the list becomes PCM only, and the route follows | **Observed** | Same console with a headset plugged in, 2026-10-22: one entry, 8-channel PCM at 48 kHz (second test fixture). An E-AC-3 title that went to the receiver that morning was decoded on the PS5, with no underruns |
| Passthrough refused → automatic fallback | **Not yet observed** | Code path tested only by construction: no TV here refuses |
| Dolby Vision stripping, Profiles 8.1 and 7 | **Proven** | NAL-for-NAL identical to FFmpeg `filter_units=remove_types=62\|63`; decodes pixel-identical to the untouched stream (real 8-second samples) |
| Profile 5 and incompatible DV refused | **Observed** | Profile 5 title converted by the server (`VideoRangeTypeNotSupported`) |
| MKV/MP4 direct play | **Proven + observed** | [direct play](DIRECT_PLAY.md) |

Routes observed on the console, 2026-10-22, each with 0 dropped frames unless
noted:

| Title's track | Route | Result |
| --- | --- | --- |
| DTS core, H.264 MKV | Passthrough | 25 fps, no underruns |
| DTS-HD MA, HEVC 10-bit MKV | Passthrough (core) | 24 fps, no underruns |
| E-AC-3, MP4 direct play | Passthrough | 24 fps, no underruns |
| TrueHD 7.1, 4K DV Profile 7 remux | PS5 CPU, 8 channels | No underruns; video 18.5 fps (connection limit, below) |
| Opus 5.1 | Server → E-AC-3 → passthrough | 24 fps, no underruns |
| AAC 44.1 kHz, MP4 | Server resamples → PS5 decoder | 24 fps, no underruns |
| DV Profile 8.1, 4K HEVC | Direct, HDR10 | 24 fps, no underruns |

### Known limit: high-bitrate remuxes over the internet route

Two 4K remuxes decoded at 16–19 fps with audio starving. The same happened
through the server's remux, so it is not direct play: reads averaged about
50 Mbit/s. That server is reached through a Cloudflare tunnel, even from the
same network, and its LAN listener is plain HTTP. Connecting to the LAN
address avoids the tunnel.
