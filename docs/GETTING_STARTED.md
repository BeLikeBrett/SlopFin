# Build and run SlopFin

## Use a release download

Download the [public preview](https://github.com/BeLikeBrett/SlopFin/releases/tag/01.000.002).
The folder ZIP is the recommended option: extract it and transfer the entire
PPSA99001 folder to your loader's homebrew directory (commonly
`/data/homebrew/`). Let the loader register it, then launch SlopFin.
The native package is an optional experimental installer format; read
[native FPKG](NATIVE_FPKG.md) before using it. Do not register the folder and
native package with the same title ID simultaneously.

After the initial folder installation, **Settings → Updates** can download and
install future GitHub releases on the console. Keep elfldr running and leave
the console on during installation. See [updates and recovery](UPDATES.md).

You do not need to compile SlopFin to try a release. The following sections
are for contributors and users who want to build from source.

## Requirements

- A jailbroken PS5 with FTP (normally port 2121) and elfldr (normally port 9021).
  Firmware 8.20 is the main tested environment; verify your loader's app support.
- A reachable Jellyfin server and an account with access to movie or TV libraries.
- A Linux/WSL build host with Make, Python 3, Clang/lld, LLVM utilities, curl,
  tar and unzip. `make doctor` checks the required tools. Clang 18 or the
  equivalent installed toolchain can be selected with `PS5_CLANG`.

Run from the cloned repository root containing the Makefile. The app already
has a title ID and presentation assets; `make init` is unnecessary.

```sh
cp .env.example .env                 # set PS5_HOST to your console's address
make doctor
make test
make PS5_CLANG=/usr/bin/clang
make deploy PS5_CLANG=/usr/bin/clang
bash tools/launch.sh
```

The build downloads pinned public SDK dependencies into ignored `.deps/` and
creates the runtime shim. No proprietary SDK installation is required.
Deployment stages the complete `dist/PPSA99001/` folder; `eboot.bin` alone lacks
its runtime, metadata and assets. Console helpers read `PS5_HOST` from the
environment or the local `.env`; no developer's console address is built in.
Capture helpers additionally need Pillow, and HDR previews need NumPy.

For optional CPU E-AC-3/DTS-core/TrueHD decoding, first prepare the dependency:

```sh
make sdk-archives PS5_CLANG=/usr/bin/clang
PS5_CLANG=/usr/bin/clang bash tools/build-software-audio.sh
make PS5_CLANG=/usr/bin/clang SOFTWARE_AUDIO=1
make deploy PS5_CLANG=/usr/bin/clang SOFTWARE_AUDIO=1
```

Both builds retain server fallback. See [software audio](SOFTWARE_AUDIO.md)
for limits and [NOTICE](../NOTICE.md) for linked-library redistribution terms.
Optional image formats have additional tooling described in [FFPKG](FFPKG.md)
and [deployment](DEPLOYMENT.md); folder deployment does not need them.

Read [compatibility](COMPATIBILITY.md) and [testing](TESTING.md) before treating
build success as hardware validation. Keep `.env`, saved logins and captures
private. Account settings live at `/data/slopfin/config.json` on the console.

### Controller text entry

On PS5, search, server address and account fields open the system keyboard.
Confirm to return the text to SlopFin; cancel keeps the previous value. Search
keeps its current library scope and Triangle reopens the keyboard to edit.
Search uses only the PlayStation keyboard, requested below the search field.
Artwork is hidden while typing to leave that area clear. The system controls
its final placement. Movies, Series and Episodes have their
own horizontal rows; Up/Down switches rows, Left/Right browses a row, and Cross
opens the selected item. TV libraries show series and matching episodes; movie
libraries show movies. Each type has a separate result limit so episode matches
cannot crowd out titles. The Linux preview has no replacement keyboard.

Username sign-in shows a form with editable Username and Password fields.
The password stays hidden by default; Show password reveals it and also lets
you type visibly in the system keyboard. Choose Sign in after reviewing the
fields. An error keeps the fields available to correct and retry. Circle
returns to Quick Connect and clears the entered password.

### First-launch server setup

A fresh install opens one Server address text field and one Continue button.
Select the field to open the PS5 keyboard. Confirm to review the address, then
Continue to sign in. Cancellation and connection errors keep your entry.

Enter `media.example.com` directly; a dotted server name defaults to HTTPS on
port 443. `https://media.example.com` works too. A bare IPv4 address defaults
to HTTP on Jellyfin's port 8096. Explicit protocols and ports are respected;
HTTPS is never silently downgraded. Copied `/web/index.html` links, surrounding
whitespace and custom base paths are normalized. DNS names and HTTPS use the
PS5 HTTP/SSL stack for API calls, artwork and streamed playback, with certificate
verification left enabled. IPv6 and self-signed certificates remain unsupported.
Saved bare-IP configurations retain the existing LAN socket transport.
Changing servers clears the previous account only after the new server has
been reached successfully; equivalent addresses preserve the session.
