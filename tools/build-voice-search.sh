#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
# Pinned, static CPU-only Whisper. Dependency sources remain outside the repo.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD
commit=d1be6fde11ac6e0407606b4e42fe72d34add8037 # whisper.cpp v1.9.5
sha=645445da884df6dbd52b30f82d7be7c01d1767c938714e477e786b88d529e9f4
mode=${1:-ps5}
case "$mode" in ps5|host) ;; *) echo 'usage: tools/build-voice-search.sh [ps5|host]' >&2; exit 2;; esac
cache="$root/.deps/voice"
archive="$cache/whisper-$commit.tar.gz"
upstream="$cache/whisper.cpp-$commit"
mkdir -p "$cache"
if [[ ! -f $archive ]]; then
    curl --fail --location --retry 3 "https://codeload.github.com/ggml-org/whisper.cpp/tar.gz/$commit" -o "$archive.part"
    mv "$archive.part" "$archive"
fi
printf '%s  %s\n' "$sha" "$archive" | sha256sum --check
[[ -d $upstream ]] || tar -xf "$archive" -C "$cache"
source_dir=$upstream
platform=()
if [[ $mode == ps5 ]]; then
    sdk="$root/.deps/native/ps5-payload-sdk"
    [[ -d $sdk/target/include ]] || { echo 'Run make deps first to bootstrap the SDK.' >&2; exit 2; }
    export PS5_PAYLOAD_SDK=$sdk
    export PS5_CLANG=${PS5_CLANG:-$(command -v clang-18 || command -v clang)}
    # A separate disposable source copy leaves the verified upstream untouched.
    source_dir="$cache/ps5-source-$commit"
    patch_sha=$(sha256sum "$root/tools/prepare-voice-search.py" | cut -d' ' -f1)
    if [[ ! -f $source_dir/.slopfin-patch || $(cat "$source_dir/.slopfin-patch") != "$patch_sha" ]]; then
        rm -rf "$source_dir"
        cp -a "$upstream" "$source_dir"
        python3 "$root/tools/prepare-voice-search.py" "$source_dir"
        printf '%s\n' "$patch_sha" > "$source_dir/.slopfin-patch"
    fi
    # The checked-in compiler wrapper is intentionally not executable.
    compiler_wrapper="$cache/prospero-voice-cc"
    printf '#!/usr/bin/env bash\nexec bash "%s/tooling/prospero-clang18" "$@"\n' "$root" > "$compiler_wrapper"
    chmod +x "$compiler_wrapper"
    platform=(-DCMAKE_SYSTEM_NAME=FreeBSD -DCMAKE_SYSTEM_PROCESSOR=x86_64
        -DCMAKE_C_COMPILER="$compiler_wrapper"
        -DCMAKE_CXX_COMPILER="$compiler_wrapper"
        -DCMAKE_AR="$(command -v llvm-ar-18 || command -v llvm-ar)"
        -DCMAKE_RANLIB="$(command -v llvm-ranlib-18 || command -v llvm-ranlib)"
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY)
fi
build="$cache/build-$mode"
# Reconfigure is cheap; all compile work remains incremental for unchanged inputs.
cmake -S "$source_dir" -B "$build" "${platform[@]}" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$cache/install-$mode" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_C_FLAGS='-O2 -ffunction-sections -fdata-sections' \
    -DCMAKE_CXX_FLAGS='-O2 -ffunction-sections -fdata-sections -fexceptions -frtti' \
    -DBUILD_SHARED_LIBS=OFF -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_EXAMPLES=OFF \
    -DWHISPER_BUILD_SERVER=OFF -DWHISPER_CURL=OFF -DWHISPER_COREML=OFF \
    -DWHISPER_OPENVINO=OFF -DWHISPER_VITISAI=OFF \
    -DGGML_NATIVE=OFF -DGGML_CCACHE=OFF -DGGML_OPENMP=OFF -DGGML_BLAS=OFF \
    -DGGML_LLAMAFILE=OFF -DGGML_BACKEND_DL=OFF -DGGML_ACCELERATE=OFF \
    -DGGML_CUDA=OFF -DGGML_METAL=OFF -DGGML_VULKAN=OFF -DGGML_OPENCL=OFF \
    -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_BMI2=OFF
cmake --build "$build" --parallel "${JOBS:-4}"
cmake --install "$build"
# Shipped binaries must carry these notices when voice search is enabled.
mkdir -p "$cache/install-$mode/share/licenses/whisper"
cp "$upstream/LICENSE" "$cache/install-$mode/share/licenses/whisper/LICENSE"
printf '\nVoice search %s: %s\n' "$mode" "$cache/install-$mode"
