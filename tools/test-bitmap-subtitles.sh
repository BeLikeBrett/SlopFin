#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
read -r -a flags <<< "$(pkg-config --cflags --libs libavcodec libavutil)"
"${HOST_CXX:-clang++}" -std=c++20 -O2 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -pthread \
    -DSLOPFIN_BITMAP_SUBTITLES -Isrc tests/test_bitmap_subtitles.cpp src/bitmap_subtitles.cpp \
    "${flags[@]}" -lz -o build/tests/bitmap_subtitle_tests
build/tests/bitmap_subtitle_tests
"${HOST_CXX:-clang++}" -std=c++20 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -DSLOPFIN_BITMAP_SUBTITLES -Isrc tests/test_negotiation.cpp src/json.cpp \
    -Wl,--gc-sections -o build/tests/bitmap_negotiation_tests
build/tests/bitmap_negotiation_tests
