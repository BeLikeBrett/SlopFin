# Project constraints

Read the [compatibility guide](docs/COMPATIBILITY.md) and
[architecture](docs/development/ARCHITECTURE.md) before changing platform code.
Keep implemented behavior separate from tested hardware results.

Local reference checkouts may exist at `../boilerplate/`,
`../reference-prosperotv/` and `../research/`. They are read-only upstream
references: never edit them. All project work belongs in this repository.
They are not required to clone or build the public project. Provenance and
public upstream links are in [NOTICE.md](NOTICE.md).

## Build and deployment

- On this development host, select `PS5_CLANG=/usr/bin/clang`; its installed
  Clang works. Do not install an older compiler merely to satisfy a filename.
- The shared development console uses `SOFTWARE_AUDIO=1`. Always deploy it with
  `make deploy PS5_CLANG=/usr/bin/clang SOFTWARE_AUDIO=1` so the optional CPU
  decoder is preserved. Read the build guide before changing that dependency.
- Console helpers resolve `PS5_HOST` from the environment or ignored `.env`.
  Do not hardcode developer addresses, account IDs, credentials or report hosts.
- Preserve the configured HTTPS endpoint, signed-in account and preferences.
  Do not exercise dashboard actions, password changes or uploads against the
  production server without explicit authorization and a reversible test.
- Fully close the app before replacing its files. The folder and native package
  share a title ID; use a separate test identity when validating installation.
- Do not infer successful launch from an accepted install request or a format
  check. Native FPKG needs compatible kernel and A53/PPR support, separately
  from the directory loader. Do not load new firmware offsets by assumption.

## Memory, threads and runtime

- Image-sized and larger allocations use `bigalloc::allocate`. The runtime
  heap is small; flexible memory has substantially more capacity.
- Decoder direct memory is **type 12**. `0x32` and `0x33` are mapping
  protections, not allocation types; confusing them can take the console down.
- Keep video decode at pipeline depth **3**. Depth 1 serializes difficult 4K
  streams. Read a frame slot only while the decoder is recorded as holding it.
- Background threads use `images::spare_cores()`. They inherit the creator's
  affinity and otherwise contend with the render thread. Never spin workers
  on short sleeps.
- Do not write large files from the render thread on a timer. Capture and
  trace output are explicitly requested through markers and may stall a frame.
- Failure paths must park rather than return from native `main`. Shutdown
  belongs to the main loop; do not present after releasing VideoOut.
- Use POSIX reads for file contents. This runtime's `fread` has returned zero
  bytes for files that other interfaces read correctly.
- Dashboard uploads and whole-file reads must fit the constrained heap. Shrink
  and re-encode images; stream log tails or use flexible memory for large data.
- SlopFin is sandboxed. `/data` is available; `/user` and USB paths require the
  explicit picker payload `assets/slopfin-sandbox.bin`. FTP visibility does not
  establish that the application can read a path.

## Playback and interface

- HDR follows the console setting through `sceVideoOutGetOutputStatus` byte 4.
  Do not reintroduce a user-facing marker for it. Preserve the console's HDR
  setting after experiments; the shared console uses registry `VIDEOOUT_hdr=0`.
  A successful HDR buffer change does not prove the TV received HDR.
- Never scale, mix, reorder or capture-write an IEC 61937 bitstream ring. Re-run
  the FFmpeg byte comparison when changing `src/iec61937.hpp`.
- Autoplay decisions live in `src/autoplay.hpp` and run from drawing, so an
  input-owning prompt cannot prevent an episode ending. Playback overlays must
  participate in the frame-reuse test or they will not appear.
- Text/button focus uses shared dark surfaces, cyan outlines and visible-glyph
  alignment. Apply the same tokens and motion to Settings, Dashboard, login,
  profile and sidebar. Keep focused labels bright and bounds stable.
- Text entry uses the PlayStation system keyboard. Host preview captures cannot
  demonstrate its appearance, sound or final placement.
- Library names and IDs come from Jellyfin. Keep long sidebars, empty libraries,
  missing images, failed requests and large episode result sets navigable.

## Verification

Run `make test`, `make host` and `make lint` as appropriate to the change.
Native builds must have zero static FSELF errors. Visual changes require a
reviewed capture. Decoder, HDR, audio, memory and timing claims require console
validation; the Linux preview cannot establish them.

Use documented layouts, reference decoders or known input values as comparison
authorities. Two copies of the same output can share the same mistake.
Inspect test harness behavior before accepting measurements.

- Captures stall their own frame. Do not capture while measuring focus or
  playback timing; write `/data/slopfin-input` directly instead.
- Judge frame timing by its distribution. At depth 3, the trace's `decode`
  field measures submission, not completed decode; use completion counts and
  shown FPS. Trace markers can drop or reorder, so absence is not proof.
- Read crash reports first and symbolize only against the matching build ELF.
  Keep frame pointers and line tables. A stale process can remain after a crash.
- Confirm the startup build stamp before judging deployment. The loader may
  rewrite `eboot.bin`; remote byte equality is not a reliable build check.
- Caption captures with their environment and limits. Remove private names,
  addresses and credentials before publishing logs or screenshots.

Controller, capture, launch, timing and crash helpers are documented in
[testing](docs/development/TESTING.md), [Linux preview](docs/development/HOST_BUILD.md) and
[diagnostics](docs/development/DIAGNOSTICS.md). The app closes on `/data/slopfin-quit`.
`tools/launch.sh` is for developer iteration; a crashed instance may be killed
before relaunch, so use the orderly close path where possible.

## Source and licensing

Use the existing Allman C++ formatting. Keep comments for constraints and
non-obvious decisions; discussion and investigation notes belong in docs or
review descriptions. Preserve copyright/SPDX headers and the ProsperoTV
GPL-3.0-or-later attribution. Dependencies retain their own licenses.
Do not commit credentials, caches, generated SDK archives or proprietary
modules. Binary distributions include the applicable notices and corresponding
source; optional FFmpeg builds also distribute the matching FFmpeg source.
