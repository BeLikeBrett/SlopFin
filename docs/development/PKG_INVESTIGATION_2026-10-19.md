# PKG offline investigation — 2026-10-19

Native install-and-launch remains unresolved on the tested PS5 firmware 8.20.
The initial build-only pass made no console requests. The follow-up below used
read-only FTP and Payload Manager queries. Neither pass installed or launched
anything or changed console files, configuration or payloads.
No candidate from this pass should be published or treated as a working app.

## Corrected hypotheses

- **LibProspero encryption is not an established escape from the PPR hang.**
  UltraPack's detailed [builder notes](https://github.com/knutwurst/ps5-ultrapack/blob/4962d2dc80fdf77d1d563089fa9ba00b0efac85f/backend/native/src/ffpfsc-pkg-tool/README.md#firmware-compatibility)
  say its launch-tested output uses `PlaintextNoAuth`. Its encrypted output
  round-tripped offline but failed at console launch. This is the author's
  report on 11.60, not a SlopFin result on 8.20.
- **The legacy RSA fake-key test cannot validate the PS5 PPR mount path.**
  Official [v1.11 fpkg.c](https://github.com/EchoStretch/kstuff-lite/blob/v1.11/ps5-kstuff/uelf/fpkg.c)
  returns from the encrypted PPR `verifyImage` case before calling
  `pfs_derive_fake_keys`. That function services the different legacy
  `verifySuperBlock` layout. Its 2048-bit unwrap must not be described as
  proof that kstuff can unwrap a PS5 PPR image key.
- **An obsolete protocol does not explain all the prior crashes.** Local
  October 3 records already document a fresh official 1.11 boot, a recognized
  plaintext protocol, successful `verifyImage` emulation and both key traps,
  followed by the stalled mount. A later install might have a different
  runtime, but that would require fresh evidence.
- **Stored NAPS blocks are not established as safer launch candidates.**
  UltraPack's wrapper explicitly warns that its uncompressed backend failed
  launch on the author's console. SlopFin's earlier stored builds also stalled.
  These reports do not prove every stored layout fails on every console.

An upstream [8.00 freeze report](https://github.com/EchoStretch/kstuff-lite/issues/84)
and [10.01 launch-freeze report](https://github.com/EchoStretch/kstuff-lite/issues/83)
show similar symptoms in other titles. They support investigating the mount
stack; they do not establish the cause of SlopFin's failure.

## Tool identity and bounded binary review

UltraPack is pinned locally at `4962d2dc80fdf77d1d563089fa9ba00b0efac85f`,
under ignored `.deps/fpkg/ps5-ultrapack/`. Its wrapper builds on Linux with the
installed user-level .NET SDK. It reports wrapper 2.2.9 and
LibProsperoPkg 1.2.0, informational build `d7090eb6`.

The pristine and upstream-patched library SHA-256 values match UltraPack's
`lib/SHA256SUMS`. The patched input is
`934e1d06d33796902ca949ac5abf65833af40a4a4caa5e356847ba1ccdf7486c`.
Execution-free managed inspection admitted complete metadata and IL-only code.
A bounded decompiled-code search found the process launch in an optional
`p2d.exe` artwork encoder; native loading covers OpenSSL and optional compression
backends. No network API call sites were found in that search. This is not a
general security certification of the library or its dependencies.

For license-free homebrew, a checksum-gated local Cecil patch disables
`EnsureAboutRightSprx` and removes its embedded `right.sprx` resource. The input
is preserved. Patched-library SHA-256:
`0a1943ffbd92b9e08e050c0147e21849bd55f12445613cfd6d953834e934bd3a`.
This additional modification means the candidate is not an exact reproduction
of UltraPack's hardware-tested pipeline. Both extracted comparison trees are
free of `right.sprx`.

Managed experiment sources remain under ignored `.local/native-pkg/`, following
the repository's rule against checked-in managed code. They are investigation
tools, not a replacement release packager.

## Built artifacts and verification

All artifacts remain in ignored `build/pkg-comparison/`. None were installed.

| Candidate | Package SHA-256 | Library full verification |
| --- | --- | --- |
| Upstream HomebrewTest sample, PPSA99099 | `9ed0efcac6b01014e75ec3167bff7b1736a93a0c5fcc25a2638a6f27920e365b` | 11 outer blocks, 22 NAPS chunks, 4 inner files; zero issues |
| SlopFin 01.000.014, isolated PPSA99004 | `0478e876a3748e00c47fe3617832b4a49e9901345da24dfd19f98762e99bd77d` | 109 outer blocks, 70 NAPS chunks, 28 inner files; zero issues |

The upstream sample is a supplied binary fixture, not a homebrew independently
shown to launch on this console. Its SELF digest also disagrees with the local
inspector after extraction; that separate discrepancy is unresolved. The full
package verifier does not establish SELF integrity. Exclude this sample from
live tests and do not call it a known-good 8.20 canary.

SlopFin's extracted tree preserves 27 staged source files byte-for-byte. The
two SELF files differ only by their PS5 magic and eight bytes of version-trailer
alignment padding. Metadata changes are limited to documented publisher fields;
the staged identity/category/launch intent, HDR attribute and version remain.
The folder input is untouched. `compare_libprospero.py` rejects missing files,
unexpected assets, code changes, symlinks and rights modules, and records input
hashes in `slopfin-payload-comparison.json`.

The normal built-in backend produced the same package bytes with `--mode none`
and explicit `--mode kraken --kraken-backend builtin`: this library's publisher
PFSv2 path still builds NAPS data in both cases. The explicit built-in command
is preferred. Uncompressed comparison outputs are retained separately and are
excluded from any proposed live test.

### Two verifier disagreements resolved or bounded

1. PS5Upload 7.0.31 flags the candidate's PlayGo general digest. Its verifier
   concatenates three entry hashes; this library also includes entry `0x3000`,
   the scenario JSON. An independent Python SHA3 calculation matches all four
   entries exactly. Per-entry, container and outer-block checks also pass.
   This explains the disagreement without changing package bytes.
2. PS5Upload's payload decoder fails with `kraken: chunk too short` on the
   candidate's PFSv2 layout, including its uncompressed comparison. Therefore
   it has **not** independently decoded all NAPS data. The library's full
   verifier and extractor do, and the extracted SlopFin payload is compared
   independently with the staged source. This coverage distinction remains.

The project's SELF inspector initially reported invalid digests because its
extractor assumed version records immediately followed the last segment.
Publisher-normalized files put them at the declared aligned file-size marker.
The extractor now accepts a complete, well-formed trailer there and preserves
the older contiguous form. A regression fixture covers both magics, aligned
and contiguous trailers, and truncation. Reconstructed packaged `eboot.bin`
matches the folder ELF byte-for-byte; both executable and libc integrity checks
pass. No signer or application behavior changed.

### Local checks

- `make test-integration`: passed, including the SELF regression, update
  transactions and 21 Python integration tests. The new comparison tests reject
  a changed code byte, damaged/missing payload, symlink and extra rights module.
- A deliberately flipped byte in outer block 100 was rejected by the library's
  full verifier for both its image checksum and the corresponding PlayGo CRC.
  The damaged package was temporary and removed after the check.
- Changed C++ files pass the installed formatter and clang-tidy with warnings
  treated as errors. `git diff --check` passes.
- `make lint` stops at existing formatting violations in `src/update_package.cpp`,
  `src/ui_common.cpp` and `src/seerr.cpp`; those files were unchanged in this pass.
- The cached A53 patcher's `host-test-ppr` passes. The aggregate upstream test
  target is incomplete in this cache (`kmb_range_patch.c` is absent), and its
  notification test hits a host-compiler format warning. No A53 test here proves
  anything about live queue behavior or the console hang.

## Reproduction on this development host

The upstream source, checked library and local managed projects are already
cached. Do not copy an unreviewed replacement DLL over them. From the app root:

```sh
~/.dotnet/dotnet publish .deps/fpkg/ps5-ultrapack/backend/native/src/ffpfsc-pkg-tool/PkgTool.csproj \
  -c Release -r linux-x64 --self-contained false -p:PublishSingleFile=false -o build/ultrapack-tool
~/.dotnet/dotnet run --project .local/native-pkg/homebrew-patcher -c Release -- \
  .deps/fpkg/ps5-ultrapack/backend/native/src/ffpfsc-pkg-tool/lib/LibProsperoPkg.dll \
  build/ultrapack-tool/LibProsperoPkg.dll
~/.dotnet/dotnet build .local/native-pkg/libprospero-check -c Release -o build/libprospero-check
```

The staged source is a separate copy of `dist/PPSA99001`, with only its package
identity and game-category tuple changed as in `build-native-pkg.sh`. Do not
change the production folder identity. Build to a fresh output directory:

```sh
~/.dotnet/dotnet build/ultrapack-tool/ffpfsc-pkg-tool.dll build \
  build/pkg-comparison/slopfin-source build/pkg-comparison/slopfin-builtin \
  --content-id UP9000-PPSA99004_00-SLOPFIN000000000 --title-id PPSA99004 \
  --title 'SlopFin Package Comparison' --version 01.000.014 \
  --mode kraken --kraken-backend builtin --deterministic -j 2
```

Run the local `LibProsperoCheck.dll` on the package for a full JSON report,
then the upstream wrapper's `extract-inner` into a fresh directory. Audit the
extracted result with the checked-in tool:

```sh
python3 tools/native-pkg/compare_libprospero.py \
  build/pkg-comparison/slopfin-builtin/UP9000-PPSA99004_00-SLOPFIN000000000-A0100-V0100.pkg \
  build/pkg-comparison/slopfin-source build/pkg-comparison/slopfin-extracted
```

## Remaining gates

- The console mount failure needs an evidence-backed change before another
  launch is reasonable. Merely switching packers, compression or category has
  not established one. No new firmware offsets or payloads were installed.
- The original storage recovery required a matching folder copy. The follow-up
  below removes that dependency and adds local tests. Its descriptor binding and
  asset access still need package-runtime validation after mounting is resolved.
- The updater validates and replaces the folder copy, not an installed package.
  Download success cannot be called a successful package update. A package-aware
  installer, identity handoff and restart flow still need implementation and tests.
- Playback, account retention, controller behavior and update installation need
  real package-runtime validation once mounting is justified. No claim of support
  for every PS5 model or firmware follows from these offline results.

The existing folder release remains the usable installation path.

## Follow-up: source canary and read-only runtime audit

The user clarified that only PS4 PKGs have been used; there is no established
native PS5 PKG launch baseline on this console. The user then authorized a live
test **only once confidence in success exceeds 70 percent**. This supersedes
the earlier request for a separate approval. It does not authorize testing below
that threshold or inventing a probability to clear the gate.

### Source-built canary

`tools/native-pkg/canary/main.c` replaces the opaque upstream sample. It calls
only `sceKernelOpen`, `sceKernelWrite`, `sceKernelFsync`, `sceKernelClose` and
`sceKernelUsleep`. It writes `SLOPFIN_PKG_CANARY_V1` once to
`/download0/slopfin-pkg-canary-v1.txt` inside its own sandbox, then sleeps until
the shell terminates it. It has no renderer, networking, libc companion,
constructors, SlopFin settings access or kernel-patching code. A marker, rather
than a screen image, is the intended future evidence of application entry.

`bash tools/build-pkg-canary.sh` builds it locally into a fresh
`build/pkg-canary.*` directory and records input hashes, exact imports and SELF
integrity. `CANARY_TITLE_ID` defaults to isolated PPSA99005 and rejects PPSA99001.
This script has no network or console operations and builds only the input
folder; the reviewed LibProspero wrapper performs the separate package step.

The packaged candidate from `build/pkg-canary.1hk1le` has SHA-256
`c083f8ece10af257ce98d92f834f2007620e86b289a4b0d47575515b3694db24`.
Full verification passes 11 outer blocks, 21 NAPS chunks and all 3 inner files.
The extracted SELF integrity passes and its reconstructed ELF matches the
source-built input. The independent payload comparator passes too. Host tests
cover short writes, open failure, failed/zero writes and an invalid oversized
write result; the integration target passes with this new test included.

This proves source identity and offline package consistency. A smaller program
still has to cross the same PPR mount path that hung before, so it is not proof
of a safe launch.

### Read-only console observations

The boot autoloader still starts nanodns and Payload Manager. Payload Manager
configuration selects `local/kstuff-lite`, and the process snapshot contains
`kstuff.elf`, `shadowmountplus.elf` and `pldmgr.elf`.

Both local and repository copies of kstuff-lite 1.11 are 1,737,080 bytes and
match the previously cached official release, SHA-256
`ab9a6cb4d3b1daf139d4d646e402b1cf569071acd64599c936d7a3a6164dc779`.
Stored bytes and a process name do not establish the current state of every
kernel hook. Payload history also contains kstuff-toggle; its effect on current
patch activation was not queried by executing code.

The existing A53 log describes the October 17 installation. It is not a fresh
selector-state observation. No status payload was sent, no selector was loaded
and no boot configuration was changed. Private snapshots stay under ignored
`.local/native-pkg/read-only-2026-10-19/`.

Two participants in [issue 83](https://github.com/EchoStretch/kstuff-lite/issues/83)
report that drakmor's 1.13 fix 4 resolved their launch freeze. The
[8.00 report](https://github.com/EchoStretch/kstuff-lite/issues/84) remains open,
and a later commenter says the suggested fix did not work for them. These are
leads, not verified 8.20 compatibility. The author's public GitHub and etawen
refs checked here expose `1.11-opt` and `1.12`, not the cited 1.13 build. No
unreviewed replacement payload was downloaded to the console or executed.

### Recovered October 17 comparison

The historical encrypted package was found in ignored `.local/retry-1017/`.
PS5Upload verifies its encrypted outer blocks, and the independent payload
checker reads 66 stored blocks and matches all 27 source files.

LibProspero rejects its CNT wrap and reports a nested NAPS reserved-bits issue,
skipping full payload verification. The CNT disagreement is explained: its
checker requires its own deterministic padding, while the old builder uses
all-FF nonzero PKCS#1 encryption padding. Public re-encryption with
**LibProspero's own modulus** exactly reproduces the old package's stored wrap.
Different deterministic padding is not by itself malformed RSA encryption.
The encrypted reader/NAPS disagreement remains unresolved; do not describe it
as proof of a bad key or of the console's failure cause.

**Confidence gate: not met.** We have better-reviewed input and verified stored
kstuff identity, but no demonstrated mount fix or fresh proof of matching A53
and kernel runtime state. Nothing in these results supports a greater-than-70%
launch-success claim. Do not install or launch either candidate on this basis.


## Follow-up: package-aware storage and asset access

The application now retains an open `/app0` directory descriptor at entry,
before storage or avatar helpers can change its process root. Certificates,
app metadata and helper payloads are opened relative to that descriptor using
libkernel's `_openat`. Each component is opened with `O_NOFOLLOW`; intermediate
components also require `O_DIRECTORY`. This removes the fallback assumption
that a package has a matching `/data/homebrew/PPSA99001` copy. Fonts continue to
load before recovery. Shared settings remain at `/data/slopfin`.

The versioned storage request carries the requesting PID and its open app and
result descriptors. The helper scans valid `PPSAxxxxx_NNN` sandboxes and compares
SDK-resolved vnodes for those descriptors against that sandbox's app/result
paths. It also checks the process's original root against the sandbox directory.
It does not truncate or write a result for an unbound request. A failed root
change restores the original root and jail on a best-effort basis. No new kernel
offsets were introduced; this uses the cached public SDK APIs. The SDK's
`kernel_get_proc_file` resolves the descriptor's data object, which is the vnode
for these vnode-backed descriptors. This is an implementation review, not live
proof of the ABI on every firmware.

Host tests cover retained directory access after renaming/replacing its path,
path traversal and symlink rejection, malformed/overflowed/aliased request
fields, both normal and isolated title IDs, wrong process/descriptor/root
bindings, partial reply writes and rollback after a setter failure. The native
HTTPS ABI fixture now models the retained directory and `_openat` calls.
`make test` passes in full. The software-audio native build passes and both
SELF containers report valid integrity. `make lint` remains blocked by the
same three pre-existing formatting violations in unchanged files. The full
`tools/run_clang_tidy.sh` pass and Linux preview build also complete successfully;
local build reports retain the evidence.

A fresh offline comparison lives at `build/pkg-storage-j_czwn0u/`, under isolated
PPSA99004. Its package SHA-256 is
`0d74fb27ae9b42638d14f056605cc995dc7540692c8ebf6157f5f8830b55ef06`.
Full verification covers 109 outer blocks, 70 NAPS chunks and 28 inner files,
with no reported issues. Independent extraction/comparison preserves all 27
unchanged source files, both executable containers with only the previously
reviewed magic/trailer padding normalization, and expected publisher metadata.
The reconstructed packaged executable exactly matches the reconstruction of
the source SELF. Compared with the compiler's pre-signing ELF, both omit the
same 24-byte, zero-address, zero-memory-size `PT_NOTE` at offset 3537000; 14 of
its bytes are nonzero in the compiler output. No other bytes differ. This
signer behavior predates the storage work and is distinct from packaging.
The checked-in signer was not changed to preserve this note.

### Native update installer review

Payload Manager's `src/app_installer.c` registers its own browser-launcher
folder through `AppInstallTitleDir`/`AppInstallAll`; it is not a native package
replacement implementation. The relevant PS5Upload code is instead
`payload/installer/sony.c`, `jobs.c`, `jobs.h` and
`payload/include/sceAppInstUtil.h` at the cached 7.0.31 revision.

That route initializes AppInstUtil and calls `sceAppInstUtilInstallByPackage`
with zeroed metadata/output structs. Local packages are normally served over a
loopback HTTP range server; bare-path/file-URI attempts are fallback paths.
Some firmware paths temporarily use ShellCore credentials. Its metadata tail
has firmware-sensitive semantics: the source reports that a zero tail can
uninstall an existing copy before installing a base package. Its job `accepted`
means the API accepted the request. A serving job's `done` means its bytes were
covered and the server aged out; neither establishes an installed, launchable
same-title update. The source explicitly avoids a different API that previously
reported success with missing content.

A SlopFin PKG updater therefore still needs a verified same-title replacement
contract, installed-version/content checks, app-close/restart ownership and
recovery behavior. The folder installer has not been relabeled as package
support. No new installer payload was sent or executed.

The >70% launch-confidence gate remains unmet. Storage improvements happen
after application entry and cannot fix the earlier PPR mount hang. No console
writes, installations, launches, jailbreak changes or account changes were made
in this follow-up.
