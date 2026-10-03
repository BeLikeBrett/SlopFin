#!/usr/bin/env bash
# SlopFin - re-copy the SDK C++ runtime archives under '+'-free names.
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
# The upstream build script only accepts [A-Za-z0-9_.-] in archive paths.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
lib=$root/.deps/native/ps5-payload-sdk/target/lib
[[ -d $lib ]] || { echo "SDK not fetched yet; run make sdk-archives first" >&2; exit 1; }
mkdir -p "$root/.deps/native/cxx"
cp "$lib/libc++.a"    "$root/.deps/native/cxx/libcxx.a"
cp "$lib/libc++abi.a" "$root/.deps/native/cxx/libcxxabi.a"
cp "$lib/libunwind.a" "$root/.deps/native/cxx/libunwind.a"
echo "Prepared cached C++ runtime archives"
