# SlopFin compatibility

Updated 2026-10-03. Most console evidence comes from firmware 8.20 and a limited
set of servers and displays. A successful build or decoder export does not
establish playback compatibility on another setup. Detailed measurements remain
in [validation history](../PROGRESS.md).

| Area | Current implementation | Limits |
| --- | --- | --- |
| Server connection | IPv4, DNS, verified HTTPS, explicit ports and base paths | IPv6 and self-signed certificates are unsupported. Dotted names default to HTTPS/443; bare IPv4 defaults to HTTP/8096. HTTPS is never silently downgraded. |
| Libraries | Server-provided movie/TV library IDs and names; movie, series and episode browsing | Music albums, audiobooks, photos and live TV are unsupported. Large libraries and unusual metadata need more testing. Long sidebars scroll to the selected library. |
| Search | Separate Movies, Series and Episodes rows with independent limits and per-library scope | Search quality depends on Jellyfin metadata. The system keyboard's final placement is controlled by PS5. |
| Login | Quick Connect and username/password; saved device/account settings | Quick Connect requires server support and approval. The Linux preview has no console keyboard. |
| Video | H.264, HEVC Main/Main10; MPEG-TS delivery through copy/remux or server transcoding | 4K decoding does not guarantee full-detail 4K presentation; CPU conversion and rendering can limit performance. |
| SDR / HDR10 | SDR output, PQ tone mapping, experimental ten-bit HDR10 and live switching | HDMI metadata, brightness, display/receiver combinations and HDR capability negotiation need broader validation. See [HDR evidence](HDR.md). |
| Dolby Vision / HLG / HDR10+ | Selected Dolby Vision PQ base layers can fall back to HDR10 | No native Dolby Vision HDMI output, validated HLG path or HDR10+ dynamic metadata. Other profiles require server conversion. |
| Audio | Native AAC/MP3/AC-3; PCM output; experimental AC-3/E-AC-3/DTS-core HDMI bitstream; optional CPU E-AC-3/DTS-core/TrueHD with `SOFTWARE_AUDIO=1` and the documented opt-in marker | Output paths and channel mappings need physical speaker/receiver checks. No general Atmos or DTS:X preservation claim. Unsupported formats/rates use server fallback. See [audio evidence](AUDIO.md) and [software audio](SOFTWARE_AUDIO.md). |
| Playback controls | Pause, seek, progress reporting, audio/subtitle selection, quality limits and autoplay | Restarts can take time; bitrate ceilings are not constant media rates. Unsupported picture subtitles can require video transcoding. |
| Intro skipping | Server Intro segments and validated named chapter fallback | Requires usable server metadata; coverage is not guaranteed for every episode. See [intro skipping](INTRO_SKIPPING.md). |
| Text / artwork | UTF-8 titles, ellipsis, bundled Noto Sans and artwork fallbacks | Font coverage is chiefly Latin, Greek and Cyrillic; missing scripts are not fully supported. Unusual aspect ratios and missing metadata need broader review. |
| Profile pictures | Circular crop preview with movement, zoom and JPEG export | Console/USB gallery access needs the bundled sandbox helper and elfldr. Crop selection/export is tested; new-editor production upload was not exercised. |
| Updates | GitHub folder downloads, SHA-256 verification, backup/rollback and restart | Requires elfldr on port 9021 and a matching `/data/homebrew/PPSA99001` folder install. Native packages use their installer. See [updates](UPDATES.md). |
| Diagnostics | Local crash/session reports; optional explicitly configured report receiver | No custom receiver is required. Nothing uploads automatically. See [diagnostics](DIAGNOSTICS.md). |

Before a public release, test fresh login and playback against another Jellyfin
installation, mixed movie/TV libraries, missing artwork, a long library list,
a server base path, stereo and surround outputs, and an SDR-only display.
Record actual delivery codecs and output behavior for each result.
