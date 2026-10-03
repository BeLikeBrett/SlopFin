# Notices

## Native build dependencies

The application build uses LLVM/Clang/lld, zlib 1.3.2, and the public
[PS5 payload SDK](https://github.com/ps5-payload-dev/sdk). The bootstrapper
downloads SDK v0.42 after verifying SHA-256
`8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da`.
It downloads zlib 1.3.2 from the upstream source archive after verifying
SHA-256 `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16`
and compiles its static archive locally. Both dependencies remain under ignored
`.deps/native/`, retain their upstream licenses, and are not distributed by
this repository. No Sony SDK file is included.

Target C++ compilation uses the LLVM libc++ headers distributed by the public
SDK. Those headers retain the Apache-2.0 WITH LLVM-exception license recorded
upstream. The application statically links the SDK's libc++, libc++abi and libunwind
archives. They are copied to ignored `.deps/native/cxx/` during the build,
not stored in this repository. Preserve their upstream license notices when
redistributing linked artifacts. Copies of the upstream LLVM license texts are
packaged under `assets/licenses/` ([LLVM 18.1.0](https://github.com/llvm/llvm-project/tree/llvmorg-18.1.0)).

The project’s PS5 ELF converter and FSELF writer are independently authored
GPL-3.0-or-later code. SharpProspero was a useful public format reference during
development but is not fetched, copied, linked, or required by the build.

## Host preview and tests

The Linux preview links SDL2, libcurl and zlib supplied by the build host.
Host model tests use the C++ standard library; network fixtures use Python and
OpenSSL. The retired boilerplate renderer/GoogleTest suite is not required.
These host libraries and test tools are not linked into PS5 artifacts.

## Optional PacBrew dependencies

When selected through `PACBREW_*` build variables, the build downloads the prebuilt ports image
from [ps5-payload-dev/pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo)
release `v0.40.2`, verifies its published SHA-256, and extracts only the
`target/user/homebrew` prefix under ignored `.deps/pacbrew/`. It does not
replace the pinned SDK or install files globally. PacBrew recipes and every
linked third-party library retain their upstream licenses; applications must
review those terms before redistribution.

## Optional UFS2Tool dependency

When `.ffpkg` output is requested, the platform bootstrapper fetches
[SvenGDK/UFS2Tool](https://github.com/SvenGDK/UFS2Tool) at commit
`b5307a60d5b4e3a68ba680e0e33cfadf05017c77` into the ignored
`.deps/UFS2Tool` cache and builds it with the host .NET SDK. UFS2Tool is
BSD-2-Clause software and is not distributed by this repository.

## Optional MkPFS dependency

When `.ffpfsc` output is requested, the platform bootstrapper fetches
[PSBrew/MkPFS](https://github.com/PSBrew/MkPFS) at commit
`6cb8313dfe0c988ac52617794553f343243d3a56` into the ignored `.deps/MkPFS`
cache and installs its Python dependencies into an ignored virtual environment
there. MkPFS and its dependencies retain their own licenses and are not
distributed by this repository.

## Independently authored runtime shim

`tooling/native/libc_builder.cpp` and the manifests under
`tooling/native/runtime/` are independently authored for this project and
licensed under GPL-3.0-or-later. The generated `runtime/libc.prx` contains
project-authored compatibility stubs, startup code, and semantic loader
metadata. It contains no Sony runtime implementation.

Original ps5-native-app-boilerplate code is Copyright (C) 2026
BlackBearReloaded and licensed under GPL-3.0-or-later. Source and script files
carry matching SPDX identifiers.

## Original presentation assets

The BlackBear icon, selection artwork, and default selection track
`sce_sys/snd0.at9` are original assets supplied by BlackBearReloaded, Copyright
(C) 2026 BlackBearReloaded, and distributed under GPL-3.0-or-later. The track
is titled `Night Drive`.

No proprietary runtime module, encryption key, or game file is included.

## SlopFin additions

- `sce_sys/icon0.png` and `assets/jellyfin-icon.svg` are the Jellyfin project
  icon from https://github.com/jellyfin/jellyfin-ux, copyright the Jellyfin
  contributors, licensed Creative Commons Attribution-ShareAlike 4.0
  International. SlopFin is an unofficial client and is not affiliated with or
  endorsed by the Jellyfin project.
- The hardware video decode backend derives from ProsperoTV
  (https://github.com/blackbearreloaded/ProsperoTV), copyright BlackBearReloaded,
  licensed GPL-3.0-or-later.

## Optional software audio experiment

`src/iec61937.hpp` follows the IEC 61937 burst layout as written by FFmpeg's
`libavformat/spdifenc.c` (LGPL-2.1-or-later, the FFmpeg developers); it is an
independent implementation checked byte for byte against that muxer's output.

`SOFTWARE_AUDIO=1` links a minimal build of FFmpeg 9.0.1 libavcodec,
libavutil and libswresample from https://ffmpeg.org/releases/ffmpeg-9.0.1.tar.xz.
Copyright the FFmpeg developers; this configuration reports LGPL-2.1-or-later
(no GPL/nonfree FFmpeg options enabled). Upstream license texts remain in
`.deps/audio/ffmpeg-9.0.1/COPYING*`. `tools/build-software-audio.sh` records the
archive SHA-256, full build configuration and two generated configuration
corrections for unavailable runtime time functions. No upstream source is
patched. Allocator hooks and the probe are project-authored GPL-3.0-or-later.

The verified release signing fingerprint is
`FCF986EA15E6E293A5644F10B4322F04D67658D8`. This optional dependency is not used
by default. When redistributing an experimental linked binary, include the
app's corresponding source, these build instructions, FFmpeg's corresponding
source and license texts; preserve recipients' ability to rebuild the binary.

## Fonts

- **Noto Sans** (`assets/font-regular.ttf`, `font-medium.ttf`, `font-bold.ttf`):
  Copyright 2022 The Noto Project Authors
  (https://github.com/notofonts/latin-greek-cyrillic), SIL Open Font License
  1.1. It shipped without a notice before this entry; the licence text is now
  packaged as `assets/OFL-NotoSans.txt`.
- **Atkinson Hyperlegible** (`assets/font-readable-regular.ttf`,
  `font-readable-bold.ttf`), the "Easy to read" subtitle font: Copyright 2020
  Braille Institute of America, Inc., SIL Open Font License 1.1, from
  https://github.com/google/fonts/tree/main/ofl/atkinsonhyperlegible. Licence
  text packaged as `assets/OFL-AtkinsonHyperlegible.txt`. Unmodified.


## Image libraries

- `vendor/stb_image.h` (decoding artwork) and `vendor/stb_image_write.h`
  (re-encoding a chosen profile picture as a small JPEG) are by Sean Barrett,
  https://github.com/nothings/stb, public domain or MIT at the user's choice. Full notices are packaged in `assets/licenses/`.

- `vendor/stb_truetype.h` (font rasterization) is also from stb and retains its
  public-domain/MIT notice in `assets/licenses/stb_truetype.h.txt`.

## Native package tooling

Optional native packages are built with the public GPL-3.0 PS5Upload FPKG
engine (https://github.com/phantomptr/ps5upload), pinned in
`tools/build-native-pkg.sh`. Its copyright and license remain in the dependency
checkout. The project-authored verification helper is GPL-3.0-or-later. The
packaging tool is a host build dependency and is not linked into SlopFin.
No proprietary `right.sprx` or publishing SDK is redistributed.
