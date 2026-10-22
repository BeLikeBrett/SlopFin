#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the public PS5 Vulkan driver in an isolated, ignored cache.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cache="$root/.deps/gpu"
sources="$cache/sources"
mkdir -p "$sources"
for command in git meson ninja glslangValidator rsync; do
    command -v "$command" >/dev/null || { echo "GPU build requires $command" >&2; exit 2; }
done
# Host Mesa CLC also needs LLVM/Clang development files and LLVMSPIRVLib.
fetch() {
    local name=$1 revision=$2
    if [[ ! -d $sources/$name/.git ]]; then git init -q "$sources/$name"; fi
    if ! git -C "$sources/$name" cat-file -e "$revision^{commit}" 2>/dev/null; then
        git -C "$sources/$name" fetch --depth 1 "https://github.com/mihawk-99/$name" "$revision"
    fi
    git -C "$sources/$name" checkout -q --detach "$revision"
}
fetch PS5_Vulkan f410e67a0241b3e8edcdbcd2a9b285dfc4a412ed
fetch PS5_PayloadSDK b83202be73e930050e00de0f1b4d9ec46c0391bf
fetch PS5_Mesa 1ba9874f478657ccf6da72ce55f0260164cc0bd0
vulkan="$sources/PS5_Vulkan"
bash "$sources/PS5_PayloadSDK/platform/tools/setup-sdk.sh" \
    "$vulkan/.deps/native/ps5-payload-sdk" b83202be73e930050e00de0f1b4d9ec46c0391bf "$vulkan/.deps/native"
PS5_MESA_FORK="$sources/PS5_Mesa" bash "$vulkan/tools/build-radv.sh" release
mkdir -p "$cache/radv-release" "$cache/ps5-payload-sdk"
rsync -rlp --checksum --delete "$vulkan/.deps/native/radv-release/" "$cache/radv-release/"
rsync -rlp --checksum --delete "$vulkan/.deps/native/ps5-payload-sdk/" "$cache/ps5-payload-sdk/"
echo "GPU dependencies ready; build with GPU_MENU=1 make SOFTWARE_AUDIO=1"
