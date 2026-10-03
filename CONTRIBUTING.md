# Contributing

Keep changes reproducible and playback claims tied to evidence. Read the
[build guide](docs/development/BUILD.md), [architecture](docs/development/ARCHITECTURE.md), [project constraints](CLAUDE.md) and
[compatibility guide](docs/COMPATIBILITY.md) before changing platform code.

Before opening a pull request:

1. Run `make test`, `make host`, and `make lint`. Add a focused regression test
   for changed behavior rather than assertions that simply repeat the code.
2. Build with `make PS5_CLANG=/usr/bin/clang`. Also build `SOFTWARE_AUDIO=1`
   when changing audio integration; the setup guide explains that dependency.
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
Release tags must exactly match `sce_sys/param.json`'s `contentVersion`
(`NN.NNN.NNN`, without a `v` prefix). Coordinate deployments when sharing a console.

For bug reports, include the build, firmware/loader, Jellyfin version, relevant
codec/container/subtitle details and whether the server copied or transcoded
media. Redact tokens, passwords, addresses and private media names from logs.
