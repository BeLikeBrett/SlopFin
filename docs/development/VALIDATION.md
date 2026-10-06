# Validation record — 2026-10-05

This separates new checks from existing console evidence. Firmware 8.20 remains
the main console setup; this record does not certify other firmware or HDMI devices.

## New checks for 01.000.003

| Check | Result | What it establishes |
| --- | --- | --- |
| `make test` | Passed | Settings save/reload and request preferences; TrueHD CPU decoding takes priority over server conversion when both audio settings are on; delivery descriptions distinguish CPU, platform and receiver decoding; Square skips with/without timeline and Cross does not; open panels suppress skipping; existing playback/update regressions. |
| `bash tools/test-software-audio.sh` | Passed | TrueHD 5.1, E-AC-3 stereo/5.1 and DTS 5.1 decoded PCM matched the independent reference with RMS error **0.0000**. Fragmented input, TS, EOF, cancellation and reopening passed. This is a Linux decoder check, not PS5 HDMI/speaker validation. |
| Synthetic settings/intro render captures | Reviewed | Shared renderer has readable settings, menu scrolling and one Square intro prompt; no personal account, server or media data used. |
| `make host`, PS5 build with `SOFTWARE_AUDIO=1`, `make lint` | Passed locally | Signed native containers have valid integrity and zero static FSELF errors. Release workflow also verifies the build. No Linux check establishes console output behaviour. |
| Final console updater download/install/restart | Pending | FTP was reachable but elfldr on port 9021 refused connections. A previous updater result file alone does not prove this build passed. |

## Earlier console evidence

- H.264 / HEVC Main/Main10 and selected 4K sources: [video pipeline](VIDEO_DECODE.md).
- Dolby Vision Profile 7 / 8.1 compatible base-layer decode:
  [base-layer evidence](DOLBY_VISION.md). Native Dolby Vision output is unsupported.
- HDR surface registration and both output transitions on firmware 8.20:
  [HDR evidence](HDR.md). HDMI signalling/brightness still need physical verification.
- CPU audio fixtures and limited movie/soak trials:
  [software audio](SOFTWARE_AUDIO.md). `tests/test_delivery_summary.cpp` records the
  Avatar HEVC/TrueHD-copy console session used for its fixtures.
- AC-3 channel ordering and compressed-audio byte comparison:
  [audio](AUDIO.md). Byte-correct HDMI packing does not prove audible receiver output.
- Intro skips on two episodes, including with controls visible:
  [Intro checks](../INTRO_SKIPPING.md). The new consistent Square routing is tested
  separately in `tests/test_player_controls.cpp`.

A known decoding result can be stated as tested while receiver/speaker/display
output remains pending. Avoid a blanket “experimental” label that hides that distinction.
