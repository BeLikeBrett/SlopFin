# SlopFin architecture

Updated 2026-10-03. The app uses a custom C++ interface, VideoOut framebuffer,
native hardware video decode, platform audio decode and a PCM output sink.
See [compatibility](docs/COMPATIBILITY.md) for validated scope rather than
assuming every platform capability is integrated.

## Pipeline

1. `jellyfin.cpp` sends a device profile with container, video/audio codec,
   sample-rate, channel, video-range and bitrate constraints. It negotiates
   source-compatible codec ordering to preserve video where possible.
2. `http::Stream` reads MPEG-TS and removes HTTP chunk framing. The server can
   remux, transcode only audio, or transcode video. Source metadata and delivered
   metadata are different; live copy/transcode status controls HDR handling.
3. `tsdemux` reassembles PES access units and their timestamps. Audio compressed
   frames may cross PES boundaries and need codec-specific framing. A transport
   producer feeds audio and queues compressed video in a bounded 128 MiB flexible-
   memory ring; a full presentation pool cannot block access to the next audio PES.
4. `player` submits H.264/HEVC to `sceVideodec2`. HEVC Main10 output on the tested
   console uses low-aligned 10-bit samples in 16-bit words. A CPU conversion
   prepares SDR or packed PQ/BT.2020 pixels in a bounded presentation pool.
5. `audio` decodes AAC, MP3 or AC-3 and maps PCM to the stereo/eight-channel sink.
   The sink clock drives playback synchronization. It currently runs at 48 kHz;
   other source rates require server resampling until a local resampler exists.
   A sixteen-second PCM reservoir waits for output space instead of discarding
   samples, and cancellation releases blocked producers before resource teardown.
6. The render thread selects frames and draws video, subtitles and controls.
   `gfx` scales/composites into staging, converts to tiled scanout order and
   waits for actual flip completion before reusing a display buffer. Clean
   unchanged pictures can reuse already drawn pixels.

Current 4K decode commonly reduces to a 1920-wide converted picture. A 4K
hardware decoder or HDMI output does not prove full-detail 4K presentation.
HDR10 surfaces register with the tested title flag/format, but HDMI signalling,
TV capability negotiation and brightness validation remain separate work.

## Two targets, one renderer

The console build is the product. `make host` builds the same interface for
Linux: `gfx`, `app`, `text`, `icons`, `images`, `ime`, `pad`, `http`,
`jellyfin` and `config` are compiled unmodified, and `host/` implements the PS5
C ABI they already call on top of SDL2 and POSIX. The tiled copy and the
ARGB-to-ABGR scan-out conversion run on both, so a capture taken on Linux is
what the console would send over HDMI. `player` and `audio` are console-only;
`host/host_player.cpp` stands in so the playback overlay is drivable.
See [docs/HOST_BUILD.md](docs/HOST_BUILD.md).

## Ownership

| Component | Responsibility |
| --- | --- |
| `main`, `app`, `gfx`, `text`, `pad` | Render/input loop, views, drawing, glyphs and controller handling |
| `motion` | One spring and one set of curves, so every screen shares a sense of weight |
| `ime` | PS5 system keyboard lifecycle and text submission |
| `icons` | Interface glyphs and the DualSense face buttons, as signed distance fields |
| App data worker | Library, detail, search and sign-in requests |
| Artwork workers | HTTP image fetch/decode and cache population |
| Transport worker | Stream read, demux, native audio decode/submission, bounded compressed-video queue |
| Playback worker | Stream setup, native video decode, conversion and transport lifetime |
| Audio output worker | Paced output, safe sink changes and PCM consumption |
| Reporter/subtitle workers | Session progress and external text subtitle retrieval |
| `telemetry`, `trace` | Explicitly requested frame recordings and diagnostic messages |

Rendering should avoid blocking I/O. Existing remote test commands and stream
restarts still contain blocking operations; they are limitations to improve,
not evidence of an entirely asynchronous UI. Expensive captures are explicitly
requested, never periodic. Host-tested pure logic includes timestamps, framing,
HDR math, scaling, URL handling and bitrate measurement.

## Platform constraints

- Use `bigalloc`/flexible memory for large buffers; the runtime heap is limited.
- Decoder direct-memory type is 12. Protection bits are not memory types.
- SDR drawing uses ARGB while scanout needs the verified ABGR convention.
  HDR10 is packed A2B10G10R10. Comparing two app buffers cannot validate HDMI.
- A vblank is not a completed flip; do not overwrite a displayed buffer.
- Do not close an audio port underneath the thread blocked in its output call.
- Condition-based worker synchronization is preferable to short-sleep spinning.
- Return from `main` is unsafe on this runtime; failure paths park explicitly.
- Detailed ABI findings and investigation rules live in [CLAUDE.md](CLAUDE.md).

---

## Playback and refresh details

Video decode uses pipeline depth 3 to improve serial HEVC throughput. A returned
picture is read only from a frame slot the decoder owns; see [video decode](docs/VIDEO_DECODE.md).

Home refreshes Continue Watching and Next Up after playback and every 30 seconds.
Partial refreshes preserve library rows and artwork warming. Search sections are
rebuilt only when result data changes, rather than on every rendered frame.

Optional CPU decoders and experimental HDMI compressed output are separate
from native audio decoding. Their integration and validation limits are recorded
in [audio](docs/AUDIO.md) and [software audio](docs/SOFTWARE_AUDIO.md).
