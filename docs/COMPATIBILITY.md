# SlopFin compatibility

Updated 2026-10-20. Console evidence comes mainly from PS5 firmware 8.20.
**Implemented**, **decoder-tested** and **verified physical output** are different
claims. A Linux test cannot prove HDMI behaviour on a PS5.

## Video

| Source | Where decoding / conversion happens | Current result | Remaining limits |
| --- | --- | --- | --- |
| H.264 | PS5 hardware decodes; PS5 CPU converts pixels for rendering | Console playback tested, including selected 4K sources | Current converted picture is usually at most 1920 pixels wide. |
| HEVC Main / Main10 | PS5 hardware decodes; PS5 CPU converts pixels | SDR and PQ Main10 console playback tested | Decode support does not imply full-detail 4K presentation. |
| HDR10 | PS5 renders a ten-bit HDR surface or tone maps to SDR | Surface registration and live switches tested on firmware 8.20 | HDMI metadata, brightness and different displays/receivers still need physical validation. |
| Dolby Vision with compatible PQ base layer | PS5 hardware decodes the HDR10-compatible HEVC base; SlopFin discards DV enhancement/metadata | Profile 7 / 8.1 played on firmware 8.20; stripping identical to FFmpeg's | Always on, with automatic fallback to Jellyfin. No native Dolby Vision output. |
| Other codecs, incompatible DV, HLG or HDR10+ | Jellyfin server converts to a supported stream | Server conversion path | No validated native HLG path or HDR10+ dynamic metadata output. |

MKV and MP4 files whose video and audio the console can take as stored play
**directly**: SlopFin reads the file itself and the server runs no FFmpeg job
([direct play](development/DIRECT_PLAY.md)). Other files are repackaged by the
server without re-encoding where possible (**remux**). Changing only the audio
format can leave the video unchanged. SlopFin's CPU pixel
conversion is rendering work, not video transcoding.
[Video evidence](development/VIDEO_DECODE.md) · [HDR evidence](development/HDR.md).

## Audio

| Source / selected path | Who decodes it? | Output | Tested scope / limits |
| --- | --- | --- | --- |
| AAC, MP3, AC-3 | PS5 platform audio decoder (`sceAudiodec`) | 48 kHz PCM | Native decoder path and channel probes. Do not call this hardware decoding merely because it uses a platform API. Unsupported rates/layouts request server conversion. |
| E-AC-3, DTS core, TrueHD with PS5 software decoding | PS5 CPU, using FFmpeg | 48 kHz S16 PCM, supported layouts up to eight channels | Synthetic PCM/reference checks and limited movie trials. Physical speaker routing needs broader checks. No Atmos objects, DTS:X or full 24-bit fidelity. |
| AC-3, E-AC-3, DTS core with HDMI passthrough | TV / AV receiver | Compressed IEC 61937 bursts | AC-3 5.1, E-AC-3 7.1 and DTS 5.1 test tones heard on the test TV (2026-09-15); DTS badge confirmed. Packing matches FFmpeg. Other equipment/lip sync need checks. DTS carries only the core. |
| Other audio, unsupported rates/layouts or an explicit compatible conversion choice | Jellyfin server converts; PS5 then decodes the result | Negotiated compatible audio | The server uses its own CPU or GPU configuration. SlopFin does not locally re-encode to another streaming codec. |

Which path a track takes is decided automatically, from the track and from the
formats the TV, receiver or headset reports to the PS5
([audio route](development/AUDIO_ROUTE.md)). **Settings → Audio/Video** has one
choice:

| Surround sound | What happens |
| --- | --- |
| **Automatic** (default) | AC-3, E-AC-3 and DTS go to the TV or receiver when it lists the format; otherwise the PS5 decodes them, or Jellyfin converts them. |
| Decode on PS5 | Everything is decoded on the PS5 and sent as PCM. For receivers that play Dolby or DTS silently or out of sync. Atmos is not kept. |

TrueHD cannot pass through; the PS5 decodes it. Dolby Vision Profiles 7 and 8.1
always play from their HDR10 picture. Choices apply to the next playback.

A format outside the advertised capabilities is negotiated with Jellyfin **before**
playback. If PS5 audio decoding or the Dolby Vision base layer fails during a
title, SlopFin restarts it where it stopped with that option off for the rest
of the title, so Jellyfin converts that part. Other decoder failures are shown;
select a compatible conversion and restart.
The recorded TrueHD passthrough attempts produced beeping or silence, including
a real TrueHD/Atmos movie sample. They did not establish working TrueHD passthrough.
[Audio evidence](development/AUDIO.md) · [Software decoder](development/SOFTWARE_AUDIO.md).

## App support

| Area | Supported | Not supported / requirements |
| --- | --- | --- |
| Media libraries | Movies, series and episodes | Music albums, audiobooks, books/comics, photos and live TV are not implemented. |
| Connection | IPv4, DNS, verified HTTPS, ports and server base paths | No IPv6 or self-signed certificates. Hostnames default to HTTPS/443; bare IPv4 to HTTP/8096. |
| Sign-in | Quick Connect and username/password; saved account | Quick Connect requires server approval. |
| Subtitles | Text styling/timing; server burn-in for picture subtitles | Burning subtitles can force server video transcoding. |
| Skip Intro | Jellyfin Intro segments or validated named chapters; **Square** skips | Needs timestamps on the server. Open track/quality panels hide the button. |
| Avatar editor | Circular crop, movement, zoom and JPEG export | USB/console gallery needs the sandbox helper and elfldr. New-editor upload was not exercised against the production server. |
| Discover / requests | Seerr/Jellyseerr search, requests and download tracking; shared UI and live API checked on Linux | Native catalog, own-request list, saved profiles and context-aware actions checked on firmware 8.20. Request creation/removal uses fixture tests; production request records were preserved. Requires a separate request server. [Setup](DISCOVER.md). |
| Folder updates | GitHub download, digest/ZIP checks, backup/rollback and restart helper | Native GitHub download, verification, fresh backup, installation and automatic restart passed with 1.08 and 1.09 on firmware 8.20. The 1.09 check also replaced stale FTP-owned staging and handoff files. Requires elfldr and a matching folder install. |

Text coverage is chiefly Latin, Greek and Cyrillic. Other firmware, unusual metadata,
large libraries and display/receiver combinations need independent testing.
[Validation record](development/VALIDATION.md).
