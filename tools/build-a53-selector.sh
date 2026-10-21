#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the A53 selector payload native packages need, from a pinned
# ppr-patch revision, with that revision's source alongside it (GPL-3.0).
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
ppr="$root/.deps/fpkg/ppr-patch"
revision=fd4c8224563130e9698d3b2b2f44712826ceb525
if [[ ! -d $ppr/.git ]]; then
    mkdir -p "$(dirname "$ppr")"
    git clone --no-checkout https://github.com/drakmor/ppr-patch.git "$ppr"
fi
if [[ $(git -C "$ppr" rev-parse HEAD 2>/dev/null || true) != "$revision" ]]; then
    git -C "$ppr" fetch origin "$revision"
    git -C "$ppr" checkout --detach "$revision"
fi
[[ -z $(git -C "$ppr" status --porcelain --untracked-files=no) ]] ||
    { echo "$ppr has local changes; refusing to build from it." >&2; exit 2; }
PS5_PAYLOAD_SDK="$root/.deps/native/ps5-payload-sdk" \
    make -s -B -C payloads/a53-selector PPR="$ppr" slopfin-a53-selector.elf
out="$root/dist/native-pkg"
mkdir -p "$out"
cp payloads/a53-selector/slopfin-a53-selector.elf "$out/"
git -C "$ppr" archive --format=tar.gz --prefix="ppr-patch-${revision:0:12}/" \
    -o "$out/ppr-patch-${revision:0:12}-source.tar.gz" "$revision"
echo "Built dist/native-pkg/slopfin-a53-selector.elf from ppr-patch ${revision:0:12}."
