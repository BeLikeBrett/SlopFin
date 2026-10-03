# Optional FFmpeg audio decoder

This experimental path decodes **E-AC-3, DTS core and TrueHD** on the PS5 CPU
and feeds the production AudioOut queue. It is compiled into the public builds
but is enabled for movie trials only when `/data/slopfin-software-audio` exists.
Remove that marker before the next playback to restore ordinary negotiation.

## Build and test

```sh
make sdk-archives PS5_CLANG=/usr/bin/clang
PS5_CLANG=/usr/bin/clang bash tools/build-software-audio.sh
make PS5_CLANG=/usr/bin/clang SOFTWARE_AUDIO=1
python3 tools/make-audio-fixtures.py
bash tools/test-software-audio.sh
```

Host decoder tests require FFmpeg development libraries. For console fixtures,
close playback, deploy the matching build and verify a fresh startup stamp:

```sh
make deploy PS5_CLANG=/usr/bin/clang SOFTWARE_AUDIO=1
bash tools/launch.sh
python3 tools/audio-fixture.py eac3_51
python3 tools/audio-fixture.py dts_51
python3 tools/audio-fixture.py truehd_51
python3 tools/audio-fixture.py truehd_51 --sink
```

The `--sink` trial includes the production PCM queue and AudioOut. A successful
PCM export alone does not establish playback or speaker output.

## Implementation limits

- FFmpeg 9.0.1 is pinned. The reduced build includes these decoders, parsers,
  sample conversion and utility code; network/demuxer/encoder code is disabled.
- Output is **48 kHz S16 PCM**, including tested TrueHD 7.1 input. Atmos objects,
  DTS:X and full 24-bit source fidelity are not preserved.
- DTS support covers the core; unknown profiles and DTS-HD retain server fallback.
  Unknown rates or channel layouts must not be advertised as supported.
- Large allocations use flexible memory. Fragmented input, decoder flushing,
  channel ordering and output ownership require regression checks.

Synthetic fixtures test parser fragmentation, channel identity and sample
counts against an independent reference. Movie trials and soak tests have been
performed on one firmware/display setup; wider physical output validation is
still needed. Server or disk delivery stalls can cause buffering independently
of the decoder.

The release includes matching FFmpeg source, and [NOTICE](../../NOTICE.md)
records linked-library redistribution terms.
