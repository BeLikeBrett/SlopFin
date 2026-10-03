# Native PS5 package

SlopFin provides a folder download and an experimental native `.pkg` download.
Both contain the same application. Packaging does not improve playback quality
or performance. Native packages use Sony's package installer and appear as
installed applications; the folder uses a homebrew mount service.

## Validation status — 2026-10-03

| Check | Result |
| --- | --- |
| Finalized-image container, digests and layout | Passed |
| Full Kraken decode and inner filesystem walk | Passed: 65 blocks, 26 source files |
| Byte comparison with source, including known executable/metadata normalization | Passed |
| Installer registration and installed `app.pkg` | Passed on firmware 8.20, isolated test title PPSA99002 |
| Launch | Pending a fresh session with current kstuff and A53/PPR support |

The first launch returned `0x80020060` from the PPR filesystem mount, before
SlopFin started. The test environment had kstuff-lite 1.10 loaded and the A53
selector in stock/native state. It lacks the newer mount protocol. The official
1.11 payload has been prepared for the next session. No successful native
package launch or package playback is claimed yet. Folder launch and playback
have been tested separately; see [compatibility](COMPATIBILITY.md).

## Install a downloaded package

Use a compatible jailbroken console with current native FPKG support. Firmware
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
in [getting started](GETTING_STARTED.md), then build with `SOFTWARE_AUDIO=1`.
Output is `dist/native-pkg/UP9000-PPSA99001_00-SLOPFIN000000000.pkg`, plus its
checksum, input manifest, build log and verification report.

The build fetches public GPL tooling from PS5Upload revision
`a364f7a473bfd6aa5582e7474bfce54fb88a0f5a` into ignored `.deps/`. It uses
plaintext/no-auth package markers, native Kraken layout and license-free
homebrew data; no proprietary publishing SDK or `right.sprx` is bundled.
The engine adjusts executable signatures/version padding and installed-package
metadata only in the packaged copy. The folder build remains byte-for-byte
unchanged. Our additional verifier reconstructs every compressed block, walks
inner file paths, checks all source files and verifies container artwork.

For an isolated install test that preserves the normal title registration:

```sh
NATIVE_PKG_TITLE_ID=PPSA99002 make fpkg PS5_CLANG=/usr/bin/clang SOFTWARE_AUDIO=1
```

This stages a separate identity named SlopFin Package Test. Use an unused title
ID and close SlopFin before testing; both identities use the same saved settings.
