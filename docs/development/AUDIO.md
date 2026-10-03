# Audio paths

SlopFin has three audio paths. See [compatibility](../COMPATIBILITY.md) for the
current support limits; adding a platform export alone does not enable a codec.

## Console decoding and server conversion

AAC, MP3 and AC-3 can use console decoders and feed PCM to AudioOut. Unsupported
sources can be converted by Jellyfin to a compatible stream. Negotiation can
preserve the original video while converting only the audio.

The playback Audio panel chooses a track, a compatible conversion format and
channel layout. Choosing another track resets its format/channel overrides.
The server's actual response determines the delivery path; do not infer it
from the source track's label.

## HDMI passthrough

AC-3, E-AC-3 and DTS core can be packed into IEC 61937 bursts for the compressed
AudioOut path. TV/receiver compatibility and physical lip sync remain experimental.

| Format | Carrier | Burst |
| --- | --- | --- |
| AC-3 | 48 kHz | Type 1, 6144 bytes |
| E-AC-3 | 192 kHz | Type 0x15, 24576 bytes covering six audio blocks |
| DTS core | 48 kHz | Type 11/12/13 according to core frame length |

`src/iec61937.hpp` owns packing. Its AC-3/E-AC-3/DTS output was compared byte
for byte with FFmpeg's SPDIF muxer. Never scale, mix, reorder or PCM-capture a
compressed burst ring. Codec rates/layouts outside the implemented path need
server fallback. DTS-HD lossless, DTS:X and TrueHD passthrough are unavailable.

`/data/slopfin-no-bitstream` disables passthrough for the next playback.
Remove the marker to restore ordinary negotiation.

## Optional CPU decoding

The FFmpeg path supports E-AC-3, DTS core and TrueHD at tested rates/layouts,
feeding 48 kHz S16 PCM. It does not preserve Atmos objects or full 24-bit output.
[Software audio](SOFTWARE_AUDIO.md) describes the build, opt-in and fixtures.

## Ownership and validation

The player coordinates pause, seek, buffering and shutdown with the AudioOut
queue. Buffer capacity is a reservoir, not a fixed startup delay. Large PCM
buffers use flexible memory. Keep actual underruns separate from intentional
buffering and source delivery gaps.

Run `make test` for framing and audio ownership checks. Software fixtures test
sample counts and channel identity; native sink probes exercise AudioOut.
Physical listening and receiver/display checks are still required.
[Testing workflow](TESTING.md).
