# Dolby Vision and HDR10 fallback — GPT investigation

## Current finding: Avatar: Fire and Ash

Jellyfin reports this file as:

| Field | Value |
| --- | --- |
| VideoRangeType | DOVIWithEL |
| DvProfile / DvLevel | 7 / 6 |
| DvBlSignalCompatibilityId | 6 |
| Display title | Dolby Vision Profile 7.6 (HDR10) |
| Codec/depth | HEVC Main10, 10 bits |
| Transfer/primaries/matrix | SMPTE 2084 PQ / BT.2020 / BT.2020 NCL |
| Resolution | 3840x2160 |

This is a candidate for HDR10 **base-layer** playback. Profile 7 has an
HDR10-compatible base layer; it does not inherently require converting the
library file to Profile 8.1 just to obtain HDR10. See Dolby's
[Profile 7/HDR10 compatibility explanation](https://professionalsupport.dolby.com/s/question/0D54u00009VsMJKCA3/can-a-tv-that-doesnt-say-it-has-dolby-vision-use-it-anyway-like-mode-or-something-on-a-samsung-tv?language=en_US)
and [profile/level definitions](https://professionalsupport.dolby.com/s/article/What-is-Dolby-Vision-Profile).

A metadata declaration alone is insufficient: the app must correctly handle
or discard enhancement-layer/RPU data, preserve the base-layer transfer and
static HDR metadata, and demonstrate correct decode/output on the console.
The integration is now available behind `/data/slopfin-dv-hdr10`. It validates
profile/compatibility/PQ/primaries/depth, pins the selected media version, then
admits only that source's compatible range in a second negotiation. Before
hardware decode it removes DV RPU, enhancement-layer and nonzero-layer NAL units
while retaining layer-zero HEVC and static HDR SEI. Transcoded video clears the
base-layer/HDR flags and uses the delivered SDR format.

Avatar's Profile 7 source has now played with `Video hevc copy (DV HDR10 base)`
and AC-3 copy. HDR10 mode registration succeeded and a diagnostic capture shows
a coherent image. The original file was not modified. The first live 16-second
trace had zero decode failures but 2.837 seconds of audio underruns during
socket stalls, averaging 20 shown FPS. This establishes a working base-layer
decode trial, not smooth playback across the library. Brett subsequently
confirmed that his TCL 55Q51K reports HDR and the lighting appears correct,
while visibly dropping frames. This is TV/user confirmation, not a calibrated
HDMI/color measurement.

Profile 8.1/compatibility 1 also passes the console base-layer trial: “Chapter
10: The Dark Lord” (`DOVIWithHDR10Plus`, PQ/BT.2020/Main10) negotiates HEVC
copy, registers HDR10 output and renders a coherent 3840x2160 decoded image.
Its 15.50-second trace shows 372 pictures (24.00 FPS), no drops, decode failures
or audio underruns. E-AC-3 is still converted by the server to AAC in this
normal playback path. Dynamic HDR10+ and DV HDMI metadata are not claimed.

## Why the current app reports SDR

Without the opt-in marker, the HEVC profile permits `SDR|HDR10`. A source labeled `DOVIWithEL`
fails that constraint, so Jellyfin may transcode video and tone-map to SDR.
A filter such as `tonemap_cuda=...p=bt709:t=bt709:m=bt709...` confirms an SDR
transcode. Reporting SDR for those delivered bytes is correct. The fix must
change compatible delivery and decoding, not merely relabel the display.

Even a supported HDR10 source can be tone-mapped when a lower bitrate ceiling,
unsupported video codec or subtitle burn-in requires video encoding. See
[Jellyfin transcoding](https://jellyfin.org/docs/general/post-install/transcoding/).

## Why not advertise every range

| Range/source | Intended handling |
| --- | --- |
| HDR10 PQ | Existing native decode/PQ path or explicit SDR output |
| DOVIWithHDR10 / Profile 8.1 | Validate base-layer decode and DV metadata removal, then admit as HDR10 fallback |
| DOVIWithEL / Profile 7 | Validate HDR10-compatible base-layer extraction, including enhanced NALs, before admitting |
| Pure DOVI / Profile 5 | No ordinary HDR10-compatible base layer; keep a validated server fallback |
| HLG / DOVIWithHLG | Separate HLG transfer handling required; never reinterpret as PQ |
| DOVIWithSDR | SDR-compatible fallback is SDR, not HDR10 |
| HDR10Plus variants | Static HDR10 fallback and dynamic HDR10+ metadata support are separate claims |

`IsRequired: false` weakens handling of missing metadata; it does not add a
Dolby Vision decoder. The proposed broad range list would overstate current
SlopFin support. Prefer a narrow, tested profile and per-file compatibility.

## Next implementation gate

1. Read DV profile/base-layer compatibility and color-transfer fields into the
   playback plan, independently of the source's generic HDR label.
2. Test Profile 8.1 (single layer) and Profile 7 (enhancement layer) fixtures.
   Keep layer-zero HEVC picture/parameter sets and HDR static metadata; exclude
   DV-only RPU/enhancement data through a validated Annex-B filter.
3. Compare a reference base-layer decode against the app, then verify actual
   TV HDR mode and sustained cadence. Do not alter library originals.
4. Only then include corresponding DV enum values in the negotiated profile
   and label playback `Dolby Vision source -> HDR10 base-layer output`.
5. Test bitrate-triggered SDR conversion and back, without stale source flags.

[dovi_tool](https://github.com/quietvoid/dovi_tool) can separate layers, remove
DV data or convert compatible metadata. These are useful fixture/reference
operations, not proof of native DV HDMI output or a universal library rewrite
requirement.

## Native Dolby Vision over HDMI

This is a separate long-term research item: output format, HDMI capability
signalling, metadata transport, firmware API access and licensing all need
investigation. Neither HDR10 base-layer playback nor stripping metadata
produces Dolby Vision. No bypass or “trick” is promised without hardware proof.

---

### Opus 5 note (2026-09-14)

*Appended by Opus 5; the record above is the GPT agent's and is unchanged.*

The frame drops Brett saw during the base-layer trials predate two later
changes: server-side delivery fixes and the decoder moving to pipeline depth 3.
On the current build, Backrooms (2026) -- Profile 7 with an enhancement layer,
94 Mbps, base layer extracted by this path -- plays at 23.95 fps with no drops,
decode failures or audio underruns, and Avatar at 24.02 fps
([video decode](VIDEO_DECODE.md)).
