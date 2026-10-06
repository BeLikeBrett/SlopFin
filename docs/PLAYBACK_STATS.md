# Reading playback information

Touchpad or R3 opens the playback information panel. The Details sheet's
**On this PS5** section previews the server's negotiated plan. Live playback
reports the actual audio output path; receiver audibility still needs listening.

## Conversion and decoding

| Word | Meaning | Where SlopFin does it |
| --- | --- | --- |
| Copy / remux | Keep the compressed picture/sound; change only the streaming container | Jellyfin server |
| Transcode / convert | Decode and re-encode to a different codec, resolution, rate or channel layout | Jellyfin server; its CPU/GPU depends on server settings |
| Video decode | Turn compressed video into picture frames | PS5 hardware for H.264 / HEVC |
| Platform audio decode | Turn AAC / MP3 / AC-3 into PCM sound | PS5 audio decoder API; internal hardware/software implementation is not established |
| Software audio decode | Turn compatible E-AC-3 / DTS core / TrueHD into PCM sound | PS5 CPU using FFmpeg, when enabled |
| HDMI passthrough | Send compressed audio without decoding it in SlopFin | TV / AV receiver decodes it |
| Pixel conversion / tone mapping | Prepare decoded pictures for rendering/display | PS5 CPU; this does not re-encode a video stream |

Example: a file has HEVC video and TrueHD audio. With PS5 software decoding on,
Jellyfin can copy both streams; PS5 hardware decodes HEVC and its CPU decodes
TrueHD to PCM. With that option off, Jellyfin can convert only the audio while
copying the video. No full video transcode is necessary for that audio change.

## Panel labels

| Label | What to read from it |
| --- | --- |
| Original file | The file's original formats/resolution and declared average bitrate |
| Stream received | What Jellyfin sent; copy versus server conversion |
| Video decode | Where received video is decoded |
| Audio decode | PS5 platform decoder, PS5 CPU software, TV/receiver passthrough, or failed output |
| Quality ceiling | The maximum requested streaming rate; it does not inflate a smaller file |
| Stream bitrate | Compressed selected audio/video per second of media, measured over a short window |
| Network rate | Bytes received per real second, including streaming overhead and stalls |
| Playback fps | Newly presented pictures per second; compare with the source fps |
| Decode submit / pixel conversion | Time for decoder submission and CPU picture conversion; submission is not complete decode time |
| Video ahead | Positive means the picture leads the audio clock; negative means it trails |
| Display / cadence | HDMI refresh and how many refreshes each picture is held; film at 60 Hz normally alternates two and three |

A 120 Mb/s **quality ceiling** can accompany a 2 Mb/s stream. That is normal:
the ceiling limits conversion, while bitrate describes the actual media.
A low network rate during pause or buffering is different from low playback fps.
[Developer measurements](development/BITRATE.md).
