# Direct play of MKV and MP4

Since 1.15, SlopFin reads Matroska (MKV/WebM) and MP4 files directly from the
Jellyfin library instead of asking the server to repackage them into MPEG-TS.
The server sends the file's own bytes; no FFmpeg job runs for it. Anything the
console cannot take as stored still goes through the server, as before.

## When a file plays directly

The device profile lists `mkv, matroska, webm, mp4, m4v, mov` with H.264 and
HEVC video, so Jellyfin applies its usual checks: codec profiles and levels,
video range, the quality limit and subtitle delivery. A title it can play
directly comes back with `SupportsDirectPlay` and no transcode URL.

SlopFin then checks the selected audio track itself (`src/direct_play.hpp`),
because Jellyfin's direct-play check has let unlisted codecs through. The track
must be 48 kHz and one of:

- AAC, MP3, or AC-3 up to 5.1, decoded by the console;
- E-AC-3 or DTS with HDMI passthrough on;
- E-AC-3, DTS core or TrueHD with PS5 software audio on.

A converted format chosen in the Audio panel, a channel limit, or an audio track
in a separate file sends the title to the server. Text subtitles are fetched
from the server and drawn by SlopFin. With bitmap subtitle decoding included,
embedded PGS and VobSub tracks in MKV are decoded on the PS5 and drawn over the
video, preserving their palette, alpha and authored positions. This avoids a
server video encode for those subtitle selections. See [picture subtitles](BITMAP_SUBTITLES.md)
for build switches, fallback boundaries and validation.

## Reading the file

`src/direct_source.cpp` reads the index with HTTP range requests, then opens
one range from the first byte playback needs:

| Container | Index | Start of playback |
| --- | --- | --- |
| MKV | Head (EBML, SeekHead, Info, Tracks), then Cues | The cluster of the last cue at or before the position |
| MP4 | Top-level boxes walked header by header, then `moov` | The sync sample at or before the position, and the audio sample at it |

The last title's index is kept, so seeking costs one range request. A direct-play
seek also keeps the negotiated plan and play session, as Jellyfin's own clients
do; any other change negotiates again. Indexes live in flexible memory.

`src/mkv.cpp` and `src/mp4.cpp` produce the same access units as the TS
demuxer:

- video in Annex-B, with an access unit delimiter and the parameter sets before
  each keyframe (`src/annexb.hpp`);
- AAC with ADTS headers;
- AC-3, E-AC-3, DTS, TrueHD and MP3 frames as stored.

Matroska codec delay is removed from audio timestamps and MP4 edit lists are
applied, so both tracks run on title time. Priming frames before zero are
dropped.

A file is refused, and the title goes to the server for the rest of that
session, when it cannot be read this way:

- header compression or content encoding;
- unsupported codec setup;
- fragmented MP4;
- MP4 tracks stored more than 48 MiB apart;
- no MKV Cues for a seek;
- a server that ignores ranges.

`player: direct ... refused (<reason>)` in the trace says which.

## Exact seeking

Playback decodes from the keyframe before the position, but shows nothing and
plays no audio before it. Pictures before the target that nothing references
are not decoded at all (`src/seek_skip.hpp`):

- H.264 with `nal_ref_idc` 0;
- HEVC sub-layer non-reference pictures, only when the SPS declares a single
  sub-layer.

Decoding FFmpeg's output with those pictures removed gave byte-identical frames
from the target on, for H.264 and HEVC, MKV and MP4.

## Evidence (2026-10-20)

Against FFmpeg (`h264_mp4toannexb`/`hevc_mp4toannexb`, raw ADTS/AC-3/E-AC-3,
ffprobe timestamps), on files with B-frames, 10-bit HDR10 HEVC, faststart and
end-of-file `moov`, two audio tracks and all three lacing modes:

- video NAL units were identical;
- audio bytes were identical apart from priming;
- timestamps agreed within Matroska's millisecond rounding.

`tests/test_direct_source.cpp` keeps these properties with small fixtures in
`tests/media/`.

On the PS5 (firmware 8.20), against a real library over HTTPS:

| Title | Result |
| --- | --- |
| 4K60 HEVC MKV, AAC | Decoded 59.94/s; shown 50.6/s, a CPU picture-conversion limit independent of direct play |
| 1080p HEVC MKV, E-AC-3 Atmos passthrough | 24.00 fps, 0 dropped, 0 underruns over five minutes |
| 1080p HEVC MKV, AAC, text subtitles | Subtitles drawn; end of file and next-episode autoplay worked |
| 720p H.264 MKV, AAC | Played directly |

The same title and seeks, directly and through the server's remux:

| | Direct | Remux |
| --- | --- | --- |
| Seek to picture | 0.41–1.59 s, exact | 2.03–2.21 s, nearest server keyframe |
| Playback | 24.00 fps, 0 dropped | 24.00 fps, 0 dropped |

A quality limit below the source still converts on the server. MP4 has not yet
been played from a real library on the console.

## Comparing with the old path

Create `/data/slopfin-remux` to play every title through the server's remux, as
before 1.15; delete it to return to direct play.
