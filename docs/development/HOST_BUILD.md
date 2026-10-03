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

The host backend keeps platform differences outside the renderer. It runs
the tiled copy and ARGB-to-ABGR conversion, then untile-converts for SDL.
Captures exercise the shared pixel format and tile layout; console output
still requires hardware validation.

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

Headless runs advance a virtual clock by 1/60 second per frame so image
writes do not distort animation captures. Use `--real-clock` to measure
against elapsed host time.

`./build/host/slopfin --icon-sheet out.png` draws every icon large with its
name under it for comparing glyphs and checking related signed-distance fields.

## Looking at what comes out

`tools/look.py` inspects capture geometry, contrast, palette and differences.
Its subcommands are `zoom`, `scan`, `plate`, `geometry`, `sheet`, `diff`,
`grid`, `contrast` and `palette`. Use `--help` for their arguments.
`tools/filmstrip.py` captures consecutive animation frames and reports motion
and settling behavior.

Compare results with a reference palette, layout or known geometry. Comparing
two views of the same incorrect buffer cannot establish correctness.

## What the preview cannot tell you

- **Anything about decode, pacing or audio.** There is no decoder here.
- **Anything about HDR or HDMI signalling.** The preview is sRGB.
- **Cost.** The host has far more CPU than the console's four cores, so a
  drawing change that is comfortable here can still decide the frame rate
  there. Measure that with `tools/trace.sh` on the console.
- **Memory.** `bigalloc` is backed by `mmap` here and by flexible memory
  there; the console's constrained process heap does not exist on a workstation.

HTTPS/DNS preview builds also require the libcurl development package.
`make test-server-network` runs the optional live hostname/TLS integration test.

For isolated picture-picker previews, set `SLOPFIN_PHOTOS` to a fixture folder.
Without it, the host picker uses your Pictures directory. Crop rendering and
JPEG export are shared with the PS5; `make test` exercises both with synthetic
images and does not upload them to a server. Update checking and staging also
run in the preview, but installation and relaunch use the PS5 helper.
