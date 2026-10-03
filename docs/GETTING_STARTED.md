# Install and connect

You need a jailbroken PS5, a homebrew loader that can launch app folders, and
a Jellyfin account with access to movies or TV. Firmware **8.20** is the main
tested environment.

## Install the folder download

1. Open [Releases](https://github.com/BeLikeBrett/SlopFin/releases) and download
   the folder ZIP from a published preview.
2. Extract it and transfer the **complete `PPSA99001` folder** to your loader's
   homebrew directory, commonly `/data/homebrew/`.
3. Let your loader register the title, then open SlopFin from the PS5 menu.

For an upgrade, close SlopFin before replacing files. Keep
`/data/slopfin/config.json`; it contains your sign-in and settings.

Use the folder build for normal use. Native FPKG is experimental and currently
stalls before app startup on the test console.
[Native package details](development/NATIVE_FPKG.md).

## Enter your server

First launch shows a **Server address** field and **Continue**. Select the field
to open the PlayStation keyboard. Confirm your entry, then choose Continue.

| Example | Connection |
| --- | --- |
| `media.example.com` | HTTPS on port 443 |
| `https://media.example.com` | The same HTTPS connection |
| `https://media.example.com/jellyfin` | HTTPS with a server base path |
| `192.168.1.20` | HTTP on Jellyfin's default port 8096 |
| `http://192.168.1.20:8096` | Explicit local connection |

Include the protocol when you need to override the default. HTTPS verifies
certificates and never falls back to HTTP automatically. Self-signed
certificates and IPv6 addresses are unsupported.

## Sign in

Choose **Quick Connect** and approve the code from an already signed-in Jellyfin
client, or use **Username and password**. Both fields are editable; **Show
password** lets you review the entry. Select Sign in when ready.

If a connection or sign-in fails, your fields remain available to correct.
Circle returns from password sign-in to Quick Connect and clears the password.

## Start watching

Use the sidebar to choose a library. Triangle opens Search; Square opens your
profile menu. [Controls](CONTROLS.md) explains DualSense seeking and playback
shortcuts. [Compatibility](COMPATIBILITY.md) covers codecs and output limits.

The next preview adds in-app folder updates and circular avatar cropping.
The updater is still awaiting its final console test; use
[published release downloads](https://github.com/BeLikeBrett/SlopFin/releases)
until then. [Update status](UPDATES.md).

For a source build, use the [developer build guide](development/BUILD.md).
