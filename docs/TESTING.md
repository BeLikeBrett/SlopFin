# Testing SlopFin

## Host checks

From `app/`:

```sh
make test
make host
make lint
make PS5_CLANG=/usr/bin/clang
```

Playback tests cover timestamps/scanout convention, audio shutdown ownership,
subtitle parsing, TS/AC-3 framing, HDR matrix/packing, scaling, URL selection,
negotiation, media bitrate and isolated frame recordings. App model tests also cover native keyboard requests, server address handling,
search grouping, intro metadata, settings persistence and navigation motion.
Renderer checks cover rounded gradient clipping, symmetric corners and partial
edge coverage. The avatar test drives crop controls and verifies the exported
JPEG using a synthetic image. Update checks cover SHA-256 reference vectors,
release selection, ZIP integrity, path rejection, staged-file tampering and
rollback after an injected partial-install failure, including executable
permissions and restoration of original file modes.
Network checks require OpenSSL and generate a disposable local certificate;
they do not require a public server or credentials.
`make test-unit` aliases the app model suite; `make test` adds local network and
tooling integration tests. The optional software audio test needs host FFmpeg
development libraries: `bash tools/test-software-audio.sh`.

## Console evidence

Coordinate with the person/agent owning the console before deploying. Record
the trace's build stamp, item and audio/subtitle indexes, quality limit, actual
server video/audio delivery, SDR/HDR mode and overlay state.

| Tool | Evidence |
| --- | --- |
| `tools/launch.sh` / `tools/ensure-running.sh` | Launch and render-loop liveness |
| `tools/press.sh` | Input or `play:<id>:<seconds>:<subtitle>:<audio>:<bitrate>` |
| `tools/trace.sh 12 /tmp/run.csv` | Fresh isolated frame recording |
| `tools/analyse-trace.py /tmp/run.csv` | Frame-time percentiles and presentation rate |
| `tools/listen.sh 3` | PCM capture and speaker/gap/clipping analysis |
| `tools/shot.sh` | Staging diagnostic preview; not HDMI color verification |
| `tools/audio-caps.sh`, `tools/video-caps.sh` | Explicit capability probes; run while playback is stopped |
| Server session/FFmpeg logs | Actual copy/transcode, sample rate, channels and tone-map filters |

Do not screenshot, dump audio, probe codecs or run another deployment during a
frame-time benchmark. Measure clean playback separately from debug/controls.
A three-second media bitrate window can differ from the file-wide source
average and the selected ceiling; see [bitrate definitions](BITRATE.md).

## Codec fixtures

Run `python3 tools/make-audio-fixtures.py` to create synthetic compressed audio
and reference WAVs locally. It records codec/rate/channel metadata and performs
no deployment. See [audio evidence](AUDIO.md).

## Required media matrix

- AAC stereo 44.1 and 48 kHz, AAC surround, AC-3 stereo/5.1, E-AC-3 and FLAC
  fallback; actual TrueHD-to-AC3 output and channel routing.
- H.264/HEVC SDR and PQ Main10; cold video transcode; HDR-to-SDR and back;
  bitrate changes both above and below the source rate.
- Dolby Vision Profile 8.1 and Profile 7 only after a tested base-layer path;
  incompatible/pure-DV/HLG sources must not be mislabeled as PQ HDR10.
- Source cadences from film to 50/59.94 fps, pause/resume, repeated seeks,
  selected/default audio changes, subtitles on/off and long playback.

Acceptance means reference-correct pixels/PCM, correctly reported fallback,
responsive controls and sustained source cadence. No single screenshot,
average FPS, successful decoder creation or library export proves those.
