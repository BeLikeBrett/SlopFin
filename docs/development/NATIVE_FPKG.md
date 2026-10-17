# Native PS5 package

Public releases currently provide the folder download. Native `.pkg` builds
are available from source for testing; no launch-verified PKG is published.
Both formats contain the same application. Packaging does not improve playback quality
or performance. Native packages use Sony's package installer and appear as
installed applications; the folder uses a homebrew mount service.

## Validation status

| Check | Result |
| --- | --- |
| 1.10 stored package: container, digests and layout | Passed offline, 2026-10-16 |
| Stored package: all blocks and inner filesystem walk | Passed; every block read, zero compressed |
| Byte comparison with source, including executable/metadata normalization | Passed; every source file compared |
| Earlier compressed package: installer registration | Passed on firmware 8.20, isolated test title PPSA99002 |
| Earlier compressed package: launch | Blocked at PPR mount; no app startup |
| 1.10 stored package: console install / launch | Pending hardware test |

The earlier compressed package stalled inside PPR mounting before the app process started.
Header validation and `verifyImage` emulation pass; two G6 key-index hooks
apply, but the mount does not return. Tests used official kstuff-lite 1.11
and ppr-patch revision `fd4c8224563130e9698d3b2b2f44712826ceb525`
with the exact retail 8.20 profile. This does not establish compatibility
with other firmware or identify the cause of the stalled mount.

The current build defaults to **stored blocks**, following PS5Upload's
[uncompressed-package fallback](https://github.com/phantomptr/ps5upload/commit/ab8cbcdd44922df2f11a1df42e5a67bbb7c0f34d)
for consoles that install compressed packages but cannot launch them. Offline
validation does not establish that this resolves SlopFin's 8.20 mount stall.

Use the folder build for normal use. A stalled package mount can require a
console restart; do not change selectors while package I/O is outstanding.
[Compatibility](../COMPATIBILITY.md) covers the separate folder playback results.

## Installation requirements (experimental)

Until the mount blocker above is resolved, use the folder ZIP for normal use.
Native testing requires a compatible jailbroken console with current FPKG
support. Firmware
8.20 is the current SlopFin test target. Support ranges claimed by payloads do
not constitute SlopFin hardware validation on other firmware.

1. Check the [kstuff release notes](https://github.com/EchoStretch/kstuff-lite/releases)
   and the [A53/PPR selector documentation](https://github.com/drakmor/ppr-patch).
   Native plaintext packages need both matching kernel mount logic and the A53
   selector. A53 installation requires the console's package I/O to be idle.
   Follow those projects' firmware and setup instructions.
2. Use [PS5Upload](https://github.com/phantomptr/ps5upload) or another installer
   explicitly supporting finalized PS5 FIH packages. Its stream-install path
   installed the SlopFin test package successfully. An accepted install request
   alone does not prove that content landed; check for the installed title.
3. Install the SlopFin `.pkg`, then launch its home-screen entry. On first use,
   enter your Jellyfin server, then sign in. Settings are stored independently
   at `/data/slopfin/config.json`.

The folder and release package share PPSA99001. Choose one installation method;
do not register both copies of that title simultaneously. Back up account
settings before changing methods. The automated folder deploy command does not
install native packages or change your jailbreak.

`.ffpkg` and `.ffpfsc` are filesystem images for a mount service. They are not
native packages and must not be submitted to Sony's package installer.

## Build from source

Install Rust/Cargo (tested with 1.99.0) alongside the normal native build tools.
Run from the repository root:

```sh
make fpkg PS5_CLANG=/usr/bin/clang
```

For the optional software-audio build, first follow the dependency preparation
in the [build guide](BUILD.md), then build with `SOFTWARE_AUDIO=1`.
Output is `dist/native-pkg/UP9000-PPSA99001_00-SLOPFIN000000000.pkg`, plus its
checksum, input manifest, build log and verification report.

The build fetches public GPL tooling from PS5Upload revision
`c16e5ab26463c238cc1265d0c2d5e2c32b349d59` (v7.0.31) into ignored `.deps/`. It uses
plaintext/no-auth package markers, native block layout with stored blocks and license-free
homebrew data; no proprietary publishing SDK or `right.sprx` is bundled.
The engine adjusts executable signatures/version padding and installed-package
metadata only in the packaged copy. The folder build remains byte-for-byte
unchanged. The additional verifier reads every block, rejects compressed blocks in stored mode, walks
inner file paths, checks all source files and verifies container artwork.

For an isolated install test that preserves the normal title registration:

```sh
NATIVE_PKG_TITLE_ID=PPSA99002 make fpkg PS5_CLANG=/usr/bin/clang SOFTWARE_AUDIO=1
```

This stages a separate identity named SlopFin Package Test. Use an unused title
ID and close SlopFin before testing; both identities use the same saved settings.

For a compression comparison, set `NATIVE_PKG_COMPRESSION=balanced` (also accepts
`fast` or `smallest`). `stored` is the default. Compression only changes package
size; it does not improve playback. Keep these comparisons local until the
corresponding package has been installed and launched on the target firmware.
