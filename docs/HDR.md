# Experimental Native HDR10

Current implementation is experimental. See [compatibility](COMPATIBILITY.md),
[Dolby Vision/base-layer investigation](DOLBY_VISION.md), and the
[GPT plan](../PLAN.md). Measurements below are tied to their tested builds.

## Verified on Firmware 8.20

- `sceVideoOutRegisterBuffers2` accepts native format `0x8100070422000000`
  with the title HDR capability bit set in `sce_sys/param.json` (bit 29,
  decimal 536870912). The same format was refused without that title flag.
- The packed surface is A2B10G10R10: R at bit 0, G at bit 10, B at bit 20.
- HEVC Main10 decoder output is low-aligned ten-bit YCbCr. The player preserves
  PQ during BT.2020 NCL matrix conversion instead of applying its SDR tone map.
- UI sRGB colors are linearized, transformed from BT.709 to BT.2020 primaries,
  scaled to 203-nit reference white, and composited in linear light.
- A real HEVC Main10 SDR title played into the HDR surface; a full-resolution
  diagnostic crop was coherent. This is not a PQ-source or HDMI validation.
  The initial run was severely slow: draw time 117 ms median, with the user
  reporting pixelation and choppiness. HDR was disabled immediately afterwards.
- SDR-to-HDR conversion has since moved from repeated 4K display redraws to
  source-resolution decoded frames. Video enlargement now uses bilinear
  filtering. HDR remains opt-in pending performance revalidation.
- Live `sceVideoOutSubmitChangeBufferAttribute2(handle, 0, attribute, nullptr)`
  succeeds in both directions. Requests are acknowledged by the render thread
  before the playback worker produces frames in the new format.
- The Amazing Spider-Man and Pacific Rim now play original copied Main10 PQ
  video through the HDR compositor. Pacific Rim needed six decoded-picture
  buffers; its SPS declares `sps_max_dec_pic_buffering_minus1 = 5`. The former
  four-buffer configuration failed with `0x811d0302`, then `0x811d0303`.
- Preserve `SubtitleStreamIndex=-1` in the generated stream URL. Otherwise the
  stream endpoint can select an image subtitle by default and re-encode video
  despite negotiation having selected subtitles off.

`make test-playback` covers PQ reference levels and round trips, limited-range
YCbCr matrixing, packed channel order, reference white, and alpha blending.

## Opt-In Only

Create `/data/slopfin-hdr-auto` over FTP, then restart the app. It starts in SDR,
requests HDR for PQ titles, and requests SDR on playback exit or SDR playback.
The render thread switches the registered buffer attributes before drawing.
Failed HDR transitions retain SDR and use the existing tone-map path.
Remove the marker and restart to disable experimental HDR transitions.

The older `/data/slopfin-hdr` format marker is only a forced-format diagnostic;
leave it absent during automatic-mode testing. HDMI capability negotiation is
not yet implemented. September 12 changes use confirmed server video
copy/transcode status instead of retaining the source HDR flag for an SDR
transcode. Bitstream/HDMI metadata validation remains a separate requirement.

`tools/shot.sh` understands HDR captures. Its PNG is an SDR diagnostic preview,
with highlights above reference white clipped, not a calibrated HDR screenshot.
HDR captures are point-sampled when shrunk to preserve packed pixel codes.

## Earlier performance result (before the September 12 optimizations)

The earlier Pacific Rim run after the six-buffer correction: 307 accepted pictures,
zero decode failures or concealments. The HDR compositor remained too slow:
6.96 displayed frames/s, draw median 118 ms, decode median 49.6 ms, conversion
median 18.9 ms. These are not acceptable playback results. Automatic HDR testing
was disabled afterwards; the successful return to SDR and native AC-3 playback
were rechecked. Do not describe this as completed HDR support.

## Still required

- Validate the actual HDMI transfer/colorimetry metadata and TV HDR mode.
- Validate peak brightness, black level, and gamut against measured patterns.
- Recover and validate mastering-display and content-light metadata submission.
- Add display capability negotiation and validate transition behavior at HDMI.
- Measure and improve 4K rendering performance, including UI overlays.
- Preserve full 4K detail: conversion still reduces 4K source frames to 1920
  pixels wide before scaling them into the 4K framebuffer. Hardware decode at
  4K does not yet mean a full-resolution 4K presentation pipeline.
- HLG, Dolby Vision, and HDR10+ dynamic metadata are not implemented. The
  playback profile requests SDR or HDR10 only; HLG must not enter the PQ path.

The title flag reference is
[PS5 param.json attributes](https://gist.github.com/andshrew/5ef86db5c10e65198a0f01c7795ea478).
Runtime acceptance was independently checked on this console. Proprietary
platform binaries used for local ABI inspection are not included in this repo.

## September 12 GPT continuation measurements

On the same Pacific Rim starting scene at a 1920x1080 surface, an initial
recording measured 13.51 displayed fps and 19.18 ms median conversion. The
integer HDR matrix version measured 19.80 fps and 3.47 ms median conversion.
These recordings include different amounts of initial overlay activity and
are development observations, not a controlled claim of universal FPS gain.
Host tests compare the matrix against the floating-point BT.2020 reference
within one ten-bit code. HDR remains short of an unconditional smoothness claim.

The 4 Mb/s quality test successfully switched to a server tone-mapped SDR
transcode after allowing cold video-transcode startup time. The debug panel
reported the changed limit and SDR delivery. Native DV output is not implied.
Player/display cadence work continued in another agent after these measurements;
new performance results must identify that later build separately.

## Opus 5 addendum (2026-09-15): following the PS5's HDR setting

*Opus 5's section; the GPT sections above are unchanged.*

**The marker is gone.** `/data/slopfin-hdr-auto` is no longer read. An HDR title
gets HDR10 buffers when the console's Settings > Screen and Video > HDR is "On
When Supported", and SDR with SlopFin's tone map when it is "Off". The setting is
read again at each title, so changing it needs no app restart.
`/data/slopfin-sdr-only` forces SDR for testing.

**How the setting is read.** `sceVideoOutGetOutputStatus` byte 4 is 2 for On When
Supported and 1 for Off. Found by writing the setting's registry entry
(`VIDEOOUT_hdr`, key 169148416) from a payload with SlopFin running and diffing
display reports: byte 4 went 2 -> 1 -> 2 with writes of 0, 1, 0, and nothing else
moved. It does not follow SlopFin's own buffers; a report during HDR10 playback
matched an SDR one. The tools are in `tools/hdr-setting`.

**Measured on the console, Backrooms (HEVC, DV profile 8 HDR10 base, 4K),
current build:**

| PS5 setting | Output | Decoded | Shown | Dropped | Over 20 ms |
| --- | --- | --- | --- | --- | --- |
| On When Supported | HDR10 (`change to HDR10 result=0`) | 23.96/s | 24.01/s | 0 in 30 s | 3 of 1,199 |
| Off | SDR, tone mapped | 24.04/s | 23.98/s | 0 in 15 s | 12 of 959 |

The HDR10 compositor no longer costs what it did on 2026-09-12 (6.96-19.8 fps).
A seek now keeps the current HDR scanout mode instead of requesting SDR during
stream teardown and HDR again during reopen. The replacement stream still
requests its negotiated mode; if that mode actually differs, the normal output
switch happens. This avoids presenting an ordinary skip to the TV as a fresh
HDR session.
The overlay reads "HDR source -> HDR10 output | PS5 HDR: On When Supported" or
"HDR source -> SDR output | tone mapped by SlopFin | PS5 HDR setting is Off".

**Still not known: whether the TV switches.** Nothing readable on this firmware
reports the HDMI signal. libSceAvSetting's monitor queries answer `0x802a0002`
from a payload and `sceAvSettingInit` never returns. A Remote Play stream asked
for HDR is tagged PQ/BT.2020 during SlopFin's SDR home screen as well as during
HDR10 playback, so it does not show the link either. The TV's own indicator
remains the authority.

Other registry values read on Brett's console, for reference: Deep Colour
(`color_depth`) 0 against a factory 1, resolution (`mode`) 4 against a factory
19, and Adjust HDR calibration `max_tml` 767, `max_ff_tml` 719, `min_tml` 70,
which `sceSystemServiceGetHdrToneMapLuminance` reports as 981, 637 and 0.125
nits.
