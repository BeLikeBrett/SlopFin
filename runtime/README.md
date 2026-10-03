# Clean-room runtime shim

For current SlopFin build instructions see [getting started](../docs/GETTING_STARTED.md).
The generated runtime and its accompanying checksum, not the historical hash
below, are authoritative for the current build.

`libc.prx` is generated locally from the independently authored source in this
repository and is distributed under GPL-3.0-or-later. It contains no Sony
runtime implementation, proprietary SDK binary, or game file.

The inherited template release artifact (historical, not the current SlopFin
runtime guarantee) has SHA-256:

```text
e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036
```

Generate it from the repository root:

```sh
make libc
```

Then verify it from this directory with:

```sh
sha256sum -c libc.prx.sha256
```

The generated file is ignored by Git. Bare `make` also creates it as part of a
normal application build. Tagged GitHub Releases provide the verified binary
as a convenience asset.

The complete source, reproduction procedure, and compatibility scope are in
[`tooling/native`](../tooling/native) and
[runtime shim](../docs/development/RUNTIME_SHIM.md).
