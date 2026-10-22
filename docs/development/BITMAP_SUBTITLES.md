# Picture subtitles on the PS5

Embedded PGS (`S_HDMV/PGS`) and VobSub (`S_VOBSUB`) tracks in directly played
MKV files can be decoded by FFmpeg on the PS5 CPU and drawn with alpha over the
video. The video remains on the existing hardware decoder path. Subtitle
selection preserves the server's track choice; external tracks do not count
toward the ordinal used to select a track in the file.

The MKV reader forwards selected subtitle blocks, title timestamps and
BlockDuration independently of audio and video. VobSub CodecPrivate supplies
its palette and canvas. PGS supplies its canvas, palettes and objects in the
packets. Matroska zlib compression and stripped headers are restored with a
4 MiB packet limit before decoding. Other encodings retain server fallback.
Empty PGS compositions clear the screen; later compositions replace
earlier ones. The existing title clock and timing adjustment select the image,
so pause freezes captions too. Seeks reset the decoder and queued compositions;
a caption that started before the read range may not appear until the next
complete composition arrives.

Images live in flexible memory. The timeline is bounded to 64 MiB and 512
compositions; it retains up to 60 seconds of history for timing adjustments.
The renderer holds shared references while drawing, avoiding copies and
keeping images alive across a transport reset.

## Appearance and sharpness

The overlay surface follows the console output resolution, up to 3840x2160,
with 1080p fallback if registration is refused. Text glyphs are rasterized at
that surface resolution. Picture subtitles keep their source detail; alpha-
correct scaling avoids darkened transparent edges but cannot reconstruct
lettering missing from the source. Video decode/conversion resolution remains
unchanged. SIMD 2x video scaling and tile copies keep native 4K composition
within the frame budget on the tested film sources.

Settings → Subtitles separates text appearance from picture controls. Picture
size offers Smaller (75%), Original, Larger (125%), Extra large (150%) and Huge
(200%). Position offers Original, Bottom, Higher and Top. Original preserves
the authored canvas; resizing preserves the composition's spacing and aspect,
and adjusted layouts are kept inside the screen. Multi-rectangle signs move
as one composition. A large composition may be limited to fit on screen.

While a PS5 picture overlay is active, Options → Subtitles also offers Picture
size, Picture position and Back to original picture layout. Left/Right or
Cross adjusts without restarting the stream; changes persist globally. Timing
remains per title and applies to both text and PS5 picture overlays. The
original-font/colour artwork is preserved. Server-burned tracks cannot use
these local layout controls, and text font/colour/background choices do not
restyle picture artwork.

## Build and fallback

Rebuild the pinned FFmpeg dependency with `bash tools/build-software-audio.sh`,
then `make SOFTWARE_AUDIO=1` includes both audio and picture subtitle decoders.
`BITMAP_SUBTITLES=1 SOFTWARE_AUDIO=0` includes just the picture feature;
`BITMAP_SUBTITLES=0` disables its negotiation and decoder. FFmpeg's allocator
wrappers are shared by both features. No encoders or video decoders are added.

The device profile offers embedded PGS/DVD subtitles only for Matroska and
retains Encode fallback profiles. External picture tracks, MP4/TS, forced
server remux, audio conversion and quality changes requiring server delivery
use the burn-in fallback. Failure to configure a direct track retries server
negotiation; a packet decode failure or a full subtitle queue retries with
picture burn-in. Other unsupported picture formats retain server burn-in.

## Validation

`bash tools/test-bitmap-subtitles.sh` checks generated PGS and DVD packets,
reference colours and positions, clear events, BlockDuration bounds,
decode-ahead, switch-off, resets and negotiation fallback. `make test-playback`
checks the synthetic MKV's packet selection without requiring host FFmpeg.
Regenerate that fixture with `python3 tools/make-bitmap-fixture.py`.

Local comparison on 2026-10-22: two-minute library subtitle extracts from The
Boys S02E01 (PGS) and Invincible S01E05 (DVD) matched 4,710,092 reference ARGB
pixels across 74 captions, including 35 PGS clear events. Media extracts stay
outside the repository; the committed fixture is self-authored.

On the PS5, The Boys S02E01 (zlib-compressed PGS, cropped 1920x804 HEVC,
AAC 5.1) and Invincible S01E05 (DVD, 1080p HEVC, E-AC-3 passthrough) displayed
picture overlays with direct MKV playback. The server session reported
`DirectPlay` with no TranscodingInfo for the DVD seek trial. Diagnostics showed
about 23.98 fps for both 24 fps sources. These are functional trials, not a
long-duration performance benchmark or validation of every library track.

Native 4K text-overlay trial on TRON: Legacy (4K HDR10 HEVC, TrueHD CPU audio):
12.73 seconds, 305 pictures shown (23.96/s), zero drops, decode failures, audio
underruns or rebuffers. Median render loop 16.68 ms; 15/749 loops exceeded
20 ms. This is a short functional/performance trial, not a guarantee for every
cadence or source. A first scalar 4K experiment was rejected for frame drops;
the enabled path uses reference-identical SIMD interpolation.

A 1280x720 H.264/AAC source was also checked after replacing the scalar
non-2x resize with separable SIMD filtering: 10.49 seconds, 252 pictures shown
(24.01/s), zero drops, decode failures, audio underruns or rebuffers, and no
render loops above 20 ms. The full 720p-to-4K rendering path is checked against
the scalar reference in `test_subtitle_render.cpp`; interpolation helpers are
checked for multiple widths, weights and SDR/HDR10 packing. Every pixel of a
native 4K scanout is checked against the host's independent tile mapping.

The live DVD trial also verified Larger size, Bottom/Higher placement and
reset while paused. Picture preferences were restored to Original, and the
original TRON playback position/audio/subtitle selection was restored after
testing. Final build and changed-file format checks passed; host playback,
settings/config persistence, bitmap decoding and negotiation tests passed.
