# Developer guide

Start with [Contributing](../../CONTRIBUTING.md), then choose the part you need.
All commands run from the repository root.

| Task | Guide |
| --- | --- |
| Build for PS5 | [Build setup](BUILD.md) |
| Preview the interface on Linux | [Host build](HOST_BUILD.md) |
| Understand the app | [Architecture](ARCHITECTURE.md) and [platform constraints](../../CLAUDE.md) |
| Validate a change | [Testing](TESTING.md) |
| Work on hardware video | [Video decode](VIDEO_DECODE.md) and [HDR](HDR.md) |
| Work on audio | [Audio paths](AUDIO.md) and [software decoder](SOFTWARE_AUDIO.md) |
| Build the experimental native package | [Native FPKG](NATIVE_FPKG.md) |
| Investigate a crash | [Diagnostic internals](DIAGNOSTICS.md) |

## Tooling references

The following cover the inherited native-app toolchain. They are reference
manuals; their sample settings and recipes do not define SlopFin's supported
features. Keep SlopFin's PPSA99001 identity when building the app.

[Native tools](NATIVE_TOOLING.md) · [Deployment](DEPLOYMENT.md) ·
[Metadata/configuration](CONFIGURATION.md) · [Runtime shim](RUNTIME_SHIM.md) ·
[FFPKG images](FFPKG.md) · [PacBrew](PACBREW.md) ·
[Presentation assets](PRESENTATION_ASSETS.md) ·
[Platform notes](PLATFORM_NOTES.md) · [Build troubleshooting](BUILD_TROUBLESHOOTING.md) ·
[API recipes](RECIPES.md)
