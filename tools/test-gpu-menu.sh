#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
mkdir -p build/tests build/gpu
glslangValidator -V --target-env vulkan1.0 --vn slopfin_menu_shader shaders/menu.comp -o build/gpu/menu_shader.hpp
c++ -std=c++20 -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter \
    -DSLOPFIN_HOST -DSLOPFIN_GPU_MENU -pthread $(pkg-config --cflags sdl2) \
    -Isrc -Ibuild/gpu tests/test_gpu_menu.cpp src/gpu_menu.cpp src/gfx.cpp src/text.cpp \
    src/bigalloc.cpp host/host_platform.cpp $(pkg-config --libs sdl2) -lz -ldl -o build/tests/gpu_menu_tests
for width in 1280 1920 2560 3840; do
    SLOPFIN_ASSETS="$root/assets" build/tests/gpu_menu_tests "$width"
done
