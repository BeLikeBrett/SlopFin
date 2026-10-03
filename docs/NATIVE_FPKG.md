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
| Launch | Blocked at PPR mount after kstuff-lite 1.11 and an exact-profile A53 selector install; no app startup |

The original launch returned `0x80020060` from the PPR filesystem mount, before
SlopFin started, with kstuff-lite 1.10 and stock A53 code. After rebooting with
the verified official 1.11 release, its plaintext mount protocol was present.
The selector was installed from ppr-patch revision
`fd4c8224563130e9698d3b2b2f44712826ceb525`, using the exact retail 8.20 profile;
all patch writes passed readback. The non-time-accelerated installer was used.

The next launch created `PPSA99002-app0` and `PPSA99002-app0-nest` mount entries,
but the package mount did not finish. SlopFin never appeared in the process
list. Subsequent file operations and Remote Play authentication timed out,
although the payload loader and read-only kernel log probes remained responsive.
This is a console package-mount blocker, not a successful launch. Do not treat
this download as a working replacement for the folder build yet. Do not modify
the selector while that mount is outstanding; recovery requires a console restart.

A repeat test booted the official 1.11 source with observation counters enabled
and reinstalled the exact selector after its preflight passed. Two snapshots
confirmed the same stalled state:

| Runtime observation | Result |
| --- | --- |
| Plaintext package header validation | Passed; one profile match |
| `verifyImage` mailbox | One request, emulated successfully; no malformed outputs |
| G6 key-index interception | Two traps, both applied; no index or copy errors |
| Mount lifecycle | Hook stage 9; one key pair outstanding; no cleanup or return |
| SlopFin startup | Not reached |

Stage 9 means the one-shot session was armed before calling the original
package-mount function. These counters narrow the failure to the mount/read
pipeline after header validation; they do not identify a specific A53 queue
failure or prove runtime compatibility. A separate installed-native-app check
was rejected before PPR mounting, so native regression after the selector
install remains unverified. The diagnostic payload was temporary; the normal
official 1.11 autoload entry was restored and read back before the repeat launch.

Folder launch and playback were tested separately. The original PPSA99001
folder, saved HTTPS account settings and previous autoloader backup were
preserved. See [compatibility](COMPATIBILITY.md) for the app's playback results.

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
