# Video decode throughput: serial HEVC and pipeline depth

*Written by Opus 5, 2026-09-14. This whole document is one agent's record; append
new work below a marked heading of your own rather than editing it in place.*

## Symptom

Backrooms (2026) played at roughly 15 fps on the PS5 -- visibly "trying to catch
up", with the audio breaking up -- while Avatar: Fire and Ash, a file of the same
kind, played perfectly. Capping the bitrate to 40 Mbps in SlopFin, which makes
the server re-encode the video, did not help: still about 17 fps.

## Answer in one paragraph

The PS5 decoder was opened at **pipeline depth 1**, the synchronous mode: each
`sceVideodec2Decode` call finishes one picture before returning. That is only
fast when the stream itself lets the decoder split a picture up -- several
**slices**, several **tiles**, or **WPP**. Backrooms has none of those: one
slice, no tiles, no WPP, at 94 Mbps. Neither does anything the server's
`hevc_nvenc` produces. Such a stream is decoded one picture at a time on one
thread, and at 4K it cannot keep up. The decoder is now opened at **depth 3**,
which lets it work on several pictures at once. Backrooms went from 14 to 24 fps.

## Measurements

All console figures are 20-second `tools/trace.sh` windows, analysed with
`tools/analyse-trace.py`. Backrooms was at 34:06 in every run.

### Before the fix (depth 1)

| Stream | Independent regions | Decode per picture (median / p95) | Shown fps | Audio underrun |
| --- | --- | --- | --- | --- |
| Backrooms, original 94 Mbps | 1 | 62.0 / 165.4 ms | 12.58 | 9.45 s |
| Backrooms, original, freshly launched app | 1 | 55.6 / 155.7 ms | 14.79 | 8.79 s |
| Backrooms, original, core-DTS track (no AAC) | 1 | 58.6 / 160.2 ms | 14.10 | 9.07 s |
| Backrooms, server re-encode at 40 Mbps | 1 | 49.1 / 58.7 ms | 16.89 | 2.54 s |
| Avatar, server re-encode at 40 Mbps | 1 | 47.5 / 55.1 ms | 17.55 | 0 s |
| Avatar, original | 8 slices | 17.0 / 24.7 ms | 23.96 | 0 s |
| Oppenheimer, original 65.9 Mbps | 8 slices | 19.7 / 24.9 ms | 24.01 | 0 s |

A 23.976 fps film needs a picture every 41.7 ms. Note the two re-encodes: 40 Mbps
is less than Avatar's original, yet both decode far slower than it. Bitrate is
not the variable.

### After the fix (depth 3)

| Stream | Shown fps | Dropped | Decode failures | Audio underrun |
| --- | --- | --- | --- | --- |
| Backrooms, original 94 Mbps | **23.95** | 0 | 0 | 0 s |
| Backrooms, server re-encode at 40 Mbps | **23.84** | 0 | 0 | 0 s |
| Avatar, original (regression check) | **24.02** | 0 | 0 | 0 s |
| The Leftovers S03, H.264 1080p 17.2 Mbps (heaviest H.264 in the library) | **23.73** | 0 | 0 | 0 s |

A captured Backrooms frame -- a bright, flat scene, where a decoder writing into
the wrong surface shows up as blocks -- was clean.

Pause, which Brett had seen fail to stop the picture, was checked on the fixed
build: over 12.5 seconds paused the picture timestamp moved 0 ms, 0 pictures
were shown, 0 audio samples played, and the server reported `IsPaused: true`.
The earlier failure did not reproduce; the most likely explanation is the
backlog, but that is inference.

## What was ruled out, and how

Each of these was a reasonable suspect. They are listed so nobody re-tests them.

- **The server.** With throttling off, ffmpeg was 35 minutes ahead of playback;
  with it on, 30-45 seconds of video sat ready while the console played at 0.6x.
- **The network, or routing through Cloudflare instead of the LAN.** Jellyfin's
  socket to the console was `rwnd_limited` 85% of the time -- blocked because the
  console's receive window was full -- and delivered 200-367 Mbit/s whenever it
  could send. On the console, the socket read waited a median of 10 us. The
  transport thread spent 90-98% of samples in stage 4: blocked pushing into a
  full video packet queue that the decoder was not emptying.
- **Jellyfin's throttle.** Same evidence as the server: data was waiting.
- **The audio path.** Backrooms' default DTS-HD MA track is converted to AAC and
  decoded by `sceAudiodec`; Avatar's is not. Playing Backrooms' core-DTS track,
  which is copied and decoded on the CPU with no AAC anywhere, was just as slow
  (58.6 ms, 14.10 fps).
- **HDR, tone mapping, bitrate.** The 40 Mbps re-encode is 8-bit SDR,
  tone-mapped on the server at 79 fps, and still decoded at 49 ms.
- **Leaked state from earlier playback.** A freshly launched app was as slow.
- **Something about the decoder configuration.** SlopFin configures every HEVC
  stream identically (`player.cpp`, `Decoder::open`).
- **The file's basic format.** Backrooms and Avatar are both 3840x2160,
  23.976 fps, Main 10, Dolby Vision profile 7 with an enhancement layer.

## The variable: independent regions per picture

Dumping every SPS and PPS field (`ffmpeg -bsf:v trace_headers`) and counting
slice segments per picture:

| Stream | Slices | Tiles | WPP |
| --- | --- | --- | --- |
| Avatar | 8 | 1 | off |
| Oppenheimer | 8 | 1 | off |
| X-Men (2000) | 1 | 3x3 | off |
| Backrooms | 1 | 1 | off |
| Server `hevc_nvenc` output | 1 | 1 | off |

Slices, tiles and WPP are the three ways HEVC lets a decoder spread a picture's
entropy decoding across threads. A stream with none of them is serial.

Across every 2160p file in the library, most single-slice encodes (x265's
defaults) have WPP on. The only fully serial ones are **Backrooms at 93.9 Mbps**
and **The Amazing Spider-Man 2 at 6.4 Mbps** -- low enough that one thread
manages. Every `hevc_nvenc` transcode is serial, so any 4K stream the server
re-encodes was affected. That is why the problem looked specific to one film.

The published decoder research reached the same conclusion independently:

- H.264 1080p, depth 1: one slice 4.761 ms a picture, four slices 2.268 ms
  (`../research/docs/benchmarks.md`, "H.264 live slice tuning").
- A serial 4K HEVC stream at about 72 Mbps: **47.94 fps at depth 1, 128.39 at
  depth 3**, 176.91 at depth 6 ("HEVC 4K pipeline-depth and WPP experiment").

## Why it was ever depth 1

The decoder setup came from the reference client, `../reference-prosperotv`,
which is an IPTV player and opens its decoder at depth 1 with the same affinity
and priority SlopFin used. For live television that is the right choice: depth 1
returns each picture as early as possible, and the research describes pipeline
depth as a latency policy with depth 1 as its production default. A film player
has the opposite priority -- a few extra milliseconds before a picture is ready
cost nothing, and falling behind the film's frame rate costs everything. Most
files hid the mismatch because their own slices, tiles or WPP let a depth-1
decoder spread each picture across cores anyway.

## The change (`src/player.cpp`)

- `kDecoderDepth = 3`, used for `config.pipeline_depth`. If the memory query
  refuses depth 3, the decoder falls back to depth 1 and says so in the trace.
  The capability probe uses the same depth, because the query validates every
  field.
- **`/data/slopfin-depth1`** forces depth 1 on the same build, for A/B runs.
- **Pictures are only read from a slot the decoder is holding.** Previously a
  picture was accepted only if it came back in the slot handed over on the same
  call (`output.buffer == frame_slot`). At depth 3 it may come back in an
  earlier call's slot, so every slot the decoder accepts is recorded with the
  submission that supplied it, and a returned pointer must match one of those
  exactly. Anything else is refused without being read -- the decoder hands back
  a raw pointer, and reading outside its memory takes the console down.
- The trace's first twelve pictures now log `depth=` and `lag=` (how many calls
  after its submission the picture came back). On firmware tested here, depth 3
  returns every picture in the current call's slot: `lag=0`.
- Sixteen slots still exceed six references plus three in flight.

## Reading traces after this change

**At depth 3, `decode_us` -- and the "decode" figure in the on-screen stats -- is how long the submission call took, not how long a
picture took to decode.** Backrooms now shows a 1.2 ms median. Compare decoders
by `decoded` per second and shown fps, never by the `decode` row, and do not
compare that row with a depth-1 trace.

## Not done

- **End-of-stream drain.** The decoder can still hold up to two pictures when
  the stream ends. `sceVideodec2Flush` is not wired, so the last two frames of a
  film are not shown. Invisible in practice, but real.
- **A seek leaves the previous connection open on the console.** After a seek,
  the server still had the old socket in `LAST-ACK` with 3.9 MB unsent, and the
  console never acknowledged it. Something in `player::stop()` does not close the
  old stream. Not investigated.
- **Latency cost.** Depth 3 holds pictures longer before they are ready (the
  research measured about 16 ms median on synthetic 4K). Presentation uses the
  picture's own timestamps, and no underruns or sync drift appeared, but this was
  not separately measured.

## Traps met during the investigation

- **Jellyfin's `AudioStreamIndex` counts a sidecar subtitle as a stream.** For a
  film with an external `.srt`, Jellyfin's index is ffprobe's plus one. A test
  that asked for "track 3" silently played a different track.
- **`press.sh "play:..."` does not always register.** Confirm what the server's
  ffmpeg is reading before recording a trace. One trace here was about to be
  reported for Backrooms while Avatar was still playing.
- **The newest file in the server's transcode cache may be another title's.**
  One header comparison mislabelled X-Men as the `hevc_nvenc` output. Name the
  file being probed.
- **brettserver has ten Jellyfin accounts, and Brett's is not the first.** Look
  the user up by name before reading resume positions or Continue Watching.

## Reproduce

```
# console: record, then summarise
tools/press.sh "play:<itemId>:<seconds>[:<subtitleIndex>:<audioIndex>[:<maxBitrate>]]"
tools/trace.sh 20 out.csv && tools/analyse-trace.py out.csv

# server: slices / tiles / WPP of a file (base layer)
docker exec jellyfin /usr/lib/jellyfin-ffmpeg/ffmpeg -i "<file>" -map 0:v:0 -c:v copy \
  -bsf:v hevc_mp4toannexb,trace_headers -frames:v 4 -f null - 2>&1 |
  grep -E "first_slice_segment_in_pic_flag|tiles_enabled_flag|entropy_coding_sync_enabled_flag"
```

## 4K H.264 needs the 4K decoder configuration (Opus 5, 2026-09-15)

The H.264 decoder was always opened at 1920x1088, level 5.1. A 3840x1600 H.264
stream (Interstellar) was then refused on every access unit with `0x811d0303`.
`player.cpp` now opens H.264 at 3840x2176, level 5.2, when the server reports
a source wider than 1920 or taller than 1088 (`PlaybackPlan::width/height`).
Measured: 23.98 fps shown, zero decode failures, decoder frame 12537856 bytes.

## Decoder sized from the stream's own SPS (Opus 5, 2026-09-15)

Replaces the server-width rule above with something that holds for every file.
`src/video_sps.hpp` reads the first access unit's SPS (H.264 7.3.2.1 + VUI,
H.265 7.3.2.2) for coded size, bit depth, level and decoded-picture-buffer
depth: `max_dec_frame_buffering` when the VUI states it, otherwise the level's
MaxDpbMbs limit; `sps_max_dec_pic_buffering_minus1 + 1` for HEVC. Checked
against FFmpeg trace_headers on seven streams (tests/test_video_sps.cpp),
including Interstellar's real SPS. Jellyfin's RefFrames is 1 for every file in
the library, so it cannot be used.

- Size: H.264 larger than 1920x1088 opens the 4K configuration (level 5.2).
  HEVC keeps the 4K configuration it has always had.
- DPB: the stream's depth, never below the proven defaults (H.264 5, HEVC 6).
  If a deeper configuration fails to open, it retries at the default.
- Frame slots grow with it: max(16, dpb + depth + 7), so defaults stay at 16.
- Bit depth for the decoder profile comes from the SPS when readable.

Measured on the console: every DPB from 4 to 16 configures for H.264 High
1080p/4K and HEVC Main/Main10 1080p/4K; GPU memory grows linearly (4K Main10:
335 MB at 6, 825 MB at 16). Playback after the change: Interstellar H.264
3840x1600 23.90 fps, Barry HEVC Main10 1080p 23.87, The Hangover H.264 1080p
24.07, The Batman HEVC Main10 3840x1608 HDR 23.87 -- all zero decode failures.
