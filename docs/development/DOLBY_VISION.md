# Dolby Vision base-layer fallback

SlopFin does not output native Dolby Vision. An experimental option admits
selected **HDR10-compatible PQ base layers** for HEVC decoding. Enable it in **Settings → Audio & video → Dolby Vision HDR10 fallback**
before playback; turn it off to restore normal negotiation.

The source must pass profile, base-layer compatibility, transfer, primaries
and bit-depth checks. The selected media version remains pinned through the
second negotiation. Before hardware decode, the filter removes DV RPU,
enhancement-layer and nonzero-layer NAL units while retaining layer-zero HEVC
and static HDR SEI. Transcoded SDR delivery clears the base-layer/HDR flags.

Profile names alone are insufficient evidence. Pure-DV or incompatible sources
need server conversion; this path must not label them as ordinary PQ HDR10.
See Dolby's [profile definitions](https://professionalsupport.dolby.com/s/article/What-is-Dolby-Vision-Profile).

Selected Profile 7 and 8.1 PQ base-layer files were exercised on the test setup.
That is a limited decode result, not a claim of broad Dolby Vision compatibility.
[HDR output](HDR.md) has its own HDMI and physical-display validation requirements.
