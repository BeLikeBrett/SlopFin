# Software audio experiment — GPT work

Updated 2026-09-13 (America/Chicago). An opt-in streaming CPU decoder now feeds
the production AudioOut queue. Normal builds still default to server fallback.
TrueHD/E-AC-3/DTS-core movie trials require `SOFTWARE_AUDIO=1` and the console marker
`/data/slopfin-software-audio`; remove the marker before the next playback to
restore ordinary negotiation. DTS-HD and unknown DTS profiles retain server fallback.
No compressed passthrough, Atmos/DTS:X or full 24-bit lossless output claim:
the current sink receives 48 kHz S16 PCM.

## Reproduce

After a normal app build has bootstrapped the SDK:

```sh
bash tools/build-software-audio.sh
make PS5_CLANG=/usr/bin/clang SOFTWARE_AUDIO=1
python3 tools/make-audio-fixtures.py
make deploy PS5_CLANG=/usr/bin/clang SOFTWARE_AUDIO=1
bash tools/launch.sh
# Confirm a fresh build stamp and stop video before testing.
python3 tools/audio-fixture.py eac3_51
python3 tools/audio-fixture.py eac3_20
python3 tools/audio-fixture.py dts_51
python3 tools/audio-fixture.py truehd_51
# Exercise the production PCM queue and AudioOut as well:
python3 tools/audio-fixture.py truehd_51 --sink
python3 tools/audio-fixture.py eac3_51 --sink
python3 tools/audio-fixture.py dts_51 --sink
# Optional workstation decoder tests (host FFmpeg development libraries):
bash tools/test-software-audio.sh
```

The pinned FFmpeg 9.0.1 build enables only TrueHD/E-AC-3/DTS decoders, their
parsers, sample conversion and utility code. Network, container demuxers,
encoders and assembler are disabled. Allocation uses SlopFin flexible memory.
The SDK declares two unavailable time functions; the build selects FFmpeg's
existing fallbacks. Before concurrent production use, replace or guard their
shared static time storage. License/provenance is in [NOTICE](../NOTICE.md).

`software_audio::Decoder` is owned by one transport producer. It parses split
input using padded bounded chunks, rejects a parser that cannot make progress,
resamples into a bounded 48 kHz S16 buffer, and emits timestamped PCM with an
explicit map from decoded speaker identities to AudioOut slots. Unsupported
layouts or midstream format changes fail explicitly. A callback can block for
queue space; interruption wakes it before decoder destruction. EOF drains the
parser, decoder and resampler; AudioOut pads one final partial grain with silence.

The default fixture route captures decoder PCM without AudioOut. `--sink` uses
the production decoder/map/ring/output route, recording the actual submitted
blocks. Neither records physical HDMI. TrueHD, E-AC-3 and DTS 5.1 fixtures use
side surrounds; the eight-slot capture must place those in SL/SR, not BL/BR.
The independent host reference and tone identity both matter. Host tests cover
one-byte/188-byte/large fragmentation, first PTS, EOF, cancellation and reopening.

Allocation uses flexible memory, including small FFmpeg objects; allocation
cost still needs profiling. The movie trial currently admits only TrueHD/E-AC-3 and explicitly identified DTS core
with known 48 kHz metadata and 1–8 channels, honoring a user's lower channel cap.
Runtime decode failure stops the trial with an error; automatic renegotiation
has not yet been implemented. Do not advertise this as universal support.

## Streaming integration evidence (2026-09-13)

- PS5 build `08:42:21`: shared streaming TrueHD probe exactly matches the S16
  reference. E-AC-3 and DTS production-output tones pass with no decoder/output
  errors. The first TrueHD output test found a harness side/back expectation
  mismatch and a real 128-frame EOF tail that never drained.
- Build `08:46:07`: corrected TrueHD output test passes every tone and silent
  slot, decoder/output errors zero, final buffer empty. Fixture underrun totals
  include unbuffered startup and time after the fixture ends; they are not
  steady-state movie measurements.
- Avatar at 180 seconds negotiates HEVC and TrueHD **copy**. Server FFmpeg maps
  source 7.1/48 kHz/24-bit TrueHD without audio encoding; PS5 opens the CPU
  decoder and eight-slot output while keeping the DV HDR10 base-layer surface.
- Brett confirms the first Avatar trial sounded choppy but was technically
  working. Initial clean traces are not yet smooth: 18.52 s at 17.44 shown FPS with
  5.045 s audio underrun; repeat 18.50 s at 18.54 FPS with 4.331 s underrun.
  Both have zero video decode failures or dropped-frame counter increments.
  Input spends most sampled time awaiting socket reads; server remux is faster
  than real time. This does not establish which network/server-read stage is
  responsible. Buffering/performance remain a release gate.
- A separate 3.53 s live AudioOut capture has distinct channels, no clipping or
  detected silent blocks; that short capture does not negate the trace stalls.


- Build `08:49:52`, “Chapter 10: The Dark Lord”: server confirms HEVC and
  E-AC-3 copy; CPU E-AC-3 decode and HDR10 base-layer output run together.
  A clean 19.02 s trace shows 456 pictures (23.98 FPS), zero audio underruns,
  output errors, video decode failures or drops. A separate 4 s submitted-PCM
  capture has no clipping, detected silence gaps or large sample jumps.
  Brett confirms it sounds and looks great on his TV speakers.

- Build `08:53:58`, The Bourne Supremacy: H.264 and DTS core copy,
  local CPU decode. With debug visible, the 18.57 s trace shows 445 pictures
  (23.97 FPS), zero underruns/output errors/decode failures/drops, and a cadence
  of 223 two-refresh holds and 222 three-refresh holds. Brett confirms clean,
  synchronized dialogue. DTS-HD and unknown profiles remain on server fallback.
- Host TS integration tests mux all four fixtures with independent FFmpeg,
  then feed fragmented TS through SlopFin's demux and streaming decoder. PCM
  matches the elementary-stream decode and independent reference exactly.

- Brett reports selecting stereo audio transcode removes the TrueHD hitching.
  This is useful evidence that the changed audio path matters; it does not by
  itself distinguish compressed-data delivery, decoding cost and output layout.
  Added per-session software PCM/input-byte totals plus parser/decode/convert
  elapsed time separated from PCM queue callback time. These counters are
  available in the next build's frame trace; they are not CPU utilization.
  The user had moved to another episode, so no purported controlled A/B trace
  is attributed to the stereo observation.

## Console evidence

FW 8.20, build `Sep 12 2026 21:15:46`, with a diagnostic build at
`21:17:28` and the successful TrueHD retry build at `21:21:04`. Three-second, 48 kHz fixtures, one decoder thread, no video active:

| Fixture | Result | Decode/convert elapsed | Host-reference RMS error (S16 units) |
| --- | --- | --- | --- |
| E-AC-3 5.1 | All six tones and sample count pass | 0.018893 s | 0.008 |
| E-AC-3 stereo | Both tones and sample count pass | 0.007136 s | 0.008 |
| DTS core 5.1 | All six tones and sample count pass | 0.045279 s | 0.011 |
| TrueHD 5.1 | All six tones and sample count pass; S16 PCM exactly matches reference | 0.147933 s | 0.000 |

Elapsed excludes file loading/writing and decoder opening; these short probes
are not production CPU utilization or movie-playback measurements. DTS and
E-AC-3 report `5.1(side)` from the decoded channel layout; final output mapping
must honor that rather than assuming all six-channel layouts are identical.

The first DTS fixture used an 83 Hz LFE tone which the host encoder aliased.
Matching the host decode alone therefore passed an incorrect source tone. The
corrected fixture uses 31 Hz for DTS LFE and passes both checks. Other codecs
retain 83 Hz. The manifest records each fixture's actual expected frequencies.

TrueHD setup/open succeeded. The MLP parser first acquires major sync and
returns zero consumed bytes with no packet; the original probe mistook this
valid state transition for failure. The bounded retry then passed on the console, decoding all 3,600 frames
(144,000 samples/channel). No decoder-support conclusion follows from the
first failed harness run. Follow-up build `Sep 12 2026 21:24:10` feeds 188-byte chunks. TrueHD,
E-AC-3 5.1 and DTS 5.1 again pass tones and host-reference comparisons.
Decode/convert times were 0.165276, 0.019394 and 0.045780 seconds respectively.

## Production gates

1. Extend passing fragmented-input and real 7.1 cases to more profiles/rates.
2. Preserve complete frames and timestamps across split PES, seek and restart.
3. Convert/resample and map layouts into the existing owned AudioOut sink.
4. Measure memory and decode time while hardware HDR video is active.
5. Verify continuous audio, physical speakers and fallback behavior before
   advertising any codec to Jellyfin.


## Real TrueHD 7.1 excerpt

Build `Sep 12 2026 21:40:13` also decoded an unmodified encoded excerpt from
Avatar: Fire and Ash's first audio track: 977096 encoded bytes, 2,272 decoder
frames, 90,880 samples/channel at 48 kHz, layout 7.1. All eight S16 PCM channels
matched the independent host reference exactly, with zero sample offset.
Decode/convert elapsed was 0.112488 seconds for 1.893 seconds of audio, using
188-byte input chunks. The excerpt is kept only under `/tmp`, not in the
repository. This validates a real 7.1 bitstream in the software probe; movie
streaming, AudioOut integration and Atmos preservation remain separate gates.

---

# Opus 5 continuation (2026-09-13)

*Everything below this line is by a different agent (Opus 5), appended to the
GPT record above. Nothing above it has been edited. Keep new work in this
section, or start another marked one, so the two records stay separable.*

## TrueHD starvation did not reproduce — four trials, two of them stressed

The record above closes on Avatar TrueHD starving: 17.44 and 18.54 shown FPS
with 5.045 s and 4.331 s of audio underrun, plus Brett's report that it sounded
choppy. On the current tree it does not happen. Same build flags
(`SOFTWARE_AUDIO=1` plus the console marker), same title, and one trial at the
same 180-second position:

| Trial | Position | Shown FPS | Dropped | Audio underrun | Rebuffer events | CPU decode |
| --- | --- | --- | --- | --- | --- | --- |
| Baseline | 300 s | 23.98 | 0 | **0.000 s** | 0 | 1.587 s per 22.566 s of PCM |
| Baseline | 180 s | 23.98 | 0 | **0.000 s** | 0 | 1.199 s per 16.799 s |
| Server under load | 180 s | 24.00 | 0 | **0.000 s** | 0 | 1.334 s per 18.504 s |
| Transport pinning removed | 180 s | 24.00 | 0 | **0.000 s** | 0 | 2.074 s per 17.924 s |

Windows are 18 seconds. Decode cost is roughly 7% of one core.

**Brett was away, so nothing here rests on listening.** Three separate
four-second captures of the PCM actually submitted to AudioOut: eight channels
active and distinct, **0% silent blocks, longest gap 0 ms**, no clipping, no
sample jumps over half full scale. Two were downmixed to stereo and sent to him
to judge by ear remotely. (`tools/listen.sh` caps at four seconds -- the capture
buffer is four seconds of eight-channel.)

## Two hypotheses tested and rejected

Neither is the cause, and both are recorded so nobody spends the time twice.

- **Server contention.** The poster warming added the same morning makes
  Jellyfin resize hundreds of images for the first time, ~130-180 ms of server
  CPU each, and the GPT traces fall inside that window. Recreated deliberately:
  three threads forcing fresh resizes at unseen sizes throughout playback, 63
  of them completed during the trace. **Zero underruns, 24.00 FPS.** Rejected.
- **The transport thread's core.** `TransportProducer::run` pins itself to
  `images::spare_cores()`, and the same inheritance bug had put the artwork
  workers on the render core. Removing the pinning and rebuilding: **still zero
  underruns, 24.00 FPS.** Rejected -- though that window contained a **1.28 s**
  socket stall, absorbed whole by the eight-second PCM reservoir.

## What this does and does not establish

It does **not** disprove the earlier observation. Different day, different
server and network state, and no attempt was made to rebuild the 08:4x tree and
reproduce it there.

The likeliest explanation is the GPT work's own later changes: those traces are
from builds `08:42:21` and `08:46:07`, while `player.cpp` and `audio.cpp` were
last changed at 16:53 -- the buffer gate, the EOF drain and the PCM queue
backpressure all landed after the choppy traces were recorded. That is a
timeline, not a measurement.

One detail argues against a delivery explanation specifically: **the buffer gate
never engaged in any trial** -- 0 rebuffer events, 0.000 s of intentional
buffering, every time. Whatever fixed this, it is not the gate quietly covering
a starving stream.

## Thirteen minutes straight, unattended

The four trials above are 18-second windows. This is one continuous playback of
Avatar (HEVC/DV HDR10 base + TrueHD 7.1, both copied) sampled every 95 seconds
for **13 minutes 8 seconds**. The counters in the frame trace are cumulative per
playback session, so the figures below cover **every second of the run**, not
only the sampled windows:

| | Whole run |
| --- | --- |
| Audio played | 732.5 s |
| **Audio underrun** | **0.000 s** |
| Audio errors | 0 |
| Rebuffer events | 0 |
| Intentional buffering | 0.64 s, all of it at startup |
| Decode failures | 0 |
| Pictures shown | 17,558 |
| Dropped | 7 (0.04%) |
| Shown FPS across eight windows | 23.93 - 24.02 |

Two four-second PCM captures, one in a quiet passage (-58 dBFS) and one loud
(-23 dBFS): 0% silent blocks, longest gap 0 ms, no clipping, no glitches.

The seven dropped frames are one at startup and then three shortly after each
of the two audio captures. Writing a three-megabyte WAV off the console costs
vertical blanks -- the same harness effect that made a screenshot look like a
300 ms stall elsewhere in this project. Probably measurement, not playback, but
that is an attribution and not a measurement.

**Correcting the section above:** the buffer gate *does* engage, once, for
0.64 s at startup, which is what it is for. The earlier "never engaged" was an
artefact of windows that all began after playback had settled.

## Eighteen minutes straight: the gate earns its keep

A second continuous run of the same title, 1,083 s (18 min 3 s), sampled every
95 seconds. Audio again never starved -- and this time the gate was used in
anger:

| | Whole run |
| --- | --- |
| Audio played | 1,007.2 s |
| **Audio underrun** | **0.000 s** |
| Audio errors | 0 |
| Decode failures | 0 |
| **Rebuffer events** | **3** |
| **Intentional buffering** | **12.23 s** (0.81 s of it at startup) |
| Pictures shown | 24,136 |
| Dropped | 14 (0.06%) |
| Shown FPS across eleven windows | 23.97 - 24.03 |

The three rebuffers fall between t+495 s and t+687 s -- roughly minutes eight to
eleven -- and cost about 11.4 s of held picture between them. The first run, 13
minutes on the same title an hour earlier, had **zero**. Same build, same
position, same house: delivery varies that much.

So the correct statement about this feature is narrower than "TrueHD is fine":

- **The CPU decoder is not the problem.** Across 31 minutes of continuous
  playback over two runs, audio underrun is 0.000 s, audio errors 0, decode
  failures 0, and every PCM capture is continuous with no silent blocks.
- **Delivery still gaps.** Three refills in 18 minutes is roughly one every six
  minutes, and a refill is a visibly held picture, not a silent one. That is
  what the gate is for, and it converted what would have been audio dropouts
  into brief pauses -- which is the right trade, but is not nothing.
- The console is on Wi-Fi here (`enp14s0`, the wired bridge through the
  workstation, is down; ping is ~9.5 ms rather than sub-millisecond), and this
  is the heaviest content in the library: 4K Dolby Vision video and TrueHD 7.1
  both **copied**, so the server re-encodes nothing and the whole original
  bitrate crosses the network.

That makes the earlier "Brett reports selecting stereo audio transcode removes
the TrueHD hitching" observation look less like a decode-cost signal and more
like a bitrate one -- but that is still an inference, not a measurement, and
the controlled A/B that would settle it has not been run.

## Where the delivery gaps actually come from (2026-09-14)

Not the console, and not the network. Measured rather than assumed, in this
order:

1. **The console's own trace.** Single socket reads blocking 0.3-1.7 s,
   repeatedly, across the eighteen-minute soak. A saturated link gives slow but
   steady reads; this is a stream that stops and restarts.
2. **The same stream pulled to a workstation** over wired Ethernet, reading
   64 KiB at a time exactly as the app's transport does. The stalls reproduce
   there, so nothing about the PS5 or its link is responsible.

   | 150 s sample, server to workstation | Throughput | Stalls over 0.5 s | Worst |
   | --- | --- | --- | --- |
   | Static file, no ffmpeg | 339 Mbit/s | 7 | 2.6 s |
   | Remux, what the PS5 gets | 311 Mbit/s | **29** | 3.2 s |

   Bandwidth is never the constraint -- both runs average over 300 Mbit/s for a
   stream that needs about 70. The remux stalls four times as often as the
   static read of the same file, so ffmpeg amplifies the problem, but the
   static read stalls too, which places the cause underneath both.
3. **Jellyfin's transcode throttling**, the obvious suspect, is **off**
   (`EnableThrottling: false`). Checked before theorising further.
4. **The server's media disk.** A 3.6 TB spinning disk, 75% full:

   | sda | |
   | --- | --- |
   | Utilisation | **89.2%** |
   | Average read latency | **236 ms** |
   | Average write latency | 389 ms |
   | Queue depth | 15 requests |

   Live per-process rates during a stream: Jellyfin reading 42 MB/s for the
   film, and a torrent client reading 25 MB/s off the same platter at the same
   time. Two sequential readers on one spindle is seek thrashing, and both
   wait. Separately, SABnzbd has written 2.2 TB over its 48-day uptime, and
   2,811 zombie processes are parented by an unrelated `uv run main.py`.

**None of this is fixable from inside the app.** It can only be ridden out, and
the server-side items are the owner's call, not a code change.

## Riding it out: both buffers doubled

The reservoir sat pinned at its ceiling in normal play, and the gate only asks
to refill once it is *empty* -- so every rebuffer meant the stream had stopped
arriving for longer than the entire reservoir.

- **PCM reservoir 8 s -> 16 s** (`kRingFrames`, audio.cpp). Eight channels of
  sixteen seconds is 12 MB against 1.4 GB free; the old ceiling was not paying
  for itself.
- **Compressed video queue 64 -> 128 MiB** (player.cpp): roughly seven seconds
  of this library's heaviest stream, now roughly fifteen.

Neither makes playback wait longer to start -- the gate decides that, and it
still opens on the same condition.

## The delivery gaps are fixed, and it was the server (2026-09-14)

Three eighteen-minute soaks of the same title, same client, same build:

| Run | Rebuffers | Intentional buffering | Underrun | What had changed |
| --- | --- | --- | --- | --- |
| Before | **3** | 12.23 s | 0.000 s | nothing |
| Bigger buffers | **3** | 11.91 s | 0.000 s | PCM 8->16 s, video queue 64->128 MiB |
| After server fixes | **0** | 0.59 s (startup only) | 0.000 s | the three changes below |

**Doubling the client buffers changed nothing**, which is the honest verdict on
that change: the gaps were longer than sixteen seconds, so no client-side
reservoir was ever going to cover them. It is kept because it costs 12 MB and
widens the margin, not because it fixed anything.

What actually fixed it, all on the server:

1. **163 GB of orphaned transcodes deleted.** Fourteen files, all from the
   previous day, one of them 23.6 GB on its own.
2. **Transcode throttling enabled** (`EnableThrottling`, 60 s lead). It was off,
   so ffmpeg raced to remux entire films at full speed -- which is where a
   23.6 GB temp file comes from, and why the disk saw a simultaneous 55 MB/s
   read and 43 MB/s write for a stream needing 9 MB/s.
3. **Seeding stopped.** A torrent client was reading 25 MB/s off the same
   spindle while the film was being read from it.

And then the cause of the cause: **the transcode cache lived on the same
spinning disk as the media**. Every remux read the source from that disk and
wrote the output back to it. It is now on the NVMe via a dedicated
`/transcodes` mount, with `TranscodingTempPath` pointed at a subdirectory
inside it rather than at the mount point itself -- Jellyfin deletes and
recreates that directory at startup, and pointing it at a mount point makes
`GetTranscodePath` throw and the server refuse to start. Measured immediately
after, during a remux:

    sda       read 100.0 MB/s   write   0.0 MB/s   util 73.6%
    nvme0n1   read   0.1 MB/s   write  98.2 MB/s   util 19.4%

The media disk no longer writes at all while serving a film. An hourly cron
deletes transcode files older than six hours, because the old path had
accumulated 230 GB and the compose file records root filling up before.

**None of this was the client.** Bandwidth was never below 300 Mbit/s, and the
same stalls reproduced pulling the same stream to a wired workstation.

## What is still worth watching

Socket stalls are real and vary a lot: the four traces peaked at 442 ms, 15 ms,
4.5 ms and 1.28 s. All were absorbed by the reservoir, but the 1.28 s case is
half of what the reservoir holds for surround. A longer title, a higher
bitrate, or a worse network moment could still exceed it, which is presumably
why the gate exists. The gate has therefore never been exercised in anger --
its behaviour under real starvation remains untested.
