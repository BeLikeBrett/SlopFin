# The Linux preview

`make host` builds `build/host/slopfin`: the same interface the console runs,
in a window, talking to the same Jellyfin server and drawing the same artwork.
It supports repeatable interface inspection and captures. Video and audio
decoding remain console-only; the preview simulates playback controls.

Build dependencies are SDL2 and libcurl development packages, pkg-config,
Clang and zlib. On Debian/Ubuntu, install `libsdl2-dev libcurl4-openssl-dev`
alongside the native build tools.

## What is actually shared

`gfx.cpp`, `app.cpp`, `text.cpp`, `icons.cpp`, `images.cpp`, `ime.cpp`,
`pad.cpp`, `http.cpp`, `jellyfin.cpp`, `json.cpp`, `bigalloc.cpp`,
`subtitles.cpp` and `config.cpp` are compiled **unmodified** for both targets.
The connection backend in `web_transport.cpp` uses PS5 HTTP/SSL on console
and libcurl on the host; the renderer and app screens remain shared.

What `host/` supplies is the layer underneath: the PS5 C ABI those files
already call, implemented on SDL2 and POSIX.

| Console | Host stands in with |
| --- | --- |
| `sceVideoOut*` | An SDL window, or nothing at all when headless |
| `sceKernelAllocate/MapDirectMemory` | `posix_memalign` |
| `sceKernelMapNamedFlexibleMemory` | `mmap` |
| `scePad*`, `sceUserService*` | A 120-byte sample synthesized from the keyboard |
| `scePthread*` | `pthread` |
| `sceKernelGettimeofday` | The wall clock, or a virtual one (below) |
| `sceIme*` | Refused: no substitute keyboard; scripts can submit a search term to preview results |

Implementing the ABI rather than branching inside the renderer is what keeps
one copy of the drawing code. There is no `#ifdef` in `gfx.cpp`, and a visual
change cannot be right in the preview and wrong on the console.

**The host goes through the real scan-out.** The tiled copy into video memory
and the ARGB→ABGR swizzle both run on Linux exactly as they do on the console;
the host un-tiles at the very end, from the layout the hardware documents
rather than from `gfx.cpp`'s opinion of it. So the preview exercises the pixel
format and the tile table, and a capture is what the console would send over
HDMI rather than what the app meant to draw.

`player.cpp` and `audio.cpp` are not built here. `host/host_player.cpp`
simulates a clock, pause, seek and playback controls using the item's backdrop
and server-provided tracks. Native decode and HDMI behavior need console tests.

## Running it

```sh
make host
./build/host/slopfin                 # a window at 70% of 1920x1080
./build/host/slopfin --scale 100     # full size
```

Settings live in `$XDG_CONFIG_HOME/slopfin/config.json` (or
`~/.config/slopfin/`), in the same format the console keeps at
`/data/slopfin/config.json`, so a token copied off the console works unchanged.
`SLOPFIN_DATA` overrides the directory and `SLOPFIN_ASSETS` the font directory.
Run from the repository root so `assets/` is found.
The host accepts `SSL_CERT_FILE` for an explicit trusted CA bundle; certificate
and hostname verification remain enabled. This does not change PS5 trust support.

| Key | Button |
| --- | --- |
| Arrows, WASD | d-pad |
| Return, Z | cross |
| Backspace, X | circle |
| C | square |
| V, Tab | triangle |
| Q, E | L1, R1 |
| 1, 2 | L2, R2 |
| Esc, P | options |
| T | touchpad |
| I, K, mouse wheel | right stick (scrolling) |
| F12 | write a capture into `captures/` |

## Captures

```sh
tools/capture.sh out/ "wait 300; left; down x2; cross; wait 260; shot movies.png"
```

The script language is one step per frame: `wait N`, a button name (with
`x3` to repeat), `shot NAME`, and `search TERM` (spaces allowed). The latter
is a host-only preview hook that runs the real scoped Jellyfin request; it does
not replace or simulate the PS5 keyboard. Presses are injected exactly as the console's
`tools/press.sh` injects them, so a sequence run here and one run there are the
same sequence.

**Headless runs on a virtual clock.** One sixtieth of a second passes per
presented frame, whatever the host took over that frame. Without it, writing a
PNG — tens of milliseconds — stretches the frame it is taken on, and a
filmstrip of an animation reports it running several times slower than it does:
the instrument changing the thing it measures. `--real-clock` turns it off.

`./build/host/slopfin --icon-sheet out.png` draws every icon large with its
name under it, which is the only way to know a change to one signed distance
field did not spoil a neighbour.

## Looking at what comes out

`tools/look.py` turns a capture into an answer. See its `--help`; the
subcommands are `zoom`, `scan`, `plate`, `geometry`, `sheet`, `diff`, `grid`,
`contrast` and `palette`. `tools/filmstrip.py` captures consecutive frames of
one animation, lays them out as a strip, and reports whether the movement
accelerates, overshoots and settles.

Each compares against something outside our own output — a declared palette
colour, a layout constant, a known geometry — never against a second view of
the same buffer. That rule is in [CLAUDE.md](../CLAUDE.md) because ignoring it
once cost this project a long investigation into a colour bug that every
instrument said did not exist.

## What the preview cannot tell you

- **Anything about decode, pacing or audio.** There is no decoder here.
- **Anything about HDR or HDMI signalling.** The preview is sRGB.
- **Cost.** The host has far more CPU than the console's four cores, so a
  drawing change that is comfortable here can still decide the frame rate
  there. Measure that with `tools/trace.sh` on the console.
- **Memory.** `bigalloc` is backed by `mmap` here and by flexible memory
  there, and the 2 MiB process heap does not exist on a workstation.

HTTPS/DNS preview builds also require the libcurl development package.
`make test-server-network` runs the optional live hostname/TLS integration test.
