# Subtitles

Choose a track from the subtitle panel during playback. SlopFin displays the
language, regional variant and available Forced/SDH information supplied by
Jellyfin. Tracks with identical names have a track number so they remain
selectable. Missing language tags are shown as unknown rather than guessed.

## Text subtitles

Open **Settings → Subtitles → Text subtitles**. The preview uses the same text
renderer as playback and shows your choices on both dark and bright scenes.

| Setting | Choices |
| --- | --- |
| Font | Noto Sans, Atkinson Hyperlegible, Roboto, Lato, Source Serif, Roboto Mono |
| Weight | Normal or Bold from the selected family |
| Text colour | 16 colours, including white, cream, yellow, pink, cyan and black |
| Edge style | Outline, Soft shadow, None or Thick outline |
| Outline / shadow colour | 16 colours, independent of text and background |
| Edge thickness / opacity | Fine–Strong and Subtle–Solid |
| Background | None, See-through or Solid |
| Background colour | 16 colours, independent of the letters and edge |
| Background shape | A separate box per line or one band behind the cue |
| Layout | Text size, line spacing and screen position |

Outline controls are available when an edge style is enabled. Background
colour and shape are available when a background is enabled. Left/right
changes a choice; the d-pad and right stick move through the settings.

**Reset text appearance** restores text styling without changing picture
subtitle size, placement or track preference. Older saved font and colour
choices retain their meaning when upgrading.

These settings apply to text tracks rendered by SlopFin, including SRT and
supported plain-text conversions. Font coverage is primarily Latin, Greek and
Cyrillic; not every script is supported by every family.

## Picture subtitles: PGS and DVD/VobSub

Supported embedded MKV picture tracks are decoded and drawn as PS5 overlays,
without burning them into the video. Their lettering and colours are part of
the subtitle image, so text font, outline and colour settings do not change them.

- **Picture size:** Smaller, Original, Larger, Extra large or Huge.
- **Picture position:** Original, Bottom, Higher or Top.
- **Reset picture appearance:** restores the original size and placement,
  without changing text appearance or track preference.

Size and position are also available in the playback subtitle panel while a
native picture track is selected. External picture subtitles, unsupported
containers or decoder failures may use Jellyfin's server fallback. Burned-in
subtitles cannot be moved or restyled by the client.

## Timing

The playback subtitle panel lets you move subtitles sooner or later and reset
the timing. Timing is saved for the title, separately from appearance. These
controls are offered for client-rendered tracks.

[Controls](CONTROLS.md) · [Compatibility](COMPATIBILITY.md) · [Help](README.md)
