# Portable playback defaults — GPT design

Updated 2026-09-13. This is an implementation plan, not a claim that these
settings or capability detection already ship.

## User requirements and evidence

Brett wants SlopFin to work on other PS5s and on future TVs/audio devices,
without tuning defaults only for his current installation. Provide persistent
Auto defaults and accessible playback overrides alongside quality controls.
His current setup is a PS5 over Wi-Fi, local Jellyfin, and TCL 55Q51K built-in
speakers. His reported 200 Mb/s speed test does not measure each media socket's
worst delivery gap. Avatar HDR was confirmed by the TV/user; larger buffers
made motion much smoother. Silo AAC 5.1 crackling appears gone; 21 Jump Street
looks and sounds good. These observations do not validate every output device.
Home artwork recovered before the defensive retry fix; cause remains unknown.

## Buffering

Proposed controls: **Auto** (default), **Fast start**, **More buffering**.
Describe the tradeoff as quicker starts versus more protection against delivery
gaps, without assuming Ethernet is always reliable or Wi-Fi always unreliable.

These controls must select a media-ahead target, not sleep for a fixed wall-clock
time. Auto should start with a modest target, raise the resume target after
repeated starvation, and reduce it cautiously after sustained healthy delivery.
Choose numeric thresholds from console measurements, not the current TV setup.

Implementation gates:

- Coordinate audio and video startup/rebuffering as one playback state. Freeze
  the media clock and retain the current picture while collecting enough data;
  keep the output port alive without consuming queued PCM. User pause remains
  distinct from buffering. Do not count intentional waits as dropped frames.
- Readiness must consider playable audio and video timestamps, including
  video-only media. PCM occupancy alone is insufficient. Avoid deadlock when
  audio fills before the transport reaches the desired video timestamp.
- Bound memory, startup/rebuffer deadlines and adaptive targets. Handle EOF,
  short clips, allocation failure, seek, track changes and cancellation.
- Keep capacity an internal budget shared with decoded surfaces and artwork.
  A higher cap is not harmless: allocations compete, even on similar consoles.
  Current capacities are eight seconds of PCM and 64 MiB compressed video;
  neither means playback currently waits eight seconds before starting.
- Debug should show preset, effective target, buffered duration, buffering state
  and reason, plus separate delivery gaps, underruns and decode/render failures.
- A buffer can absorb temporary gaps; sustained delivery below consumption
  requires lower bitrate or another delivery fix. Do not silently lower a user's
  explicit quality setting or diagnose every stall as a network problem.

Validate burst gaps, sustained slow delivery, EOF and cancellation in deterministic
logic tests, then startup/seek and clean console traces at several bitrates.
Compare start latency, stall duration, audio continuity and memory headroom.

## HDR and Dolby Vision

Proposed controls: **Auto** (default), **SDR**, and an explicitly experimental
**Try HDR10** override until detection is validated. Keep **Dolby Vision HDR10
base-layer playback** separate from real Dolby Vision HDMI output.

Current `/data/slopfin-hdr-auto` follows source HDR and enables an experimental
surface switch. It does **not** establish display support. `gfx.cpp` dumps
`sceVideoOutGetDeviceCapabilityInfo_` into an opaque diagnostic block; there is
no validated capability parser or connection-change handling. Successful buffer
registration is not proof of display capability or correct HDMI metadata.

*Opus 5 note, 2026-09-15:* the marker above is no longer read. SlopFin now follows
the PS5's own HDR setting, read from `sceVideoOutGetOutputStatus`, which covers
the "PS5 output settings" input below; display capability and the HDMI link are
still unvalidated. See the Opus 5 addendum in [HDR](HDR.md).

Auto must combine source transfer/profile, decoder support, PS5 output settings,
and reported capabilities of the connected HDMI chain. Model capabilities as
supported, unsupported or unknown; unknown must never mean supported. Verify
API structure and meanings before interpreting bits. Recheck after a connection
change or next playback, and renegotiate delivery when the output policy changes.

Use HDR10 only when the full path is validated. Otherwise choose an explicit
SDR fallback and show its reason. Preserve the tested HDR trial as an override
while detection is unfinished. Do not treat Brett's confirmation as a stored
capability for every future TV. Overrides cannot add unsupported hardware modes.

For DV sources, validate the actual base layer independently of TV DV support.
A DV-capable TV does not make this PS5 client able to transmit DV. Pure DV must
not enter the PQ/HDR10 path. Keep originals untouched and retain server fallback.

Acceptance requires HDR and SDR displays, unavailable/failed capability queries,
output-switch failure, HDR-to-SDR quality changes and HDMI reconnection tests.
Where hardware is unavailable, record the missing validation explicitly.

## Audio output

Native decode and HDMI output are separate decisions. Auto should combine the
selected source with validated system/output capabilities; support stereo and
surround PCM overrides with clear routing. Do not infer a receiver from a 5.1
file, or use Brett's TV speakers as a global stereo limit. Unknown capabilities
need a documented fallback rather than an invented channel map.

Keep source codec/channels, console decoder, resampling/downmix, delivered PCM
layout and server conversion visible separately in debug. Software TrueHD/E-AC-3/
DTS probes are promising but not normal-playback support. Validate downmix levels,
clipping, layout changes and device changes before Auto claims broad compatibility.
PCM decode alone does not establish compressed passthrough or Atmos/DTS:X output.

---

*Opus 5 note (2026-09-14), appended; the GPT agent's text above is unchanged:*
the "Current capacities" line predates the buffers being doubled to sixteen
seconds of PCM and 128 MiB of compressed video. See
[architecture](../ARCHITECTURE.md), Opus 5 changes.
