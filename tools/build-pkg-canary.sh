#!/usr/bin/env bash
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
# Build a source-auditable startup probe locally. This never contacts a console.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
sdk="$root/.deps/native/ps5-payload-sdk"
[[ -f $sdk/target/lib/libkernel.so && -x $sdk/bin/prospero-lld ]] || {
    echo 'The cached public SDK is required; run make deps first.' >&2; exit 2;
}
title=${CANARY_TITLE_ID:-PPSA99005}
[[ $title =~ ^PPSA[0-9]{5}$ && $title != PPSA99001 ]] || {
    echo 'CANARY_TITLE_ID must be an isolated PPSA title, not PPSA99001.' >&2; exit 2;
}
mkdir -p "$root/build"
work=$(mktemp -d "$root/build/pkg-canary.XXXXXX")
mkdir -p "$work/source/sce_sys" "$work/source/sce_module"
[[ -f runtime/libc.prx && -f runtime/libc.prx.sha256 ]] || {
    echo 'The cached clean-room companion runtime is required; run make libc first.' >&2; exit 2;
}
(cd runtime && sha256sum --check --strict libc.prx.sha256)
cp runtime/libc.prx "$work/source/sce_module/libc.prx"
cxx=${HOST_CXX:-clang++}
"$cxx" -std=c++20 -O2 -Wall -Wextra -Werror \
    tooling/native/native_app_builder.cpp tooling/native/self_container.cpp \
    tooling/native/elf_object.cpp tooling/native/sce_module_writer.cpp -lz -o "$work/native-tool"
PS5_PAYLOAD_SDK="$sdk" PS5_CLANG="${PS5_CLANG:-/usr/bin/clang}" sh tooling/prospero-clang18 \
    -std=c11 -O2 -Wall -Wextra -Werror -fno-builtin -ffunction-sections -fdata-sections \
    -c tools/native-pkg/canary/main.c -o "$work/main.o"
"$sdk/bin/prospero-lld" -T tooling/native/ps5-pie.ld --eh-frame-hdr \
    --version-script tooling/native/app-symbols.map -L "$sdk/target/lib" -e _start \
    -o "$work/llvm-pie.elf" "$work/main.o" --as-needed "$sdk/target/lib/libkernel.so"
"$work/native-tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
    --stub-dir "$sdk/target/lib" --module-sdk 0x02000009 --companion-sdk 0x08050001 \
    --file-name eboot.elf > "$work/link.txt"
"$work/native-tool" self --sign --in "$work/eboot.elf" --out "$work/source/eboot.bin" \
    --magic 0xEEF51454 > "$work/sign.txt"
"$work/native-tool" self --inspect --file "$work/source/eboot.bin" > "$work/self-inspection.txt"
"$work/native-tool" self --inspect --file "$work/source/sce_module/libc.prx" > "$work/runtime-inspection.txt"
python3 - "$work" "$title" <<'PY'
import hashlib, json, subprocess, sys
from pathlib import Path
work = Path(sys.argv[1])
title = sys.argv[2]
expected = {'sceKernelOpen', 'sceKernelWrite', 'sceKernelFsync', 'sceKernelClose', 'sceKernelUsleep'}
symbols = subprocess.check_output(['nm', '-D', work / 'llvm-pie.elf'], text=True)
imports = {row.split()[-1] for row in symbols.splitlines() if row.split()}
if imports != expected:
    raise SystemExit('Unexpected canary imports: ' + repr(imports))
if any('integrity: valid' not in (work / name).read_text()
       for name in ('self-inspection.txt', 'runtime-inspection.txt')):
    raise SystemExit('Canary SELF integrity check failed.')
param = {
    'ageLevel': {'default': 0}, 'applicationCategoryType': 0,
    'applicationDrmType': 'free', 'attribute': 0, 'attribute2': 0, 'attribute3': 0,
    'conceptId': title[4:], 'contentBadgeType': 1,
    'contentId': f'UP9000-{title}_00-SLOPFINCANARY000', 'contentVersion': '01.000.000',
    'downloadDataSize': 256, 'gameIntent': {'permittedIntents': [{'intentType': 'launchActivity'}]},
    'localizedParameters': {'defaultLanguage': 'en-US', 'en-US': {'titleName': 'SlopFin PKG Startup Canary'}},
    'masterVersion': '01.00', 'pubtools': {'creationDate': '1970-01-01 00:00:00', 'toolVersion': '2.79'},
    'requiredSystemSoftwareVersion': '0x0000000000000000', 'sdkVersion': '0x0000000000000000',
    'titleId': title, 'versionFileUri': '',
}
(work / 'source/sce_sys/param.json').write_text(json.dumps(param, indent=2) + '\n')
(work / 'source/sce_sys/icon0.png').write_bytes(Path('sce_sys/icon0.png').read_bytes())
manifest = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in [
    Path('tools/native-pkg/canary/main.c'), Path('tools/build-pkg-canary.sh'),
]}
manifest.update({str(p.relative_to(work)): hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in sorted((work / 'source').rglob('*')) if p.is_file()})
(work / 'input-manifest.json').write_text(json.dumps({'sha256': manifest, 'imports': sorted(imports)}, indent=2) + '\n')
(work / 'NOT_FOR_INSTALLATION.txt').write_text(
    'Local startup-canary source build only. No console launch compatibility is established.\n'
    'Package verification and explicit authorization are required before live testing.\n')
print('Canary source folder:', work / 'source')
print('Five application kernel imports and both SELF containers checked. No console was contacted.')
PY
