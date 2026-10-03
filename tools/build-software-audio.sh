#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
# Optional, decoder-only FFmpeg for the PS5 feasibility probe.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD
version=9.0.1
sha=cf38e0e28c7e5605942c4a77755349b0145804a397af37eb1fb4c77cb237f635
sdk="$root/.deps/native/ps5-payload-sdk"
export PS5_CLANG=${PS5_CLANG:-$(command -v clang-18 || command -v clang)}
[[ -x "$sdk/bin/prospero-clang" ]] || { echo 'Run the normal app build first to bootstrap the SDK.' >&2; exit 1; }
mkdir -p .deps/audio build/software-audio
archive="$root/.deps/audio/ffmpeg-$version.tar.xz"
if [[ ! -f "$archive" ]]; then
    curl --fail --location "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz" -o "$archive"
fi
printf '%s  %s\n' "$sha" "$archive" | sha256sum --check
if [[ ! -d ".deps/audio/ffmpeg-$version" ]]; then
    tar -xf "$archive" -C .deps/audio
fi
cd build/software-audio
"$root/.deps/audio/ffmpeg-$version/configure" \
    --prefix="$root/.deps/audio/install" \
    --enable-cross-compile --target-os=freebsd --arch=x86_64 \
    --cc="$sdk/bin/prospero-clang" --cxx="$sdk/bin/prospero-clang++" \
    --ar=/usr/bin/llvm-ar --nm=/usr/bin/llvm-nm --ranlib=/usr/bin/llvm-ranlib \
    --disable-autodetect --disable-everything --disable-programs --disable-doc \
    --disable-debug --disable-network --disable-avformat --disable-avdevice \
    --disable-avfilter --disable-swscale --enable-pthreads \
    --disable-w32threads --disable-os2threads --disable-asm --disable-inline-asm \
    --enable-static --disable-shared --enable-avcodec --enable-avutil --enable-swresample \
    --enable-decoder=truehd,eac3,dca --enable-parser=mlp,ac3,dca \
    --malloc-prefix=slopfin_av_ \
    --extra-cflags='-O2 -fPIC -ffunction-sections -fdata-sections'
# The SDK declares these POSIX functions but its runtime does not export them.
# Use FFmpeg's own time_internal.h fallbacks. This probe is single-threaded;
# review the fallback's static time storage before concurrent decoder use.
sed -i 's/#define HAVE_GMTIME_R 1/#define HAVE_GMTIME_R 0/;s/#define HAVE_LOCALTIME_R 1/#define HAVE_LOCALTIME_R 0/' config.h
make -j"${JOBS:-4}"
make install
