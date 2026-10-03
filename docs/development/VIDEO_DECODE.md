# Hardware video pipeline

SlopFin uses Videodec2 for H.264 and HEVC, with MPEG-TS transport supplied by
Jellyfin. H.264 and HEVC Main/Main10, including selected 4K sources, have been
played on the test console. Full-detail 4K presentation is a separate rendering
and output question. [Compatibility](../COMPATIBILITY.md).

## Keep decode pipelined

The decoder permits asynchronous work. Pipeline depth is **three**: serializing
submit/wait operations to depth one caused difficult HEVC streams to fall below
source cadence. A frame slot is not available to the renderer while recorded
as owned by the decoder. Preserve that ownership through seek and shutdown.

Decoder buffers must be sized from the stream's SPS requirements. H.264 4K
configuration and HEVC decoded-picture-buffer requirements are not interchangeable.
Direct decoder memory uses **type 12**; mapping protection values are not memory
types. Large compressed queues and converted surfaces use flexible memory.

## Presentation and buffering

Presentation follows source cadence. At 59.94 Hz, tested 23.976 fps material uses
alternating two/three-refresh holds. The player coordinates the media clock and
AudioOut queue when paused, seeking or intentionally buffering.

A frozen picture during refill is not automatically a decoder drop. Keep
server socket gaps, decode failures/concealments, conversion time, presentation
holds and real audio underruns distinct in measurements.

## Validate changes

Use the same source, timestamp, audio track, subtitle selection and output mode
for comparisons. Record clean playback with `tools/trace.sh` and analyze it with
`tools/analyse-trace.py`; do not capture screenshots or audio during a frame-time
benchmark. [Testing](TESTING.md) covers the required media/output matrix.
