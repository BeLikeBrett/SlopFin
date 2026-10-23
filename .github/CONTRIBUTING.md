# Contributing

Keep changes reproducible and playback claims tied to evidence. Read the
[build guide](../docs/development/BUILD.md), [architecture](../docs/development/ARCHITECTURE.md), [project constraints](../docs/development/CONSTRAINTS.md) and
[compatibility guide](../docs/COMPATIBILITY.md) before changing platform code.

Before opening a pull request:

1. Run `make test`, `make host`, and `make lint`. Add a focused regression test
   for changed behavior rather than assertions that simply repeat the code.
2. Build the release configuration with
   `GPU_MENU=1 make PS5_CLANG=/usr/bin/clang SOFTWARE_AUDIO=1`. For rendering
   changes, also run `bash tools/test-gpu-menu.sh` at all supported output sizes.
   The build guide covers dependencies and alternative configurations.
3. Verify zero static FSELF errors. For console changes, include firmware,
   loader, server version, media delivery details and the hardware result.
4. Keep credentials, `.env`, generated builds, dependency archives, proprietary
   modules, console captures and local configuration out of commits.
5. Write comments for constraints and non-obvious decisions. Describe the
   current behavior; discussion, prompts and investigation history belong in
   the pull request or dated development notes.

Preserve copyright, SPDX headers and upstream attribution. Project-authored
code uses GPL-3.0-or-later; vendored libraries retain their original licenses.
JSON and binary assets are covered by `LICENSE` and `NOTICE.md`.

Changes to `tooling/native/` need a deterministic host check and a focused
format regression. Loader changes need hardware validation before release.
Release tags use the display version, for example `1.08` for the PS5
`contentVersion` `01.000.008` (without a `v` prefix). Version 2.00 uses
`02.000.000`. Coordinate deployments when sharing a console.

For bug reports, include the build, firmware/loader, Jellyfin version, relevant
codec/container/subtitle details and whether the server copied or transcoded
media. Redact tokens, passwords, addresses and private media names from logs.

## Commit and pull request style

Use your own contributor identity. Commit subjects should describe the user
behaviour or technical change in a short, direct sentence. Keep session links,
transcripts and local planning notes out of commit messages and source files.

Explain the problem, the resulting behaviour and relevant validation in the
pull request. Retain existing copyright and dependency attribution. Avoid
unrelated formatting, file moves or platform changes in a focused fix.
