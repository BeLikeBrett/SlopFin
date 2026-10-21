# Native PS5 package

Native packages are on hold: releases ship the folder download only, and
this route is kept for when console package support matures. What follows is
the state reached on 2026-10-19, for building and testing from source.

A native `.pkg` carries the same application as the folder. A package installs through Sony's package
installer and appears under **Media** on the home screen. The folder needs a
homebrew mount service such as ShadowMount instead. Packaging does not change
playback quality; native packages also need the A53 selector described below.

The working route was established on 2026-10-19, on firmware 8.20, under the
separate test identity PPSA99004. The investigation behind it is in the
[2026-10-19 notes](PKG_INVESTIGATION_2026-10-19.md).

## Validation status

| Check | Result |
| --- | --- |
| LibProspero plaintext/no-auth package, media category: install | Passed on 8.20 (PPSA99004); installed image matches the package |
| Launch with kstuff-lite 1.11 and the ppr-patch A53 selector | Passed on 8.20 |
| Shared settings recovery, saved server and sign-in | Passed |
| HTTPS (bundled CA), Home, Discover and Jellyseerr requests | Passed |
| Version display | Passed (compiled in; the package cannot read its own `param.json`) |
| Playback: HEVC hardware decode, E-AC-3 bitstream | Passed; ~24 fps, 0 dropped frames, no audio underruns |
| Launch without the A53 selector | Not tested; expected to stall in PPR mount (see below) |
| In-app package update | Not yet validated on a console; see [updates](#in-app-updates) |
| Avatar picker, subtitles, USB paths | Not yet checked in a package |
| Firmware other than 8.20 | Not tested |

Earlier routes did not launch and are kept for reference only. PS5Upload's
engine, both stored and compressed, installed on 8.20 but stalled in PPR mount
or returned `0x80a40020`/`0x80020060`. That was on 2026-10-16/17, under
PPSA99002/PPSA99003. Changing to the game category did not help.

## What a package needs on the console

1. **A kernel with plaintext package support.** Official
   [kstuff-lite](https://github.com/EchoStretch/kstuff-lite/releases) 1.11 was
   tested. kstuff hands plaintext packages to the kernel with sentinel key
   indices that only a patched A53 understands.
2. **The A53 selector**, from drakmor's
   [ppr-patch](https://github.com/drakmor/ppr-patch), loaded once per boot
   before any native package starts. Without it, a package is expected to hang
   while mounting.

   Upstream publishes source only. `tools/build-a53-selector.sh` builds
   `slopfin-a53-selector.elf` from the pinned ppr-patch revision, with that
   revision's source alongside it. The payload runs upstream's
   `--install --idle`. Upstream does the rest:
   - it refuses any firmware without an exact verified profile (55 layouts);
   - it leaves an already-installed selector alone;
   - it reports the result as a console notification.

   Send it to your payload loader (elfldr, port 9021) from the home screen, or
   add it to Payload Manager's autoload after kstuff. Do not load it while a
   native package is starting or running.
3. **An installer for finalized PS5 packages**, such as
   [PS5Upload](https://github.com/phantomptr/ps5upload). An accepted install
   request alone does not prove that content landed; check that the title
   appears.

The folder and the release package share PPSA99001. Choose one install method
and remove the other copy first. Settings live in `/data/slopfin/config.json`
and are kept by both.

## Sandbox differences

These are why some code paths differ between a package and the folder build:

- A package starts without `/data`. The storage helper
  (`assets/slopfin-storage.bin`) changes the process root before settings load.
- `_openat` returns `EPERM`, and `dup` fails after the root change.
  `/mnt/sandbox` and `app0/sce_sys` are root-only. `src/app_files.hpp` reads
  the app's own files (CA bundle, helper payloads) into memory at startup,
  before any of that applies.
- The package cannot read `sce_sys/param.json`. The version shown in Settings
  is compiled in from it by `tools/build.sh`. An unreadable `param.json` is
  also how the app detects that it runs as a package.
- Never call `fstatfs`: the console's structure is larger than the SDK's, and
  the call smashed the stack.

## In-app updates

An installed package updates from the release's `SlopFin-<version>.pkg`.
The steps are:

1. The app downloads the package to `/data/slopfin/update/` and checks its
   SHA-256 against GitHub's digest.
2. It hands `assets/slopfin-pkg-update.bin` a request naming its process,
   title, package size and digest, and the new version.
3. The helper verifies the package and that its content ID matches the
   running title, then reports "ready". The app closes.
4. The helper uninstalls the old copy and waits until the title is gone.
   Installing over the copy that had just closed took the whole console
   offline in testing. It then installs the new package from a local-only
   HTTP server.
5. It confirms that the installed version and bytes match, deletes the
   download, and relaunches SlopFin.

This flow has host tests but has not yet completed on a console.

## Build from source

Requirements are the normal native build, plus the .NET 10 SDK. Run from the
repository root:

```sh
make fpkg PS5_CLANG=/usr/bin/clang SOFTWARE_AUDIO=1
bash tools/build-a53-selector.sh
```

`tools/build-native-pkg.sh` fetches UltraPack's wrapper at a pinned revision
into ignored `.deps/`. It verifies the shipped LibProsperoPkg checksum, then
applies `tools/native-pkg/homebrew-patcher`. The patcher stops the library
inserting Sony's `right.sprx`, which must not be redistributed, and removes
its embedded copy. The patched library's checksum is checked too.

The package is built deterministically, so the same input gives the same
bytes. `tools/native-pkg/package-check` then reads back every block, chunk and
file. `compare_libprospero.py` compares each extracted file with the source.

Output goes to `dist/native-pkg/`:

- `SlopFin-<version>.pkg`
- its checksum, verification report and comparison report
- `slopfin-a53-selector.elf`, with the ppr-patch source tarball

CI does not build or publish these while native packages are on hold.

For an isolated console test that leaves the normal title alone:

```sh
NATIVE_PKG_TITLE_ID=PPSA99004 bash tools/build-native-pkg.sh
```

That gives a separate entry named SlopFin Package Test. It shares the saved
settings, so close SlopFin before testing. `NATIVE_PKG_VERSION=01.000.015`
overrides the package version, for update tests.

`.ffpkg` and `.ffpfsc` are filesystem images for a mount service. They are not
native packages and must not be submitted to Sony's package installer.
