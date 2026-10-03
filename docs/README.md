# SlopFin documentation index

Updated 2026-10-03. Start with current app behavior, then consult implementation
or inherited build references. A historical measurement is not a guarantee for
another file, firmware, build or display.

## Current application documentation

| Document | Scope |
| --- | --- |
| [Playback defaults](PLAYBACK_DEFAULTS.md) | Portable Auto policies, buffer/HDR/audio settings and acceptance gates |
| [Intro skipping](INTRO_SKIPPING.md) | Server detection, chapter fallback, controls, coverage limits and validation |
| [Compatibility](COMPATIBILITY.md) | Current implementation, measured cases and limits |
| [Software audio experiment](SOFTWARE_AUDIO.md) | Reproducible PS5 decoder probes and production gates |
| [Development plan](../PLAN.md) | Active priorities and acceptance gates |
| [Progress](../PROGRESS.md) | Dated results; earlier entries retain historical context |
| [Architecture](../ARCHITECTURE.md) | Current pipeline and ownership |
| [Audio](AUDIO.md) | Native decoders, software research, fallback and channel/rate limits |
| [HDR](HDR.md) | HDR10 implementation, measurements and unverified HDMI behavior |
| [Profile and dashboard](PROFILE_AND_DASHBOARD.md) | Profile menu and picture picker, the dashboard, and the fuller detail pages |
| [Video decode](VIDEO_DECODE.md) | Why serial 4K streams stuttered, decoder pipeline depth, reading traces at depth 3 |
| [Dolby Vision](DOLBY_VISION.md) | Base-layer fallback versus native DV output |
| [Bitrate](BITRATE.md) | Definitions, restarts and diagnostic caveats |
| [Native FPKG](NATIVE_FPKG.md) | Optional installer package, build checks and pending console launch validation |
| [Getting started](GETTING_STARTED.md) | Requirements, build, deployment and first-launch setup |
| [Linux preview](HOST_BUILD.md) | `make host`, the capture tooling, and what a preview cannot tell you |
| [Testing](TESTING.md) | Host checks and console validation workflow |
| [Project rules](../CLAUDE.md) | Memory, threading, toolchain and investigation constraints |

## Build/platform reference manuals

These originated in the native-app boilerplate. Packaging examples describe
those tools, not SlopFin's playback capabilities or required initialization.
Do not reset the app's identity to a sample title or install Clang 18 merely
because an inherited example uses it: this host builds with `/usr/bin/clang`.

| Document | What remains useful |
| --- | --- |
| [Configuration](CONFIGURATION.md) | param.json, environment and category fields |
| [Deployment](DEPLOYMENT.md) | FTP staging, dry-run and packaging workflow |
| [FFPKG](FFPKG.md) | Optional packaged-image tooling |
| [Native tooling](NATIVE_TOOLING.md) | Toolchain/build infrastructure |
| [Runtime shim](RUNTIME_SHIM.md) | Emitter provenance and reproducibility; historical artifact hashes are not current guarantees |
| [Platform notes](PLATFORM_NOTES.md) | Loader, filesystem and launch constraints |
| [Presentation assets](PRESENTATION_ASSETS.md) | Icons, DDS and launcher audio |
| [PacBrew](PACBREW.md) | Optional ports/dependency infrastructure |
| [Recipes](RECIPES.md) | Generic native API examples; not implemented app features |
| [Troubleshooting](TROUBLESHOOTING.md) | Toolchain, runtime and deployment failures |
| [Runtime README](../runtime/README.md) | Runtime source/packaging reference |
| [Tooling README](../tooling/README.md) | Host tooling reference |

Dated investigation notes in PLAN.md, PROGRESS.md and the specialist guides
retain historical measurements. Use the current compatibility guide for the
supported feature set; older observations may describe earlier builds.
