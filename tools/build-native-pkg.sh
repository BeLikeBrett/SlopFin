#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
command -v cargo >/dev/null || { echo 'Native packaging needs Rust/Cargo; see docs/NATIVE_FPKG.md.' >&2; exit 2; }
engine="$root/.deps/fpkg/ps5upload"
revision=a364f7a473bfd6aa5582e7474bfce54fb88a0f5a
mkdir -p "$(dirname "$engine")" "$root/dist/native-pkg"
if [[ ! -d $engine/.git ]]; then
    git clone --no-checkout https://github.com/phantomptr/ps5upload.git "$engine"
fi
if [[ ! -f $engine/engine/Cargo.toml || $(git -C "$engine" rev-parse HEAD 2>/dev/null || true) != "$revision" ]]; then
    git -C "$engine" fetch origin "$revision"
    git -C "$engine" checkout --detach "$revision"
fi
cargo build --locked --release --manifest-path "$engine/engine/Cargo.toml" \
    -p ps5upload-fpkg --example fpkg_build --example fpkg_verify
cargo build --locked --release --manifest-path "$root/tools/native-pkg/Cargo.toml" \
    --target-dir "$root/build/native-pkg-check"
work=$(mktemp -d "$root/build/native-pkg-work.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
mkdir "$work/source" "$work/output"
python3 - "$work/source" <<'PY'
import hashlib, json, os, re, shutil, sys
from pathlib import Path
param = json.loads(Path('sce_sys/param.json').read_text())
source = Path('dist') / param['titleId']
assert (source / 'eboot.bin').is_file(), 'Build the app first (make fpkg).'
shutil.copytree(source, sys.argv[1], dirs_exist_ok=True)
stage = Path(sys.argv[1])
test_title = os.environ.get('NATIVE_PKG_TITLE_ID')
if test_title:
    if not re.fullmatch(r'PPSA[0-9]{5}', test_title):
        raise SystemExit('NATIVE_PKG_TITLE_ID must be PPSA followed by five digits.')
    param['contentId'] = param['contentId'].replace(param['titleId'], test_title)
    param['titleId'] = test_title
    param['conceptId'] = test_title[4:]
    language = param['localizedParameters']['defaultLanguage']
    param['localizedParameters'][language]['titleName'] = 'SlopFin Package Test'
    (stage / 'sce_sys/param.json').write_text(json.dumps(param, indent=2) + '\n')
for item in stage.rglob('*'):
    if item.is_symlink():
        raise SystemExit(f'Package source must not contain symlinks: {item}')
    if item.name == 'right.sprx':
        raise SystemExit('License-free homebrew must not bundle a proprietary rights module.')
manifest = {p.relative_to(stage).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(stage.rglob('*')) if p.is_file()}
(stage.parent / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print('Package input:', len(manifest), 'files; title', param['titleId'])
PY
# Pin the scene-compatible layout even if unrelated experiments set overrides.
unset PS5UPLOAD_FPKG_IMAGE_MODE PS5UPLOAD_FPKG_PARAM_FILE PS5UPLOAD_FPKG_FW
unset PS5UPLOAD_FPKG_KRAKEN_STORE PS5UPLOAD_FPKG_META_CODEC PS5UPLOAD_FPKG_CHUNKS
export PS5UPLOAD_FPKG_KRAKEN=1 PS5UPLOAD_FPKG_LEVEL=balanced
binaries="$engine/engine/target/release/examples"
"$binaries/fpkg_build" "$work/source" "$work/output" 2>&1 | tee "$work/build.txt"
packages=("$work/output/"*.pkg)
[[ ${#packages[@]} == 1 && -f ${packages[0]} ]] || { echo 'Expected exactly one package.' >&2; exit 2; }
"$binaries/fpkg_verify" "${packages[0]}" | tee "$work/verification.txt"
"$root/build/native-pkg-check/release/slopfin-package-check" \
    "${packages[0]}" "$work/source" | tee -a "$work/verification.txt"
name=$(basename -- "${packages[0]}")
cp -- "${packages[0]}" "$root/dist/native-pkg/$name.tmp"
mv -- "$root/dist/native-pkg/$name.tmp" "$root/dist/native-pkg/$name"
cp -- "$work/source-manifest.json" "$root/dist/native-pkg/$name.source-manifest.json"
cp -- "$work/verification.txt" "$root/dist/native-pkg/$name.verification.txt"
cp -- "$work/build.txt" "$root/dist/native-pkg/$name.build.txt"
(cd "$root/dist/native-pkg" && sha256sum "$name" > "$name.sha256")
echo "Built dist/native-pkg/$name. Console installation and launch are separate checks."
