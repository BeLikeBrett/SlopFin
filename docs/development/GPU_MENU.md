# GPU menus (4K at 60 FPS)

With `GPU_MENU=1`, every menu screen is drawn by the PS5's GPU through Vulkan
(Mesa RADV from [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa)) at the
display's full resolution. Playback is unchanged: the moment a title starts,
the display goes back to the CPU path it has always used.

Measured on Brett's PS5 at 3840x2160, Home screen (`frame cost` trace lines):

| | CPU renderer | GPU, first version | GPU now |
| --- | --- | --- | --- |
| Frames per second | ~20 | ~20 | 59.9 (the refresh rate) |
| GPU work per frame | n/a | not measured | 1.5 ms |
| Dispatches per frame | n/a | ~490 | ~60 |
| Pixels checksummed per frame | n/a | 66-98 MB | 0 |

## How a frame is drawn

`gfx` keeps its drawing API. While the GPU menu is active, each primitive
(fill, gradient, image, rounded rectangle, shadow, arc, stroke, glyph) becomes
one compute dispatch of `shaders/menu.comp` into a 4K surface owned by
`gpu_menu`. `present()` adds one more dispatch that converts that surface into
the display's tiled buffer, which VideoOut scans out directly
(VK_EXT_external_memory_host).

- **Pictures are named, not compared.** A decoded poster or backdrop gets an
  id when it enters the image cache (`gfx::Bitmap::id`), and the cached
  background composition is named by its composition count. The GPU keeps one
  copy per id and never reads the pixels again. Unnamed pixels still work: a
  checksum finds them and a byte comparison confirms them.
- **Text and icons are batched.** Glyph and icon masks go into one 32 MiB
  atlas, uploaded once per character, size and weight (`text.cpp`, `icons.cpp`
  name them). A run of glyphs is one dispatch with one workgroup layer per
  glyph, instead of one dispatch each.
- **Barriers only where they are needed.** A dispatch waits for earlier ones
  only when its clip rectangle overlaps one written since the last barrier.
  Glyphs along a line, cards in a row and most panels never do.
- **Opaque and transparent pixels skip the blend**, exactly as the CPU
  renderer does, so most of the screen is written without being read.

## Why a flip thread

GPU timestamps showed the work itself takes 1.5 ms, but a submission finishes
about 16.7 ms after it is made: this console starts GPU work about a refresh
late (PS5_Vulkan's notes call this R68, seen on a regular PS5 even with the
suspend point that fixes it on a Pro). Waiting for that, then for the flip,
cost two refreshes per frame.

So the render thread never waits for the GPU in menus:

1. It records the frame into one of three submission slots and submits it.
2. It hands `{display buffer, slot}` to the **flip thread** and starts the
   next frame.
3. The flip thread waits for that submission, flips the buffer, and waits for
   the flip to show. Then it frees the buffer that was on screen.

There are three display buffers (`kFrameCount`): one on screen, one waiting to
flip, one being written. Frames reach the screen about two refreshes after
they are drawn. Each submission's command buffer starts with a barrier after
all earlier GPU writes and reads, because submissions are no longer waited for
and the previous frame's scanout may still be reading the surface.

## Playback and everything else stay synchronous

`wait_for_flip()` first empties the flip thread (`drain_flips()`), so every
caller that touches the display gets it back exactly as before: entering
playback (`set_menu_acceleration(false)`), HDR/SDR switches, captures,
benchmarks and shutdown. Playback then flips and waits for every frame on the
render thread, as it always has. `shutdown()` stops the flip thread before
releasing the GPU.

On entering playback the trace records the largest free direct-memory block
(`gfx: playback takes the display; largest free direct block ...`); 1846 MiB
of 2304 on the last check. All Vulkan memory types come from the same direct
memory on this console, so the memory type choice in `gpu_menu.cpp` does not
change what the video decoder can get.

## Reading the trace

Every 120 frames:

```
frame cost: 59.9 fps, present 0.0 ms, buffer wait 16.3 ms, gpu busy 1.5 ms,
gpu start-to-finish 16.6 ms, cpu wait for gpu 0.0 ms, 59 dispatches,
454 glyphs, 33 barriers, 1.0 submits, 0.00 cpu syncs, hashed 0.0 MB, ...
```

- **buffer wait**: the render thread idle until a display buffer is free; this
  is the headroom left in the frame.
- **gpu busy**: the GPU's own timestamps around each submission.
- **gpu start-to-finish**: submission to completion, as the flip thread saw it.
- **cpu syncs / read back**: CPU drawing forced a round trip; should stay 0 in
  menus. **hashed**: unnamed pixels compared; should stay near 0.

## Building

```sh
bash tools/setup-gpu-menu.sh        # once: fetches and builds RADV (pinned)
GPU_MENU=1 make PS5_CLANG=/usr/bin/clang SOFTWARE_AUDIO=1
bash tools/test-gpu-menu.sh         # GPU and CPU renderers agree, 4 sizes
```

`tools/test-gpu-menu.sh` draws the same scene with both renderers on the
host's own Vulkan device. It fails when more than one pixel in 10,000 differs
by more than 2/255 (rounding at arc edges is all it allows), when the tiled
scanout disagrees with the surface, or when new pixels at a reused address
come back stale. The last run: 44 pixels differ, by 1/255 at most.
