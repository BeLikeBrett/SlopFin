#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
# Optional real-codec tests; host FFmpeg dev libraries and generated fixtures required.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
mkdir -p build/tests
read -r -a ffmpeg_flags <<< "$(pkg-config --cflags --libs libavcodec libswresample libavutil)"
"${HOST_CXX:-clang++}" -std=c++20 -O2 -Wall -Wextra -Werror -DSLOPFIN_SOFTWARE_AUDIO -Isrc \
    tests/test_software_audio_stream.cpp src/software_audio_stream.cpp src/tsdemux.cpp "${ffmpeg_flags[@]}" \
    -o build/tests/software_audio_stream
for codec in truehd_51 eac3_51 eac3_20 dts_51; do
    build/tests/software_audio_stream "$codec" "build/audio-fixtures/$codec.bin" "build/tests/$codec.raw"
    format=${codec%%_*}
    ffmpeg -v error -y -f "$format" -i "build/audio-fixtures/$codec.bin" -c:a copy -strict -2 \
        -f mpegts "build/tests/$codec.ts"
    build/tests/software_audio_stream "$codec" "build/tests/$codec.ts" "build/tests/$codec.ts.raw" ts
    cmp "build/tests/$codec.raw" "build/tests/$codec.ts.raw"
done
python3 - <<'PY'
from pathlib import Path
import wave
import numpy as np
for name in ['truehd_51', 'eac3_51', 'eac3_20', 'dts_51']:
    with wave.open(f'build/audio-fixtures/{name}.reference.wav', 'rb') as f:
        assert f.getsampwidth() == 2 and f.getframerate() == 48000
        ref = np.frombuffer(f.readframes(f.getnframes()), dtype='<i2').astype(float)
    pcm = np.frombuffer(Path(f'build/tests/{name}.raw').read_bytes(), dtype='<i2').astype(float)
    assert pcm.shape == ref.shape, (name, pcm.shape, ref.shape)
    error = np.sqrt(np.mean((pcm-ref)**2))
    assert error < 8, (name, error)
    print(f'{name}: independent reference PCM RMS error {error:.4f}')
PY
