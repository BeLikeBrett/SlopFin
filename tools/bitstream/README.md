# HDMI audio bitstream experiments (dev tooling)

*Opus 5, 2026-09-15.* Can SlopFin send Dolby Digital, Dolby Digital Plus,
TrueHD and DTS to the TV untouched, the way the console's disc player does?

## What is known

`libSceAudioOut` exports an "Ex" (AV playback) family. Disassembling the console's
own copy (downloaded decrypted over FTP from `/system/common/lib`) gives:

| Call | Shape | What it does |
| --- | --- | --- |
| `sceAudioOutExConfigureOutput` | `(0, _, mode, device, _)` | Builds an HDMI output-mode request and sends it to the AV control service (`sceAvControlInit`, `GetMonitorInfo`, `ChangeOutputMode`) |
| `sceAudioOutExOpen` | `(user, mode)` | Opens port type 6 at the mode's rate |
| `sceAudioOutExGetMonitorInfo` | `(0, _, buffer, 0x400)` | The TV's EDID-derived audio capabilities |
| `sceAudioOutExGetOutputInfo` | `(0, _, buffer, 0x18)` | Nine bytes; did not change during any test |

| Mode | Request | HDMI codec type | Port rate |
| --- | --- | --- | --- |
| 0 | 6 ch bitstream | 2, AC-3 | 48 kHz |
| 1 | 6 ch bitstream | 6, AAC | 48 kHz |
| 2 | 6 ch bitstream | 7, DTS | 48 kHz |
| 3 | 8 ch bitstream | 10, E-AC-3 | 192 kHz |
| 4 | 8 ch bitstream | 0xf0 | 192 kHz |
| 5-8 | PCM 2 / 6 / 8 / 8 ch | 1 | 48 kHz |
| 9 | 6 ch bitstream | 0x16 | 48 kHz |
| 10 | 8 ch bitstream | 0xf3 | 192 kHz |
| 255 | back to default | | |

Brett's TV ("Beyond TV") reports LPCM 2 ch, AC-3 5.1 at 640 kb/s, E-AC-3 7.1 with
the Atmos flag, MAT (TrueHD) 7.1, DTS 5.1, DTS-HD 7.1 (twice) and AAC 5.1.

**From a payload** both calls are refused (`0x809b00ff`, `0x80260011`), with
ShellCore credentials too. **From SlopFin** they succeed.

## Results at the TV (Brett watching and listening, 2026-09-15)

| Stream | How it was sent | What the TV did |
| --- | --- | --- |
| AC-3 5.1 tones | Ex mode 0, 48 kHz stereo port | **Tones.** Badge read Dolby Atmos (the TV labels Dolby input that way, with its sound mode on Atmos or Auto) |
| DTS 5.1 tones | Ex mode 2, 48 kHz stereo port | **DTS badge** and tones: a real bitstream, not PCM |
| E-AC-3 7.1 tones | Ex mode 3, 192 kHz stereo port | **Tones** |
| TrueHD (ffmpeg's experimental encoder) | Ex mode 4, stereo frames | Dolby badge, **rapid beeping**: a quarter of the data rate arrives |
| same | Ex mode 4 or 10, 8-word frames | Dolby badge, silence |
| same | OpenEx type 6 format 2, 8-word frames | Dolby badge, silence |
| same | OpenEx type 6 format 6 | the output call never returned |
| same | ExPtOpen type 7 format 2, 8-word frames | Dolby badge, silence |
| same | type 7, channel orders 01324567 and 01236745 | Dolby badge, silence |
| Avatar's real TrueHD Atmos (6 s, `-c copy -f spdif`) | Ex mode 4, 8-word frames | silence |
| same | **Sys pair**: SysConfigureOutput mode 5 (codec 0x0C, MAT) + SysOpen index 5 (768 kHz), stereo frames | ran four times too long |
| same | Sys pair, 8-word frames | Dolby badge, silence |
| same, every 16-bit word byte-swapped | Sys pair, 8-word frames | Dolby badge, silence |

So AC-3, E-AC-3 and DTS bitstream work from SlopFin. **They are now the
player's normal path** (2026-09-15, late): `src/iec61937.hpp` packs frames the
way ffmpeg's spdif muxer does (verified byte for byte) and `audio.cpp` opens Ex
mode 0, 2 or 3 at playback start, falling back to decoding if that fails. TrueHD reaches the TV as a
Dolby stream the TV recognises but does not play.

## What the disc player does (for TrueHD, still open)

`/system_ex/app/NPXS40140/UHDBdPlayerCore.elf`, pulled over FTP, imports
`sceAudioOutSysConfigureOutput`, `SysOpen`, `SysGetOutputInfo`,
`SysGetMonitorInfo`, `Output` and `Open` -- not the Ex family. It contains the
MAT start, middle and end codes and the IEC 61937 preamble stored as `f8 72 4e 1f`,
so it builds MAT frames and bursts itself.

`sceAudioOutSysConfigureOutput(bus 1 or 2, flags 0/1, mode, device, 0)` modes:
0xfd AC-3 6 ch, 0xfe DTS 6 ch, 3 E-AC-3 8 ch, **5 MAT 8 ch**, 6 codec 0xf0 8 ch,
7 codec 0xf1 8 ch, 1 AAC 6 ch, 8 codec 0x16 6 ch, 0xf7-0xfc PCM layouts, 0xff default.
`sceAudioOutSysOpen(0xff, index)` opens from tables: index 5 and 7 are type 6,
grain 1024, 768 kHz, format 2; 3 and 6 are 192 kHz stereo; 0-2 and 8 are 48 kHz;
0x0b-0x3a are type 7 PCM ports at 44.1-192 kHz in several formats.

Leads not yet followed: how the disc player sizes its `sceAudioOutOutput` calls
on the MAT port (call sites near 0x1e49dd in the text segment), whether its MAT
frames differ from ffmpeg's (padding, the 0x0016 data type, burst length), and
the second SysConfigureOutput call site (0x1a454c), whose mode comes from a
virtual call.

## Running a test

SlopFin must be open and not playing. Upload an IEC 61937 stream (`ffmpeg -i in
-c copy -f spdif out.spdif`) as `/data/slopfin-bitstream-<name>.spdif`, then write
the marker `/data/slopfin-bitstream-test` containing
`mode device user port seconds words name`, for example `0 255 255 0 20 2 ac3`.
The log is `/data/slopfin-bitstream.txt`. Test streams uploaded on 2026-09-15:
`ac3` (5.1 tones), `eac3` (7.1 tones), `dts` (5.1 tones), `truehd` (5.1 tones, 5 s).

**Turn the volume down first.** If the console forwards the data as PCM instead
of flagging it as a bitstream, the TV plays loud static.

Arguments now: `mode device user port seconds words name format map`. Port 0
ExOpen, 1 ExPtOpen(format), 2 OpenEx type 6 (format), 3 the Sys pair (mode is
the Sys mode and SysOpen index). `map` reorders the words of an 8-word frame.

`probe.c` and `play.c` are the payload versions, kept because they record the
payload refusal.
