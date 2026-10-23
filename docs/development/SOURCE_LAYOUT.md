# Source layout

SlopFin shares its application and drawing code between the native PS5 build
and a Linux preview. Platform-specific display, input and playback integration
sit behind the interfaces in `src/`.

| Path | Purpose |
| --- | --- |
| `src/` | Application screens, Jellyfin/Seerr clients, playback, subtitles, graphics and updates |
| `shaders/` | GPU menu compositor |
| `host/` | Linux platform adapters and preview entry point |
| `tests/` | Host regression tests and rendering checks |
| `assets/` | Fonts, icons, public trust anchors and dependency license texts |
| `sce_sys/` | PS5 app identity, version and presentation metadata |
| `payloads/` | Console installer, storage and supporting helper payloads |
| `runtime/` | Runtime build inputs |
| `tooling/` | Native build, link and package-format tooling |
| `tools/` | Setup, build, validation, deployment and diagnostic scripts |
| `vendor/` | Third-party headers and public platform declarations |
| `docs/` | User help and development references |
| `.github/` | CI, issue templates, contribution guidance and release notes |

`Makefile` provides the build and verification entry points. The root formatter,
editor and Git policy files keep a fresh checkout consistent across development
machines and CI. Generated binaries, dependency downloads, credentials and local
captures are excluded from version control.

Start with [architecture](ARCHITECTURE.md), [build setup](BUILD.md) or
[testing](TESTING.md). See [CONTRIBUTING](../../.github/CONTRIBUTING.md) before
submitting a change.
