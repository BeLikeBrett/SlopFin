# SlopFin audio — implementation and evidence

Updated 2026-09-13. The active [compatibility table](COMPATIBILITY.md) and
[GPT plan](../PLAN.md) take precedence over old handoffs. “Native” here means
console-side decoding, not necessarily hardware acceleration or HDMI bitstream.

## The Audio panel (Opus 5, 2026-09-15)

Track, format and channels in one list, opened from the waveform button during
playback (Brett asked for the channel setting to live here rather than under
Quality). It is offered even when a title has one audio track, because format
and channels still apply.

- **Track** appears only with more than one. The server's DisplayTitle is
  prefixed with the stream's own Title, which for a remux is the whole release
  name, so that prefix is stripped when parsing (`read_track`): eleven rows of
  "The.Dark.Knight.2008.2160p.BluRay.REMUX..." became "English - DTS-HD MA".
- **Format** lists only what is *below* the track's own, because the PS5 cannot
  encode audio -- a different format means the server converts that stream
  (video is still copied). Ranks: TrueHD, DTS-HD MA/HRA, FLAC and PCM are 4;
  E-AC-3 and plain DTS 3; AC-3 2; everything else 1. Targets are E-AC-3
  (rank > 3), AC-3 (rank > 2) and AAC (rank > 1). TrueHD and DTS are never
  targets: FFmpeg's encoders for them are experimental.
- **Channels** is Automatic, Stereo, and 5.1 when the source has more and the
  chosen format allows it (AC-3 and E-AC-3 stop at 5.1).
- Choosing a track resets format and channels to that track's own.

Measured on the console (Barry S1E1, E-AC-3 5.1 source, HEVC video copied
throughout): AAC gave "eac3 -> aac transcode"; Dolby Digital gave "eac3 -> ac3"
and opened the AC-3 **bitstream** port, so a conversion still reaches the TV
undecoded; Stereo gave AAC at 2 channels, confirmed in the server's own session
record (`IsVideoDirect: true, AudioChannels: 2`).

## Bitstream passthrough (Opus 5, 2026-09-15)

AC-3, E-AC-3 and DTS are not decoded when the HDMI bitstream port opens. The
TV decodes them.

| Source | Ex mode | Carrier | Port grain | IEC 61937 burst |
| --- | --- | --- | --- | --- |
| AC-3 | 0 | 48 kHz stereo | 256 | type 1 (+bsmod), 6144 bytes, length in bits |
| DTS, any profile | 2 | 48 kHz stereo | 256 | core only, type 11/12/13, 512-2048 frames |
| E-AC-3 | 3 | 192 kHz stereo | 1024 | type 0x15, six audio blocks, 24576 bytes, length in bytes |

- `src/iec61937.hpp` frames the stream and packs bursts. Checked byte for byte
  against `ffmpeg -c copy -f spdif` for all three codecs and a real DTS-HD MA
  track; `tests/test_iec61937.cpp` pins the layout.
- `audio.cpp`: `sceAudioOutExConfigureOutput(0, 0, mode, 255, 0)` then
  `sceAudioOutExOpen(0xff, mode)`. The ring then holds packed words and the
  clock divides by the carrier rate. No volume is applied (it would corrupt the
  words). On stop: `sceAudioOutExClose`, then configure mode 255 to restore.
- If either call fails the decode path runs as before. `/data/slopfin-no-bitstream`
  forces decoding and stops the player asking the server to copy E-AC-3/DTS.
- **Seeking does not close or reconfigure the active HDMI audio port when the
  delivered codec is unchanged.** The network stream, compressed-audio parser
  and decoder state are rebuilt, but the physical output handle stays live. A
  real audio-track/codec change still performs the normal close/configure/open
  sequence, because the TV genuinely needs a new format then.
- A/V sync now includes the TV's own decode delay, which the console cannot
  measure. Watch for lip sync on real titles.
- TrueHD still decodes on the CPU; its bitstream is silent (see
  `tools/bitstream/README.md`).

## Latest CPU integration (2026-09-13)

Opt-in TrueHD/E-AC-3/DTS streaming now feeds the production PCM queue.
TrueHD, E-AC-3 and DTS output tone fixtures pass; a partial EOF grain is padded
so its final samples drain. An E-AC-3 HDR10 movie trial copies both streams and
runs at 23.98 shown FPS without underruns. Avatar copies TrueHD and decodes it
locally, but the first two traces and Brett's listening report show starvation
and choppy sound; this is still experimental. DTS-HD stays on server fallback.
Current output is 48 kHz S16 PCM, not compressed passthrough/Atmos or a claim to
preserve a 24-bit lossless source. See [software audio](SOFTWARE_AUDIO.md) for
build flags, trial marker, precise results and remaining acceptance gates.

Earlier probe-only statements below describe earlier stages of the work.

## Implemented paths

| Input | Current path | Evidence/limits |
| --- | --- | --- |
| AAC | `sceAudiodec`, codec 3, then PCM | Stereo and six-channel captures verified; six-tone channel map passes; output fixed at 48 kHz |
| MP3 | `sceAudiodec`, codec 2, then PCM | Stereo 48 kHz verified in TS remux |
| AC-3 | `sceAudiodec`, codec 4, then PCM | Six-tone production-path channel map passes; reported surround crackling still needs physical-output validation |
| TrueHD | Jellyfin -> AC-3 -> native AC-3 decode | Six-channel 48 kHz, 640 kb/s tested on Pacific Rim; lossy 5.1 fallback |
| E-AC-3, DTS, FLAC, Opus | Jellyfin -> AAC -> native AAC decode | File-specific fallback still needs matrix testing; no native decoder integrated |

Video can remain copied during an audio transcode. Dolby Digital/DTS system
output settings describe the console's final output encoding, not proof that
SlopFin can decode those source codecs or pass through Atmos/DTS:X metadata.
Decoded PCM may be appropriate for channel audio, but is not equivalent to
preserving an original object-audio bitstream.

## Confirmed stereo failure candidate and correction

The sink discards decoded frames whose sample rate is not 48 kHz. Previously
the AAC profile had no sample-rate condition, so Jellyfin could copy 44.1 kHz
AAC and the app silently dropped it. The first 1,000 library items audited
contained 45 stereo AAC tracks at 44.1 kHz (not a whole-library count).

The profile now requires 48 kHz AAC, but the console test exposed a second
problem: 21 Jump Street's stereo track 2 still arrived as copied 44.1 kHz AAC
despite `AudioSampleRate=48000` and `AudioSampleRateNotSupported` in negotiation.
The stream URL now explicitly sets `AllowAudioStreamCopy=false` for non-48 kHz
or unknown source rates. It preserves video-copy eligibility. Host tests cover
the override, duplicate query parameters and default-versus-selected tracks.
Console verification confirms 48 kHz AAC encoding with copied H.264 video.
That exposed a separate deadlock: the video frame pool filled while the same
thread was responsible for finding the next audio PES. Audio ran dry, its clock
froze, and video could not release a slot. Transport/demux/audio submission now
run independently of video decoding, through a bounded 64 MiB compressed-video
queue. 21 Jump Street track 2 now plays continuously with fresh stereo PCM.

PCM overflow previously discarded samples. The producer now waits for space;
the output thread wakes it as samples drain. Pause/stop interrupt blocked
producers before decoder resources are released. The PCM reservoir is eight
seconds per layout, backed by flexible memory; a one-second surround reservoir
did not cover the observed 1.5–2 second socket stalls. Media sockets request a
larger receive window before connecting. Repeat console testing remains required.

The audio capture tool now deletes previous output, waits for marker consumption
and requires complete metadata and PCM. Previously an empty capture could reuse
an earlier recording. Capture now observes blocks submitted to AudioOut,
including underrun silence. The old producer-side capture hid downstream gaps.
The analyzer also lost completed silence runs; it now correctly reports a
1.91-second dropout in a previously misleading Pacific Rim capture.

`tools/audio-fixture.py ac3_51` and `aac_51` both passed the production decoder,
channel remap, PCM queue and output capture: FL 317 Hz, FR 419 Hz, FC 521 Hz,
LFE 83 Hz, BL 631 Hz, BR 743 Hz, spare SL/SR silent. These tests establish
submitted PCM identity; they do not measure the TV/receiver's physical routing.
The user's report of AC-3/AAC 5.1 crackling remains an acceptance test, not
something a valid channel map disproves.

A local resampler could retain original AAC delivery in the future, but
advertising unsupported sink rates before that exists is incorrect. AC-3 stereo now passes its fixture; test
mono AC-3 separately: the current native AC-3 configuration requests
six PCM channels and the implementation expects that fixed output shape.

## Measured native contracts

- Firmware 8.20 output probes opened all eight S16/float formats, including
  eight-channel variants, at 48 and 192 kHz. This does not validate every input
  sample rate or receiver configuration.
- **(Opus 5) The decoder's buffers must be aligned, and nothing in the types says so.**
  `g_pcm`, `g_au`, `g_pcm_item`, `g_control` and `g_param` are plain globals
  whose natural alignment is as low as 1. They sat on usable addresses by
  luck. Adding unrelated globals to audio.cpp moved them, and every AAC decode
  then returned `0x807F0000` while AC-3 kept working -- identical code, a
  different address. Proved by building the same source at HEAD, where the
  same fixture passed. They now carry explicit `alignas`.
- AAC parameters are 24 bytes; max_channels is an upper bound, not a downmix
  request. Eight admits surround input. The PCM capacity is an input to decode;
  passing zero fails. AAC channel order is mapped into the standard sink order.
- AC-3 uses the 88-byte parameter/48-byte information layouts in `src/ac3.hpp`.
  Twelve probe frames each consumed 1,792 compressed bytes and produced 18,432
  PCM bytes (1,536 samples x six S16 channels). LFE must be enabled explicitly.
- A controlled six-tone AC-3 fixture establishes native order L, C, R, Ls, Rs,
  LFE. The app remaps to FL, FR, FC, LFE, BL, BR, SL, SR. AC-3 framing handles
  compressed frames split across PES packets.
- The Hangover verified video/audio copy with native AC-3 decode and six active
  speakers. Pacific Rim verified TrueHD-to-AC3: a 1.98-second sink capture had
  six active channels, silent spare sides, no clipping, no silent blocks and
  no large discontinuities.
- The output worker owns sink changes. Closing a port while that worker is
  blocked in output races; shutdown tests check this ownership.

## Native E-AC-3 / DTS / TrueHD research

Firmware files are present: Ddp (E-AC-3), M4aac and DtsHdLbr under
`/system/common/lib`; Dts, DtsHdMa, Trhd and Lpcm under `/system/priv/lib`.
The CPU dispatcher is `libSceAudiodecCpu`. Presence and export names are static
evidence, not a validated callable decoder contract.

The explicit SlopFin TrueHD reachability probe attempted:

| Path | Load result |
| --- | --- |
| `/system/priv/lib/libSceAudiodecCpuTrhd.sprx` | `0x80020002` |
| `/system/common/lib/libSceAudiodecCpu.sprx` | `0x80020002` |

FTP independently confirmed both files exist. These direct paths did not load
in the app's context. This does not prove every Sysmodule/AvPlayer route is
impossible. The probe makes no speculative decoder calls. While playback is
stopped, create `/data/slopfin-truehd-probe` and `/data/slopfin-audio-caps`, then
read `/data/slopfin-audio-caps.txt`.

The upstream [audio API matrix](../../audio-research/docs/API-MATRIX.md) and
[AvPlayer notes](../../audio-research/docs/MEDIA.md) show an E-AC-3 software
branch. They also document a source-opening failure that affected a known AAC
control; that experiment cannot establish codec support. Earlier claims that
all these codecs were already reachable by an ordinary app were too strong.

## Software decoder route

An optional FFmpeg build now decodes E-AC-3 stereo/5.1 and DTS core 5.1
fixtures on this PS5 with correct tone identities and host-reference samples.
TrueHD 5.1 also passes, exactly matching the host S16 reference. This is not a normal playback fallback
yet; see [software audio results and build steps](SOFTWARE_AUDIO.md).
Production integration must provide:

1. Bounded framing/container handling, complete decoder initialization and
   correct channel layouts, including DTS extensions and TrueHD major sync.
2. Allocation compatible with this runtime and a measured per-stream memory
   budget. Merely linking a host library does not produce a PS5 build.
3. Resampling and sample conversion to the sink, with timestamps preserved.
4. Real fixture and title tests for channel identity, gaps, clipping, CPU cost,
   seek/pause/restart and long playback, while hardware video is active.
5. Honest fallback on unsupported profiles/errors. Lossless PCM decode does not
   by itself preserve Atmos/DTS:X objects.

Hardware/platform decode stays preferred where validated. AC-3 fallback is
useful because this decoder works; AC-3 is not inherently an upgrade over AAC
and its lossy 5.1 limit must remain visible.

## Output research still open

Compressed passthrough exports and HDMI receiver-capability queries are present
but untested in this client. Neither TrueHD passthrough nor E-AC-3/DTS bitstream
output is claimed. Receiver support, sample rates/channel layouts, metadata and
port ownership need independent validation.

*Opus 5 update, 2026-09-15:* the exports are now tested at the API level. Inside
SlopFin, `sceAudioOutExConfigureOutput` and `sceAudioOutExOpen` accept Dolby
Digital, Dolby Digital Plus, DTS and TrueHD (high-bit-rate) modes and consume IEC
61937 streams in real time without errors; from a payload they are refused.
At the TV (2026-09-15) AC-3 and E-AC-3 played as bitstream and DTS showed the
TV's DTS badge; TrueHD shows a Dolby badge but stays silent through every port
tried, the disc player's own Sys pair included. The receiver-capability query
works and Brett's TV reports AC-3, E-AC-3 with Atmos, MAT, DTS and DTS-HD.
Details and the test procedure: `tools/bitstream/README.md`.

Use `tools/listen.sh`, `tools/analyse-audio.py`, and `tools/audio-caps.sh` for
actual evidence. Never infer sound from a successful port open or a codec name
in the server session. Video/HDR history is consolidated in [HDR.md](HDR.md).

## Reproducible decoder fixtures

`python3 tools/make-audio-fixtures.py` generates three-second AAC stereo
(44.1/48 kHz), AAC 5.1, AC-3 stereo/5.1, E-AC-3 stereo/5.1, DTS 5.1 and
TrueHD 5.1 probes under ignored `build/audio-fixtures/`. Each source speaker
has a distinct tone, and each encoded file has an FFmpeg-decoded S16 WAV
reference. The manifest records hashes, sample rates, channel layouts and
FFmpeg version. Back versus side-surround layouts are recorded explicitly.
All nine encodes/reference decodes and format checks passed on the host.

Native AAC/AC-3 5.1 and AC-3 stereo fixtures have passed the production output
path. E-AC-3 and DTS fixtures have passed the separate software PCM probe.
The generator itself performs no network access or console writes;
`tools/audio-fixture.py` uploads and runs a selected fixture. Match PCM by channel
identity and timestamps, account for encoder delay, and compare lossless versus
lossy codecs with appropriate criteria rather than demanding byte equality for
all formats.


## Output pacing fix (2026-09-12)

With 256-frame AudioOut blocks, 21 Jump Street AAC stereo delivered 17.819
seconds of audio in an 18.50-second trace (23.08 shown FPS), despite no ring
underruns. A 1024-frame trial restored real-time output but had some uneven
picture holds. The 512-frame repeat achieved 23.98 FPS between advances,
exactly 228 two-refresh/228 three-refresh holds, and 19.040 seconds of audio
in a 19.04-second trace. Brett confirmed it looked and sounded great.

**512 frames is now the deployed default.** The temporary override marker was
removed. Native AC-3 5.1 tones pass with this default, and Silo HE-AAC 5.1
also delivers real-time audio without measured underruns. The console-only
`/data/slopfin-audio-grain` override accepts 256, 512, 1024 or 2048 at audio
start for further controlled experiments. It does not change sample rate.

Silo S3E9 “Farewell” in this library uses HE-AAC 5.1, 48 kHz. Jellyfin confirmed
both audio and video copy, with only ContainerNotSupported/remux as the reason.
A native excerpt capture differs from the host decode during decoder startup;
after 100 ms, errors are below 0.49 S16 RMS per channel, with maxima of two
S16 units and no clipping. The initial full-window 5% waveform threshold failed
because of that startup transient; it is not evidence of sustained corruption.
Brett subsequently reported that the crackling seemed gone in the live retest.
Keep this provisional until longer playback and other HE-AAC files pass.

One 512-frame timing attempt was invalidated by another agent's deployment.
It was repeated successfully after coordination; trace tooling now rejects
changed-build captures instead of treating them as player measurements.

## Confirmed listening setup

Brett uses the built-in speakers on a TCL 55Q51K (2025 Google TV), with no
soundbar/receiver or separate surround speakers. The audible acceptance test
is correct dialogue/surround downmix and no crackling through that TV. Source
5.1 and correctly mapped eight-slot PCM are intermediate stages, not evidence
of discrete physical 5.1 reproduction. PS5 HDMI Device Type and Audio Format
Priority remain unreported. Start diagnosis with TV routing and PCM, then
compare native surround delivery against an explicit stereo downmix while
keeping the same movie section and output level.
