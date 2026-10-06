# Build SlopFin

Use Linux or WSL with Make, Python 3, Clang/lld, LLVM utilities, curl, tar and
unzip. The build fetches pinned public homebrew SDK dependencies; it does not
require a proprietary publishing SDK.

```sh
git clone https://github.com/BeLikeBrett/SlopFin.git
cd SlopFin
make doctor
make test
make host
make PS5_CLANG=/usr/bin/clang
```

`make host` needs SDL2 and libcurl development packages. It previews the UI;
it does not decode video or reproduce the PlayStation keyboard.
[Host setup and captures](HOST_BUILD.md).

## Optional software audio

The public builds include the optional FFmpeg decoder. To build it yourself:

```sh
make sdk-archives PS5_CLANG=/usr/bin/clang
PS5_CLANG=/usr/bin/clang bash tools/build-software-audio.sh
make PS5_CLANG=/usr/bin/clang SOFTWARE_AUDIO=1
```

See [software audio](SOFTWARE_AUDIO.md) for its Settings toggle and limits.

## Deploy to a development console

Copy `.env.example` to `.env` and set `PS5_HOST`. Close SlopFin before replacing
files and preserve `/data/slopfin/config.json`. With FTP and elfldr running:

```sh
make deploy PS5_CLANG=/usr/bin/clang SOFTWARE_AUDIO=1
bash tools/launch.sh
```

The deployment transfers the whole app folder, including runtime, metadata
and assets. It publishes the executable and metadata last. Console tools read
the address from `.env` or the environment; neither is bundled into the app.

[Native packaging](NATIVE_FPKG.md) is separate from folder deployment and
requires additional tooling and a compatible console runtime.
