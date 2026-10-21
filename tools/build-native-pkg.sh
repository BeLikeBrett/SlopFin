#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Packages dist/<title> as a native PS5 .pkg with UltraPack's LibProspero
# wrapper: plaintext/no-auth, media category, no rights module. This is the
# route that installs and launches on firmware 8.20 with the A53 selector.
#
#   NATIVE_PKG_TITLE_ID=PPSA99004  a separate test identity
#   NATIVE_PKG_VERSION=01.000.015  override the package version (update tests)
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
dotnet=${DOTNET:-$(command -v dotnet || echo "$HOME/.dotnet/dotnet")}
[[ -x $dotnet ]] || { echo 'Native packaging needs the .NET 10 SDK; see docs/development/NATIVE_FPKG.md.' >&2; exit 2; }

# Pinned upstream and the exact library it ships; anything else is refused.
ultrapack="$root/.deps/fpkg/ps5-ultrapack"
revision=4962d2dc80fdf77d1d563089fa9ba00b0efac85f
library_sha=934e1d06d33796902ca949ac5abf65833af40a4a4caa5e356847ba1ccdf7486c
patched_sha=0a1943ffbd92b9e08e050c0147e21849bd55f12445613cfd6d953834e934bd3a
if [[ ! -d $ultrapack/.git ]]; then
    mkdir -p "$(dirname "$ultrapack")"
    git clone --no-checkout https://github.com/knutwurst/ps5-ultrapack.git "$ultrapack"
fi
if [[ $(git -C "$ultrapack" rev-parse HEAD 2>/dev/null || true) != "$revision" ]]; then
    git -C "$ultrapack" fetch origin "$revision"
    git -C "$ultrapack" checkout --detach "$revision"
fi
wrapper="$ultrapack/backend/native/src/ffpfsc-pkg-tool"
echo "$library_sha  $wrapper/lib/LibProsperoPkg.dll" | sha256sum -c --quiet

tool="$root/build/ultrapack-tool"
if ! echo "$patched_sha  $tool/LibProsperoPkg.dll" | sha256sum -c --quiet >/dev/null 2>&1; then
    rm -rf -- "$tool"
    "$dotnet" publish "$wrapper/PkgTool.csproj" -c Release -r linux-x64 --self-contained false \
        -p:PublishSingleFile=false -o "$tool" >/dev/null
    "$dotnet" run --project tools/native-pkg/homebrew-patcher -c Release -- \
        "$wrapper/lib/LibProsperoPkg.dll" "$tool/LibProsperoPkg.dll"
    echo "$patched_sha  $tool/LibProsperoPkg.dll" | sha256sum -c --quiet
fi
"$dotnet" build tools/native-pkg/package-check -c Release -o "$root/build/package-check" \
    -p:ProsperoToolDirectory="$tool" >/dev/null

work=$(mktemp -d "$root/build/native-pkg-work.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
identity=$(python3 - "$work/source" <<'PY'
import json, os, re, shutil, sys
from pathlib import Path
param = json.loads(Path('sce_sys/param.json').read_text())
source = Path('dist') / param['titleId']
assert (source / 'eboot.bin').is_file(), 'Build the app first (make fpkg).'
stage = Path(sys.argv[1])
shutil.copytree(source, stage)
staged = json.loads((stage / 'sce_sys/param.json').read_text())
language = staged['localizedParameters']['defaultLanguage']
title = os.environ.get('NATIVE_PKG_TITLE_ID')
if title:
    if not re.fullmatch(r'PPSA[0-9]{5}', title) or title == staged['titleId']:
        raise SystemExit('NATIVE_PKG_TITLE_ID must be a different PPSA title.')
    staged['contentId'] = staged['contentId'].replace(staged['titleId'], title)
    staged['titleId'] = title
    staged['conceptId'] = title[4:]
    staged['localizedParameters'][language]['titleName'] = 'SlopFin Package Test'
version = os.environ.get('NATIVE_PKG_VERSION')
if version:
    if not re.fullmatch(r'[0-9]{2}\.[0-9]{3}\.[0-9]{3}', version):
        raise SystemExit('NATIVE_PKG_VERSION must look like 01.000.015.')
    staged['contentVersion'] = version
# The folder's media category is kept: it launches as a package and files
# under Media, with the same memory budget as the folder.
(stage / 'sce_sys/param.json').write_text(json.dumps(staged, indent=2) + '\n')
for item in stage.rglob('*'):
    if item.is_symlink():
        raise SystemExit(f'Package source must not contain symlinks: {item}')
    if item.name == 'right.sprx':
        raise SystemExit('License-free homebrew must not bundle a proprietary rights module.')
v = staged['contentVersion'].split('.')
display = f"{int(v[0])}.{int(v[1]) * 1000 + int(v[2]):02d}"
suffix = '' if not title else '-' + title
print(staged['contentId'], staged['titleId'], staged['contentVersion'],
      f'SlopFin-{display}{suffix}.pkg', staged['localizedParameters'][language]['titleName'],
      sep='\t')
PY
)
IFS=$'\t' read -r content_id title_id version name title_name <<< "$identity"
mkdir "$work/package"
"$dotnet" "$tool/ffpfsc-pkg-tool.dll" build "$work/source" "$work/package" \
    --content-id "$content_id" --title-id "$title_id" --title "$title_name" \
    --version "$version" --deterministic -j 2 > "$work/build.txt" 2>&1 ||
    { cat "$work/build.txt" >&2; exit 1; }
packages=("$work/package/"*.pkg)
[[ ${#packages[@]} == 1 && -f ${packages[0]} ]] || { echo 'Expected exactly one package.' >&2; exit 2; }
package=${packages[0]}

# Read it all back, then compare every extracted file with the source.
"$dotnet" "$root/build/package-check/PackageCheck.dll" "$package" > "$work/verification.json"
"$dotnet" "$tool/ffpfsc-pkg-tool.dll" extract-inner "$package" "$work/extracted" > "$work/extract.txt" 2>&1
python3 tools/native-pkg/compare_libprospero.py "$package" "$work/source" "$work/extracted" \
    > "$work/comparison.json"

out="$root/dist/native-pkg"
mkdir -p "$out"
cp -- "$package" "$out/$name.tmp"
mv -- "$out/$name.tmp" "$out/$name"
cp -- "$work/verification.json" "$out/$name.verification.json"
cp -- "$work/comparison.json" "$out/$name.comparison.json"
(cd "$out" && sha256sum "$name" > "$name.sha256")
echo "Built dist/native-pkg/$name ($title_id $version). Install and launch are separate checks."
