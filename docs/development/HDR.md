# HDR output

Native ten-bit HDR10 output remains experimental. Main10 decoding, successful
buffer registration and an SDR screenshot are separate from proving correct
HDMI HDR metadata, luminance and display behavior.
[Current compatibility](../COMPATIBILITY.md).

## Output selection

The app follows the PS5 HDR setting using `sceVideoOutGetOutputStatus` byte 4.
Do not restore the obsolete `/data/slopfin-hdr-auto` or forced-format markers
as a user setting. Preserve the console's HDR configuration during testing.

PQ source metadata must be cleared when Jellyfin delivers an SDR transcode.
Failed output transitions retain SDR and its tone-map path. Dolby Vision
base-layer handling is described [separately](DOLBY_VISION.md).

## Surface and conversion

- HDR buffer format: `0x8100070422000000`; title capability bit 29 is required.
- Packed surface: A2B10G10R10, with R at bit 0, G at bit 10 and B at bit 20.
- HEVC Main10 surfaces are low-aligned ten-bit YCbCr. The HDR converter preserves
  PQ using the BT.2020 NCL matrix; SDR output uses tone mapping.
- UI colors are converted from sRGB/BT.709 into BT.2020 and composited in linear
  light, with 203-nit reference white.
- Buffer-attribute transitions are acknowledged by the render thread before
  the playback worker produces frames in the new format.

`make test` checks PQ reference values/round trips, YCbCr matrices, channel
packing, reference white and alpha blending. Firmware 8.20 accepted buffer
registration and live transitions in both directions. This does not establish
universal HDR output support or performance across sources.

`tools/shot.sh` converts HDR frame dumps into an SDR diagnostic PNG, clipping
highlights above reference white. Judge matrix/packing with controlled samples;
verify HDMI output and brightness on the physical display.
