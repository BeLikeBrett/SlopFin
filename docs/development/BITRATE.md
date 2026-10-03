# Bitrate and debug measurements

The player now distinguishes these quantities:

- **Source:** Jellyfin's declared media-source average, which may include tracks
  that are not selected for this playback. It is not a current network rate.
- **Limit:** the selected total streaming bitrate ceiling sent to PlaybackInfo.
  Automatic currently uses the client's 120 Mb/s ceiling. Selecting a higher
  ceiling does not upscale or increase the bitrate of a lower-rate source.
- **Media:** compressed selected audio/video payload per media second, measured
  over three-second DTS windows (PTS fallback), excluding MPEG-TS overhead.
  Buffering, pause and slow rendering do not lower this measure. Variable-rate
  scenes and encoder buffering can produce short windows above a nominal limit.
- **Download:** MPEG-TS bytes read per wall-clock second, including delivery
  stalls and decode backpressure. It can differ substantially from Media.

A bitrate change restarts the stream. The Limit updates with the new request;
Media and Download reset and show `measuring...` until a window is available.
Negotiation failure no longer silently replaces the selection with the old
fixed 16 Mb/s transcode. Cold video transcodes get 30 seconds to return their
HTTP headers; established reads retain the normal ten-second timeout. A stop
while waiting for headers can still wait for that socket timeout.

Jellyfin session data can briefly describe the previous stream during a
restart. Delivery diagnostics now check source identity, requested audio codec
and transcode reasons before accepting that status. Delivered video copy versus
transcode determines HDR handling; source PQ metadata must not be applied to
an SDR transcode. Unknown HDR delivery fails explicitly rather than guessing.

The frame trace ring resets on each recording. Earlier repeated `trace.sh`
runs combined old and new samples, including the idle gap between recordings,
which could report a false FPS collapse. Debug remains an overlay and costs
rendering time; measure steady playback with it hidden, then measure overlays
separately. The title logo no longer draws over the debug panel.

For remote tests, `play:<itemId>:<seconds>:<subtitleIndex>:<audioIndex>:<maxBitrate>`
now accepts a bitrate in bits per second. A value of zero selects Automatic.
The same PlaybackRequest and negotiation path is used by the quality menu.


The trace tool reports the actual captured span, not just the requested wait:
the 1,200-sample ring retains about 20 seconds at 60 Hz. A longer request keeps
only the tail. `analyse-trace.py` reports both whole-window throughput and the
rate between picture advances, which excludes edge stalls. Changed-build
recordings are rejected. The debug FPS display expires to zero after a second
without a new picture, and while paused; source FPS remains separate.


## Console retest, 2026-09-13

21 Jump Street, source H.264 1920x816 plus 44.1 kHz AAC stereo:

| Selection | Debug display | Jellyfin delivery |
| --- | --- | --- |
| 1 Mb/s | Limit 1.0 Mb/s; Media about 1.0 Mb/s in captured window | H.264 encoded to 1280x544; AAC resampled; ContainerBitrateExceedsLimit |
| Automatic | Limit Auto (120 Mb/s ceiling); Media 1.4 Mb/s in captured scene | H.264 copy at 1920x816; AAC resampling remains necessary |

The source average was about 2 Mb/s. Automatic's 120 Mb/s ceiling is not an
instruction to inflate that source to 120 Mb/s. Independent server session
checks agreed with the displayed copy/transcode paths after both restarts.
These remote requests exercise the same PlaybackRequest/negotiation path as
the quality menu; they are not a separate visual menu-navigation test.

## CPU audio timing counters (2026-09-13)

New trace fields `software_frames`, `software_bytes`, `software_work_us` and
`software_queue_us` are cumulative per playback. The analyzer reports window
deltas. Work is producer elapsed time for parsing/decoding/conversion excluding
its PCM queue callback; callback time includes backpressure. Neither measures
process CPU utilization. Use these together with live socket-wait stage,
buffer occupancy and underruns when comparing TrueHD to server audio fallback.
