# SlopFin - progress

Dated investigation history follows. Earlier limits and success reports may be
superseded; current scope is in [compatibility](docs/COMPATIBILITY.md) and
active work is in the [GPT plan](PLAN.md).

Newest first. Source of truth for what actually works.

## 2026-10-02 — Focus coverage and screenshot follow-up

- Visual audit caught remaining old focus in subtitle settings, profile popup,
  end-of-title/still-watching buttons, dashboard confirmations and seek bubble.
  All now share dark blue focus fill, cyan outline and bright text.
- Native Settings and subtitle focus verified on the final deployed PS5 build
  (`captures/focus-native-settings-final.png`, `focus-native-subtitles-final.png`).
- Reviewed screenshots of Settings, Subtitles, Playback, Controller, Server,
  Account, Dashboard and profile popup in `captures/focus-final/`. These use the
  shared renderer in the host preview. The light patch in the subtitle preview
  is a sample bright video background, not a focus highlight.
- PS5 system keyboard is functional but its overlay is black in Remote Play
  capture. Do not describe a host fallback screenshot as the native keyboard.

## 2026-10-02 — Server setup, names and real HTTPS

- Fresh installs open an empty address field and one Continue button, with no
  local-versus-address choice and no developer IP default. PS5 keyboard returns
  visible text for review; validation and connection failures retain the draft.
- A bare hostname defaults to HTTPS/443; explicit HTTP(S), ports and copied
  web links work. Numeric LAN IPs retain HTTP/8096 by default. No TLS downgrade.
- Added PS5 HTTP/SSL transport for DNS, verified HTTPS, API uploads, artwork and
  incremental media reads/cancellation. Existing raw-IP LAN sockets are retained.
  Host preview uses libcurl for the same URL transport. IPv6 is still unsupported.
- Host/native builds, address tests and native API-adapter tests pass. Live host
  tests cover the user's hostname, incremental reads, cancellation and rejecting
  an untrusted certificate. PS5 public server probe, Quick Connect screen,
  saved-token session and Home artwork verified over HTTPS to the configured private server.
  South Park playback also verified over HTTPS on PS5. An untrusted certificate
  was rejected on the actual console. Per user request, the saved endpoint remains
  an HTTPS server with the original account and preferences.
  Screenshots are in `captures/domain-console-*.png`. Native first-launch and
  keyboard-return tests are in `captures/server-console-*.png`.


## 2026-09-16 — Opus 5: nothing ends on black, and the end-of-title crash is fixed

*Opus 5's entry.*

- **The end card.** A title that runs out now ends on artwork rather than
  black: an episode on the next one's backdrop with Play next episode / Watch
  again / Leave, a film on its own with the last two. It waits there until
  something is pressed. The hint is a single Circle "Back" at the bottom right
  -- Brett: "remove the x button with choose text. It's implied."
- **"Are you still watching?" redrawn** the way the streaming services do it:
  the title's artwork dimmed almost to black, the question at 64 px, one filled
  button with the countdown as a line beneath it. It can be asked after 2-5
  episodes, or after 30 minutes to 3 hours with nothing pressed (90 minutes by
  default), or never -- Settings -> Playback.
- **The crash Brett kept seeing is fixed.** A finished stream was left open
  while the end card sat there; starting anything else then stood a second
  decoder beside the first and the console killed the process. Four end-to-end
  cycles now run clean with memory flat. Detail and evidence in
  [autoplay](docs/AUTOPLAY.md).

## 2026-09-16 — Opus 5: autoplay, the next-episode card, and "are you still watching?"

*Opus 5's entry.* Built the way the television apps do it, and proved on the
console: Barry S1:E1 run to its end brought the card up at twenty seconds
(`autoplay: card up, next yes, remaining 20`), counted down, and S1:E2 started
on its own. The question renders over a paused picture with its own minute-long
countdown, and Cross resumes. Settings -> Playback holds the two choices; the
rules and their tests are in [autoplay](docs/AUTOPLAY.md).

Three things it turned up: a reused frame hid anything drawn over playback
(the card and the question are now part of that test), the crash-report offer
could open over a film and swallow the player's input, and the tooling's
`play:` path never loaded the neighbouring episodes, so nothing depending on
them could be tested remotely.

## 2026-09-16 — Opus 5: one-way actions are held, and the repeating work got cheaper

*Opus 5's entry.* Both of Brett's asks.

- **Destructive actions** are separated at the foot of a row's menu, drawn in
  red, always confirmed with Cancel focused, and only go ahead while Cross is
  held for a second with a bar filling under the button. Verified on the
  console with a temporary harmless action flagged the same way; a press alone
  does nothing. `pad::inject_hold` and `tools/press.sh "hold:cross:90"` were
  added so the tooling can exercise a held button at all.
- **Repeating work**, measured on the console idling on Home
  (`tools/trace.sh`): draw 12.46 ms median, 13.2 ms max (was 12.54 median,
  14.6 max -- the max came from building strings on the render thread every
  frame to detect the page change, which is now a comparison instead).
  "Latest in ..." refreshes every fourth turn (two minutes) rather than every
  thirty seconds, so sitting on Home is two small requests a minute instead of
  twelve; the trace shows the mix (`2 progress rows` / `6 rows with
  libraries`). The session heartbeat wakes once a second instead of twice,
  writes only when something changed or every fifteen seconds, and is pinned
  to `images::spare_cores()` so it can never land on the render core. The
  dashboard's own polling: the overview every ten seconds instead of five, a
  library scan every five instead of three, and only while that page is up.
  Home is still 30 fps -- draw plus the tiled copy is about 33 ms -- which is
  where it was before this work and is a separate problem.

## 2026-09-16 — Opus 5: Home rows refresh live, and the dashboard grows real admin control

*Opus 5's entry.*

- **"Latest in ..." now refreshes** with Continue Watching and Next Up (Brett:
  it "doesn't update in real time like continue watching does"). The library
  list is cached from the load, so a refresh is one small request per library;
  a refresh that comes back completely empty is treated as a network blip and
  leaves the rows alone. Arriving at Home also asks immediately rather than
  waiting for the next poll. Console trace: `home: refreshed 6 rows` every 30 s.
- **Dashboard**: per-user policy switches (remote access, downloads, Live TV,
  playback), reset a password, delete a user, sign every device out; replace
  all metadata on a library; enable, disable and uninstall plugins; and send a
  server log straight to the report server -- sent one from the console and it
  arrived as `013511-server-log-log_20260916.log.txt`.

## 2026-09-15 (late night) — Opus 5: crash reports, a send button, page animations, menu fade

*Opus 5's entry.* Four things Brett asked for.

- **Diagnostics.** `src/crash.cpp` installs handlers for the fatal signals and
  writes a report with the signal, the registers, a frame-pointer backtrace,
  what the app was doing, the last notes and the tail of the trace; a session
  file catches a run that dies with no chance to report. Proved on the console
  by faulting on purpose, and the addresses resolve to `src/crash.cpp:650` and
  `src/main.cpp:65` through `tools/crash-symbols.py`. The console's signal
  context is not laid out where the SDK header says -- see
  [diagnostics](docs/DIAGNOSTICS.md).
- **Send report.** After a bad run the app offers Send report / Not now, and
  Settings -> Diagnostics can send or delete at any time. Reports go to a small
  receiver on brettserver (`slopfin-reports` container, port 8103, LAN only)
  which files them by day. Sent one from the console end to end.
- **Page animations.** Every page but playback lifts and fades into place, one
  spring in `gfx::set_origin`; a page with nothing in it yet does not animate,
  so a spinner never slides in ahead of its own content.
- **The profile menu fades out** instead of vanishing, the pills lifting back
  towards the badge.

## 2026-09-15 (late night) — Opus 5: subtitle boxes no longer stack, and more choices

*Opus 5's entry.* Brett: "the backdrop behind text layers the multiple lines so
it looks awful". Each line's box was exactly one line-pitch tall, so a
translucent box overlapped the one below it and painted that band twice. Boxes
are now laid out with a gap and drawn in one pass before any text, and there is
a "One band" shape that puts a single rectangle behind the whole cue. New
choices: Huge size, three more colours (Sky, Green, Grey), a thick outline,
line spacing (Tight/Normal/Roomy) and a fourth position. Every list grew by
appending, so the choices already saved on the console keep their meaning --
pinned by a test. Checked in the preview at both shapes.

## 2026-09-15 (late night) — Opus 5: the Audio panel holds track, format and channels

*Opus 5's entry.* Brett asked for the channel setting to move out of Quality
and into the Audio button, with a choice of formats to convert to. The panel
now lists Track (when there is more than one), Format and Channels; formats are
offered only below the track's own, since the console cannot encode and a
different format is the server converting the audio while still copying the
video. Remux track names no longer repeat the release name. Verified on the
console against the server's session record. Detail in [audio](docs/AUDIO.md).

## 2026-09-15 (late night) — Opus 5: Version bar on detail pages

*Opus 5's entry.* Titles with several files (Interstellar 4K + 1080p) show a
Version row above the buttons; the choice drives the badges, Details and Play.
Checked in the preview on Interstellar: focused, 1080p chosen (badges switch to
1080p/AAC/Stereo), Details showing the 1080p file. Details in
[profile and dashboard](docs/PROFILE_AND_DASHBOARD.md).

## 2026-09-15 (late night) — Opus 5: decoder sized from every stream's own SPS

*Opus 5's entry.* The Interstellar fix below is now general: the decoder's
size, reference-picture depth and profile come from each stream's SPS, with the
server's metadata only as a fallback (details and measurements in
[video decode](docs/VIDEO_DECODE.md)). Four stream shapes re-tested on the
console with zero decode failures. Version labels now go by width too, so scope
films read 4K/1080p rather than 1440p/720p.

## 2026-09-15 (late night) — Opus 5: 4K H.264 plays (Interstellar)

*Opus 5's entry.* Interstellar's copied H.264 High 3840x1600 video failed every
frame with `0x811d0303`: the player opened every H.264 decoder at 1920x1088,
level 5.1. It now takes the source size from the server and opens H.264 wider
or taller than 1080p at 3840x2176, level 5.2 (the research's proven 4K H.264
configuration); 1080p H.264 keeps the small decoder. On the console: first
picture 3840x1600, 156 of 156 decoded and shown in 6.5 s (23.98 fps), no
failures, DTS bitstream audio, zero underruns; the picture is clean and
letterboxed.

## 2026-09-15 (late night) — Opus 5: AC-3, E-AC-3 and DTS play as bitstream in the player

*Opus 5's entry. Earlier entries by other agents are unchanged.*

The player no longer decodes AC-3, E-AC-3 or DTS. They go to the TV untouched,
packed as IEC 61937 by `src/iec61937.hpp` and written to the port
`sceAudioOutExConfigureOutput` + `sceAudioOutExOpen` open (mode 0 AC-3 at
48 kHz, 2 DTS at 48 kHz, 3 E-AC-3 at 192 kHz). DTS-HD MA sends its DTS core.

- **Packer checked against an outside authority:** its output for AC-3, E-AC-3,
  DTS and 20 s of Interstellar's real DTS-HD MA track, fed in random-sized
  pieces, was byte-identical to `ffmpeg -c copy -f spdif`. `tests/test_iec61937.cpp`
  pins the documented burst layout, including a 7.1 E-AC-3 dependent substream
  riding in its independent frame's burst (no such file in the library to test).
- **On the console** (traces, no listener yet): E-AC-3 5.1 (Chapter One: Make
  Your Mark) 24.02 fps shown, AC-3 5.1 (The Hangover) 23.98, DTS-HD MA
  (Obsidian) 23.88, all with zero underruns and zero output errors -- so the
  port paces the clock correctly at both carrier rates. AAC played normally
  straight afterwards, so closing restores the output.
- **Negotiation:** E-AC-3 and any DTS profile at 48 kHz are asked for as copies
  (`allow_bitstream`); a user channel cap still sends the server's conversion.
- **Fallback:** if configure or open fails, the old decode path runs (native
  AC-3, CPU E-AC-3/DTS). `/data/slopfin-no-bitstream` forces decoding and stops
  the copy request.
- The overlay's delivery line says "Bitstream to TV".
- Caveat: the trace analyser labels bitstream audio seconds at 48 kHz, so its
  seconds read four times high for E-AC-3; frames and underruns are right.

## 2026-09-15 (night) — Opus 5: Dolby and DTS bitstream reach the TV from SlopFin

*Opus 5's entry. Earlier entries by other agents are unchanged.*

With Brett at the TV: IEC 61937 test streams sent through libSceAudioOut's Ex
path played as Dolby Digital and Dolby Digital Plus, and a DTS stream brought up
the TV's own DTS badge, so the console passed a real bitstream from homebrew.
TrueHD did not: every port shape tried -- the Ex modes, OpenEx and ExPtOpen with
8-channel formats, two channel reorderings, a real Avatar TrueHD Atmos clip, and
the disc player's own SysConfigureOutput mode 5 plus SysOpen index 5 at 768 kHz,
with and without byte swapping -- produced a Dolby badge and silence, or rapid
beeping when only a quarter of the data rate arrived. The disc player turns out
to build its own MAT frames. Full table and leads: `tools/bitstream/README.md`.

A side effect of testing Triangle on Next Up earlier: FROM S4:E3 now shows a
little progress in Continue Watching.

## 2026-09-15 (evening) — Opus 5: detail pages filled out, series Next Up, profile menu, dashboard

*Opus 5's entry. Earlier entries by other agents are unchanged.*

Built at Brett's request and shaped by his feedback during the session; details
in [profile and dashboard](docs/PROFILE_AND_DASHBOARD.md).

- **Detail pages** use the screen: a higher hero with runtime and end time,
  credits, studios and tagline; the season strip or the rest of the season;
  Cast & Crew; the page scrolls with the focus. More Like This and the audio
  line were tried and removed at his request. Console draw 6.0-8.9 ms at 4K.
- **Series Next Up**, as the phone app shows it: a large card in the hero.
  It takes no focus: Triangle plays it (verified on the console) and Square
  opens the episode.
- **Profile badge and menu** on Home and libraries (Square): Profile, Dashboard,
  Sign Out, as animated pills centred under the badge.
- **Profile screen**: picture picker over the console's captures and USB drives,
  reached by lifting the app sandbox through elfldr; the picture is cropped,
  shrunk and uploaded as a 512-pixel JPEG. Verified end to end on the console,
  then Brett's original picture was restored. Password change is built but was
  not run against the server.
- **Dashboard** for administrators: ten pages mirroring the web dashboard, all
  loading live data in the preview. Actions and settings saves are built and
  confirmed before running, but none was run against Brett's server.
- **Focus style**: the purple focus bar is gone everywhere; a light pill marks
  focus.

## 2026-09-15 (later) — Opus 5: the disc player's Dolby/DTS bitstream path opens from SlopFin

*Opus 5's entry. Earlier entries by other agents are unchanged.*

Brett asked whether homebrew access can get Dolby out of the console, which
normally only bitstreams from the disc player. The console's `libSceAudioOut`
(pulled decrypted over FTP) was disassembled: its "Ex" calls configure the HDMI
audio mode through the AV control service and open a type-6 port at 48 or 192 kHz.
From a payload they are refused; from inside SlopFin, triggered by
`/data/slopfin-bitstream-test`, all of these were accepted and consumed five
seconds of IEC 61937 stream in 5.00 s with zero errors: Dolby Digital 5.1 (mode
0), Dolby Digital Plus 7.1 (mode 3), DTS 5.1 (mode 2), and TrueHD as a
high-bit-rate stream (modes 4 and 10). The TV's reported capabilities include
AC-3, E-AC-3 with Atmos, MAT (TrueHD), DTS and DTS-HD.

**Not established: that the TV receives a bitstream.** Nothing on the console
reports the HDMI audio format (the output-info block did not change), and Brett
was away. The test is ready for him to watch the TV's format indicator.

## 2026-09-15 — Opus 5: HDR follows the PS5's own setting; the console can be driven remotely

*Opus 5's entry. Earlier entries by other agents are unchanged.*

**HDR now follows Settings > Screen and Video > HDR.** With the setting "On When
Supported", an HDR title plays into HDR10 buffers; with "Off", it is tone mapped
to SDR by SlopFin. The setting is read at every title, so a change applies to the
next thing played without restarting the app. The `/data/slopfin-hdr-auto`
marker is retired (deleted from the console); `/data/slopfin-sdr-only` forces SDR.

How it was found, without anyone touching the menu: a payload read the console's
registry (`VIDEOOUT_hdr` = 0, its factory value, so On When Supported), then
wrote 1 and 0 back while SlopFin took display reports. `sceVideoOutGetOutputStatus`
byte 4 went 2 -> 1 -> 2 and nothing else changed. The setting was left at 0.

Measured on the console with Backrooms (4K HEVC, HDR10 base of Dolby Vision):

- Setting On: `change to HDR10 result=0`; 23.96 decoded/s, 24.01 shown/s, 0
  dropped, 3 of 1,199 loops over 20 ms, an even 2:3 cadence, 0 audio underruns.
- Setting Off, same session, no restart: SDR; 24.04 decoded/s, 23.98 shown/s, 0
  dropped.
- Overlay, photographed from the app's own capture: "HDR source -> HDR10 output |
  PS5 HDR: On When Supported" and "HDR source -> SDR output | tone mapped by
  SlopFin | PS5 HDR setting is Off".

**Not established: that the TV enters HDR.** No call readable on this firmware
reports the HDMI link, and a Remote Play HDR stream is tagged PQ/BT.2020 on SDR
screens too. The TV's own indicator is the check; the plan lists what to look at
next if it stays SDR.

**Remote Play tooling.** `tools/remote-play`: a headless pairing payload (after
linkdev) and a small session server built on pyremoteplay, patched for firmware
8.20, that takes screenshots and presses buttons in the system UI. It reached
the home screen and Settings. Limits found: SlopFin streams black, and Screen and
Video refuses to open during Remote Play, which is why the HDR experiment went
through the registry.

## 2026-09-14 (late) — Opus 5: category transitions, "On this PS5", Settings screens, subtitle appearance

*Opus 5's entry. Earlier entries by other agents are unchanged.*

**Category transitions.** Brett picked variant B from a recorded comparison: each
card's entrance is 0.65 s, one diagonal sweep across the cards on screen.
Filmstrips found two existing flashes -- new cards drawn fully for one frame
before their reveal began, and "Loading" on every switch -- both fixed. Switching
category to category now fades and sinks the old cards for a fifth of a second
instead of clearing them.

Console measurement, honestly reported: long after launch, seven switches in 20 s
cost 13 long frames of 1,199, draw median 6.6 ms, and one 22 ms draw per switch.
Temporary timing showed the grid and fade-out draw is not where that frame's
time goes (it exceeded 12 ms once, 5 ms of it the fade-out). Measured 24 s after
launch, both the build before this work and this one drop many frames (238 and
432 long frames, draw medians 10.2 and 12.0 ms), so something that loads the
console soon after launch predates it. Both are open items in the plan.

**Details: "On this PS5".** Opening Details asks the server what Play would ask,
through the player's own console options, and describes the server's own
TranscodeReasons as Jellyfin groups them. On the console: Avatar -- nothing
re-encoded, Dolby Vision played as its HDR10 base, TrueHD copied and decoded on
the PS5; The Amazing Spider-Man -- E-AC-3 copied and decoded on the PS5;
Backrooms -- video copied, DTS-HD MA converted to AAC, which is what the server's
live session reported when it was played today. Screenshots were sent to Brett.

**Settings is a menu of screens** -- Subtitles, Controller, Server, Account --
as Brett asked. **Subtitles** offers size, font (Standard, or Easy to read:
Atkinson Hyperlegible), weight, colour, edge, background box and position for
text subtitles, beside a preview drawn by playback's own code; the first choice
of each is the old look. For picture subtitles it offers "Use a text track when
there is one", which starts playback on a same-language text track and avoids a
re-encode. Checked in the preview; not yet on the TV. NOTICE.md now credits both
fonts; Noto Sans had shipped without a notice.

## 2026-09-14 (evening) — Opus 5: subtitles were 1.4 s early everywhere; per-title timing; triangle; Details

*Opus 5's entry. Earlier entries by other agents are unchanged.*

**Every subtitle showed 1.400 s early, on every title.** Brett saw it only in
SlopFin, on all media and tracks. Measured link by link against the source files
on brettserver: Jellyfin's SubRip cues match the source exactly (0.000 s on
X-Men '97 over 8 cues), but the picture timestamps in Jellyfin's stream run
+1.400 s ahead (FROM S04E03, X-Men '97, Silo, and a re-encode). The console's
own log showed FROM's first picture at 168.233 s, the value predicted for a
source keyframe at 166.833 s. The cause is ffmpeg's MPEG-TS muxer, which shifts
timestamps by twice its 0.7 s default mux delay unless told not to; Jellyfin
does not tell it. SlopFin took stream time as film time, so subtitles, the
timeline and reported resume points were all 1.4 s ahead. Corrected for every
server-muxed stream in `src/stream_clock.hpp`; direct play is untouched.
`tests/test_stream_clock.cpp` uses the measured source/stream pairs.

**Subtitle timing per title, in words.** For a text subtitle, the subtitle panel
ends with *Timing -- Showing 0.4 s sooner*, *Subtitles late? Show them sooner*,
*Subtitles early? Show them later* and *Back to original timing*. Cross moves
0.1 s (hold to repeat) and the panel stays open; the offset is saved per title
in `config.json` when the panel closes. Verified in the Linux preview, including
the saved file.

**Triangle from a bare picture opens the controls on the settings row**, as it
used to; once they are up it still switches between that row and the timeline.

**A Details button on the title screen.** The rightmost button opens a
properties sheet for the selected version -- file name, folder, format, size in
GB and exact bytes, length, bitrate and date added; video codec, profile and
level, resolution, frame rate, bit depth and HDR/Dolby Vision type; every audio
and subtitle track with bitrates. It uses data the detail request already
fetched. Formatting is tested against Backrooms' real values; checked in the
preview on Avatar (83,012,422,283 bytes, matching the file on disk). The planned
"On this PS5" section is not built yet (see the plan).

All four are deployed with `SOFTWARE_AUDIO=1`; none has been confirmed on the TV
yet.

## 2026-09-14 — Opus 5: 4K stutter fixed (decoder depth 3), Continue Watching refreshes live

*Opus 5's entry. Earlier entries by other agents are unchanged.*

**Serial 4K streams now play at full speed.** Backrooms (2026) played at
12.6-14.8 fps with 8.8-9.5 s of audio underrun per 20 s. The PS5 decoder was
opened at pipeline depth 1, which decodes one picture per call; a stream with no
slices, tiles or WPP then decodes on one thread. Backrooms is such a stream at
94 Mbps, and so is every `hevc_nvenc` transcode, which is why capping the bitrate
only reached 17 fps. Depth 3 on the same console and files:

| Stream | Before | After |
| --- | --- | --- |
| Backrooms, original 94 Mbps | 14.10-14.79 fps | **23.95 fps**, 0 dropped, 0 underrun |
| Backrooms, server re-encode 40 Mbps | 16.89 fps | **23.84 fps** |
| Avatar (was already fine) | 23.96 fps | **24.02 fps** |

Ruled out along the way, each by measurement: the server, the network (and
Cloudflare routing), Jellyfin's throttle, the AAC audio path, HDR/tone mapping,
bitrate, and leaked app state. A captured frame was checked for decoder
corruption and was clean. Pause freezes video, audio and the server's pause
state (12.5 s window). `/data/slopfin-depth1` restores the old mode for A/B.
Full record, the library survey and open items (end-of-stream drain, a socket
left open after seek): [video decode](docs/VIDEO_DECODE.md).

**Continue Watching and Next Up update while the app is open.** Previously the
rows were built once at sign-in, so something just watched only appeared after a
restart; progress was always reported to the server, the app just never asked
again. Now, leaving playback re-fetches those two rows once Home is showing
(after 0.75 s, so the stop report lands first), and Home re-fetches every 30 s
while it stays up, so progress from other devices shows too. Marking an item
played uses the same refresh instead of rebuilding all of Home. The card reveal
is keyed on a full Home load (`home_epoch`), so a refresh does not replay it.
Checked on the console: Oppenheimer, absent from the row, was first in Continue
Watching with its progress bar after 25 s of watching and backing out, with no
restart. `tests/test_home_rows.cpp` covers the splice (rows vanish when emptied,
library rows are untouched, an empty answer never wipes Home) and fails six ways
when the splice is deliberately broken.

**TrueHD was briefly lost by a deploy, and is back.** Deploying without
`SOFTWARE_AUDIO=1` removed the CPU decoder, so Avatar's TrueHD went back to a
server AC-3 conversion. Redeployed with the flag: Avatar copies TrueHD, decodes
it on the console, and plays at 23.99 fps with no underruns at decoder depth 3.
CLAUDE.md now makes the flag a deploy rule.

**The debug overlay's Source line follows the selected audio track.** It read
the file's summary, which describes only the default track, so switching
Avatar from TrueHD to DTS-HD still said TRUEHD. It now names the track actually
selected, using the server's profile (`DTS-HD MA` rather than `DTS`, `ATMOS`
when present). Confirmed on the TV by Brett: the line follows track changes.

**Server (brettserver), for context.** Jellyfin transcode throttling is on with
`ThrottleDelaySeconds=900` rather than the default 180. For progressive streams
like SlopFin's, Jellyfin 10.11.10 compares bytes written since the resume point
with a position counted from the start of the film, so the lead it keeps shrinks
the deeper into a film playback resumes; at 180 a resume at two hours kept about
0.1 s in hand. A larger value only ever throttles later, so it cannot stall any
client that 180 would not.

## 2026-09-13 — GPT local codec streaming integration

Shared TrueHD/E-AC-3/DTS CPU decoding now supports fragmented streams, explicit
channel maps, 48 kHz conversion, owned lifetime, cancellation and EOF drain.
Production AudioOut tone fixtures pass for all three; TrueHD's partial final
grain now drains. Optional host codec tests match independently decoded fixture
PCM and exercise fragmentation/timestamps/cancel/reopen. Standard playback host
tests and host preview build pass.

Avatar's opt-in movie trial copies HEVC and TrueHD from the server, decodes
TrueHD locally to PCM, and retains HDR10 base-layer output. This is a functional
integration milestone, not smooth-playback acceptance: two clean traces show
17.44/18.54 shown FPS and audio starvation during input waits. Brett confirms
TrueHD sounded choppy but worked. In contrast, the E-AC-3/HDR10 episode trial
sustains 23.98 shown FPS with zero underruns in 19.02 seconds; Brett confirms
it sounds and looks great on the TV speakers. The Bourne Supremacy DTS-core
copy trial also passes: 23.97 shown FPS, zero underruns/errors/drops, nearly
perfect 3:2 cadence with debug visible; Brett confirms clean synchronized sound.
Brett also reports smooth playback after choosing stereo audio transcode.
Added software audio work/queue/decoded-PCM/input-byte telemetry to isolate the
remaining TrueHD path; no claim yet about its root cause or a completed fix.
The latest diagnostics build passes the host suite, real-codec/TS fixture tests
and PS5 build. Runtime codec results above are from deployed `08:53:58` or the
specified earlier builds; the new work/queue timing counters have not yet been
validated in a live TrueHD recording. The user's current episode was left running.
See [software audio](docs/SOFTWARE_AUDIO.md) for exact scope and ongoing checks.

## 2026-09-13 — GPT portable defaults audit

- Recorded the user's requirement to support other PS5s and changing TVs/audio
  devices in [Playback defaults](docs/PLAYBACK_DEFAULTS.md), with buffer presets,
  HDR Auto/SDR/experimental override, audio routing and acceptance gates.
- Confirmed the existing HDR auto marker follows the source; it does not check
  sink capability. Capability API output is diagnostic opaque words only.
  The buffer capacities likewise do not implement a startup/resume target.
- Preserved the working HDR/buffer trial. New settings and capability parsing
  are planned, not represented as implemented or universally validated.
- Current checkout passes `make test-playback` and the PS5 build with
  `PS5_CLANG=/usr/bin/clang SOFTWARE_AUDIO=1`; no console restart in this pass.
- Artwork recovery host tests and host build passed; original blank-home cause
  remains unproven. Newer UI/controller/cache changes are present, so an older
  staged binary must not be launched as though it represented this checkout.

## 2026-09-13 — adaptive triggers, and the pad that was never asleep

### The controller was refused, not missing

`scePadOpen` failed every time with `0x809B0081`, which read as "no controller
attached" and sent the investigation after a retry loop, a wake-up path and a
reopen countdown. None of them could have worked. The argument was wrong:
`sceUserServiceGetInitialUser` succeeds on this console and answers **0**,
which is not a person — zero is the system pseudo-user, and the pad cannot be
opened for it.

`ime.cpp` had been asking the right question all along, which is why the
on-screen keyboard worked while the pad did not. `pad.cpp` now asks the same
way, nearest-first: foreground user, then initial user, then the login list.

    pad: user 515310724 (foreground), handle 63833600

The lesson is the one already in CLAUDE.md, in a new costume: a failure that
looks like absent hardware is worth one check against a call that *is* working
before it becomes a retry loop.

### Adaptive triggers: present, and only half implemented

Three attempts to find `scePadSetTriggerEffect`, two of them worthless:

- **A weak import** reported "absent". It says nothing — the linker binds an
  unsatisfied weak reference to zero and emits no dynamic import, so the answer
  described this build, not the console.
- **`sceKernelLoadStartModule` + `sceKernelDlsym`** answered `ENOENT`, by plain
  name and by NID alike. It also answered `ENOENT` for `scePadRead`, a symbol
  this app calls sixty times a second. A lookup that cannot find a function
  already being called is a broken lookup, and the control is the only reason
  that was visible rather than being written down as a firmware limit.
- **The SDK stub in the tree**, which is the authority and was there the whole
  time. A stub library is a list of exactly what the real module exports:

        llvm-nm --dynamic --defined-only .deps/native/ps5-payload-sdk/target/lib/libScePad.so

  lists `scePadSetTriggerEffect` among its 130 entries. It links like every
  other `scePad` call, with no lookup and nothing to fail at runtime.

The call then returned `0x80920001`, `INVALID_ARG`. The struct was not at
fault — duaLib's header carries the SDK's own `static_assert` that
`ScePadTriggerEffectParam` is 120 bytes, and ours is, with the same `padding[7]`
and 48-byte command union. Asking for every mode once, on a console with a
controller awake, located it in a single deploy instead of one guess per round
trip:

| mode | result |
| --- | --- |
| 0 off, 1 feedback, 2 weapon, 3 vibration | accepted |
| 4 multiple-position feedback | `0x80920001` |
| 5 **slope feedback** | `0x80920001` |
| 6 multiple-position vibration | `0x80920001` |

Slope feedback — the obvious fit, and what the first implementation used — is
simply not on this firmware. Every value combination inside the documented
ranges was refused, including the most conservative, so the rejection is the
mode and not the numbers.

**Weapon mode is used instead, and suits the job better.** It resists between
two positions and then breaks through. The seek rate already creeps under light
pressure and opens up as the trigger goes down, so there were two regimes
whether or not the hardware said so; the wall now sits on the boundary where a
hand can feel it. Rest against it to nudge a few seconds, push through to
commit to a fast scrub. Settings still toggles it, and the seek behaviour is
identical either way.

    pad: trigger effect -> 0 (pad present)

A trigger result is only recorded as final when byte 76 of the pad sample says
a controller is actually attached; an open handle with nothing on the end of it
refuses the call, and latching that would have turned "the pad was asleep when
playback started" into "this console has no adaptive triggers" for the rest of
the session. The effect is re-applied when a pad wakes up.

### AAC decode died of an alignment shift

Every AAC title played silently: `audio: decode failed result=0x807F0000`,
while AC-3 was fine. `tools/audio-fixture.py aac_20` and `aac_51` both failed
against the production decoder; `ac3_20` and `ac3_51` both passed, so the
harness was sound and the fault was AAC's alone.

Nothing in the AAC path had changed. `native_codec`, the `AudiodecParamAac`
values, the decoder creation and the `sceAudiodecDecode` call are textually
identical to HEAD, and the frames handed over were valid ADTS -- `ff f1 4c 80`,
AAC-LC, 48 kHz, the right channel count, a valid handle.

Built HEAD in a throwaway worktree and the same fixture passed; copied only the
audio files onto it and it failed again. Same source, different binary,
different behaviour -- which leaves where things sit in memory.

`g_pcm` is `std::array<std::uint8_t, 64 * 1024>`, alignment 1. `g_au`,
`g_pcm_item`, `g_control` and `g_param` are plain structs. sceAudiodec wants
its access-unit and PCM buffers aligned; these had been landing on usable
addresses by luck, and adding unrelated globals to audio.cpp moved them off.
They now say `alignas` out loud.

Verified after: all four fixtures pass, and the title that was silent plays
with **audio_underrun 0.000 s** over a 15 s window (15.52 s played, 5.1 AAC at
`result=0 channels=6`), video untouched at 24 fps, nothing dropped.

The regression arrived with the in-flight software-audio work and was not
caused by it in any way its author could have seen: nothing they wrote touches
AAC. It is a landmine the file has been carrying all along, and the next
global added to audio.cpp would have set it off again.

### The preview crashed at exit, and the console told us why it could not

`make host` + `--exit-after-script` segfaulted every time, after every capture
had already been written, with glibc reporting heap corruption. AddressSanitizer
named it exactly: the image cache's hash table is freed by the static
destructors that run when main returns, and a worker thread then inserts into
it (`complete_job`, images.cpp).

Nothing asks the workers to stop -- the artwork threads, the warming thread and
the data worker are all sitting on a condition variable or in a socket read
when main returns.

The console never had this, and the reason is in CLAUDE.md's hard rules:
returning from main **is** a crash there, so the app parks forever and the
process is killed outright with nothing unwound. The preview now leaves the
same way, `_Exit` after a flush. Everything it produces is written
synchronously before that point.

Three consecutive runs exit 0, `tools/capture.sh` works again, and a
navigating session under AddressSanitizer reports zero errors.

(The "hang" seen earlier the same day was not this: it was running the preview
by hand without `--exit-after-script`, which is asked to run forever.)

### The visual overhaul is folded in

`VISUAL-OVERHAUL.md` is retired. It was the source of truth for this
mini-project the way `PLAN.md` is for the app, and every box in it is ticked:
the Linux preview and its inspection tools, the grey plate under the artwork,
the shadows, the search that lives in every category, the keyboard, the
scrolling and its indicator, the playback controls and their seek, the sidebar,
the card shapes, the artwork that is warm before it is looked at, and the
arrival animation. What it recorded lives in the entries above, which carry the
measurements as well as the intent.

Its two standing rules outlived it and moved to CLAUDE.md, where they apply to
everything rather than to one effort: look at a change before claiming it, and
compare against an outside authority rather than another copy of your own
output.

The captures formerly under `docs/captures/` are archived locally; they were the
evidence for claims in this file now.

### Categories deal their cards in

A grid that appears complete in one frame reads as a screenshot rather than as
something that arrived -- and now that the posters are warm in memory before
anyone opens a category, there was no moment at all that said "this is the
list". The cards are dealt in instead: each rises 22 px and fades up over a
quarter of a second, staggered a couple of frames along the row and a few down
the rows, so the sweep follows the way the eye reads. Ease-out cubic, over in
about half a second.

It never delays anything. The cards are already drawn and already in place;
this only decides how they arrive, and the reveal is keyed on what is in the
grid rather than on the request that fetched it, so opening a category
animates and scrolling one already open does not.

Home does the same, and for the same reason: it is the screen every session
opens on, and coming back to it from a category should not be the one place
media appears from nowhere. One curve serves both -- `card_reveal(column, row)`
-- so a row of a Home shelf and a row of a grid arrive identically.

Recorded frame by frame from the preview (`--real-clock --shot-every 2`) rather
than described: Home out of the loading screen, then Movies and Shows, 30 fps.
Measured in that recording, the four Continue Watching cards begin 1, 2, 3 and
5 captured frames apart and all settle within nine.

### Every poster warm before it is looked at

On sign-in, every film, show and season poster in the library is fetched and
decoded into memory in the background, so a library opens and scrolls with its
art already there. Nothing is written to storage.

**Measured on the console**, same build both ways, switched with the
`/data/slopfin-nowarm` marker. The app counts, once per picture, whether a
card's artwork was already in memory the first time the card was drawn:

| | cards whose art was ready on first draw |
| --- | --- |
| warming off | 49 of 59 |
| warming on | **49 of 49** |

716 posters (342 film and show, 378 season, a few already on screen) warmed in
66 s from a cold server, 0 failed, 291 MiB, leaving 1439 MiB free. Most of
that time is Jellyfin resizing each poster for the first time; it keeps what
it resized, and a warm server is several times quicker. Decode is 1.1 ms a
poster, so the network is the whole cost.

What it is built from:

- **Two queues.** What is on screen is served newest first, as before; warming
  is served in library order, only when nothing on screen is waiting, and by at
  most two of the three workers, so a card being looked at never waits behind
  the library. A warmed key is not an entry until a worker takes it, so asking
  for it properly goes straight to the front.
- **Memory is limited by weight and by the kernel, not by count.** The old
  220-entry cap was why scrolling back up refetched posters. Now: a 640 MiB
  ceiling on the cache, and a floor of free flexible memory read live from the
  kernel -- 320 MiB idle, 768 MiB while a stream plays. Over it, artwork is
  released oldest-first, warmed-but-unseen before anything that was on screen,
  and never what was drawn since the last trim. Under it, nothing is released:
  a loose console keeps everything warm through playback.
- **Playback pauses warming** and makes room before the stream allocates, not
  at the next trim. The frame loop also follows the screen, so every way out of
  playback lets warming resume.
- **Episode stills are not warmed library-wide** -- 5,057 of them would be most
  of flexible memory. A season's page warms its own strip ahead of the library
  instead.
- `tests/test_images_retry.cpp` covers the rules: warming never shadows a
  demand request, waits for room and for playback to end, eviction releases
  just enough counted in bytes (a poster is under a MiB; counting whole MiB
  would have released the entire cache to cover a small shortfall -- caught
  before it shipped), unseen goes first, on-screen never goes, and playback
  raises the floor.

### The artwork workers were decoding on the render core

A thread inherits its creator's affinity. `gfx::initialize` pins the main
thread to the render core, and the image workers were created after it, from
it. The kernel's own answer, logged from inside a worker:

    images: 3 workers, would have inherited 0x1, given 0x1fc0

`0x1` is core 0 -- every JPEG this app had ever decoded competed with the
frame. They now run on cores 6-12, clear of the render thread, the row workers
and the decoder's `0x3f`. The data worker, which parses every server response,
had the same inheritance and is moved too. The rule is in CLAUDE.md.

**Not fixed, and worth measuring next:** the player's stream, transport and
subtitle threads and the audio output thread are also created from the render
thread and set no affinity, so by the same inheritance they share core 0 with
rendering. Left alone here because it is playback code with its own risks and
deserves its own before-and-after.

### Scrolling while warming costs nothing measurable

Frame trace on the Movies grid while scrolling, warming running throughout,
same build, presses sent without screenshots:

| | median | p95 | over 20 ms | worst |
| --- | --- | --- | --- | --- |
| warming off | 16.683 ms | 16.709 ms | 14 / 1199 | 33 ms |
| warming on | 16.683 ms | 16.708 ms | 14 / 1199 | 33 ms |

The first run showed a 317 ms frame with warming on. It was the harness:
`press.sh` ends with a screenshot, and the capture is written from `present`.
With presses written straight to the input file it disappeared from both.

### The loading screen holds for the first screen's pictures

Home used to appear as a page of placeholders filling in. Coming in from the
loading screen, the data worker now fetches the hero backdrop and the first
cards of the top two rows -- asked for exactly as they will be drawn -- and
shows Home when they are decoded, capped at 2.5 s so a slow server can never
hold it up for long. Measured: held 252-1204 ms with 13 of 13 pictures ready;
the five launch cards that always missed before now load five of five.

### Playback with a warm cache

PLAYBACK_RESULTS

### Rows that mix card shapes

In Continue Watching and Next Up, episodes are landscape (400x225) and
everything else -- every film -- is a poster at the same 240x360 it is in
Latest in Movies. The rule is the item's type and nothing else; an earlier
version decided by which artwork each item happened to have, and it was
wrong, because a film should look like a film whether or not its library has
a backdrop for it.

A row is as tall as its tallest card and every card stands on the same floor,
so the titles underneath share one line. A row of nothing but episodes stays
as short as it always was. The row is laid out by adding widths rather than
multiplying one: `card_offset` walks the items, and `update_home_scroll` asks
it where the focused card is. A poster card asks for the poster, not a
backdrop cropped down to one.

### The sidebar rules sit between their groups

Home floated well clear of the rule under it while Settings sat tight beneath
its own. The rule was drawn eight pixels below the slot the next word would
have taken, which is most of a row's height below the word above it -- 72
pixels of air above, 26 below, on both rules.

It now sits midway between the ink of the two words it separates: 59 above and
59 below. The ink offsets are measured off a capture (a label's ink runs from
10 to 18 pixels below its anchor at the inactive size) rather than guessed, and
the inactive metrics are used whatever the focus is, so the rule does not move
when the cursor does. Nothing else moved -- only the two lines.

### Seeking: a tap that is always the same size

Two separate faults, reported together as "sometimes it skips".

**The triggers had no tap.** The directions have had one for a while -- a press
steps a flat ten seconds, and the continuous scrub only starts after twenty
frames of holding -- but the triggers went straight to the rate. What a tap
moved was therefore the rate multiplied by however many frames the finger
happened to be down, and since a quick firm tap is several frames at a rate
that has already left the creep, no two taps agreed. The triggers now wait out
the same twenty frames and a tap steps a flat **one second**: fine where the
directions are coarse, and larger than the half second that counts as not
having moved, so a tap the wrong way can be taken back by a tap the right way.

**The cancel point could not be landed on.** Letting go at exactly zero is how
a skip is called off, and at a gathered rate zero was crossed in a single
frame. The rate is now eased down to a crawl within ten seconds of where the
skip started, in both directions.

The ease is *cubed*, and that is the whole of the fix. A straight line leaves a
tenth of the zone running at a tenth of a gathered rate, which is still about a
second of film per frame:

| ease | frames inside the cancel window | three seconds of holding reaches |
| --- | --- | --- |
| linear | 4 (0.07 s) | 660 s |
| squared | 11 (0.18 s) | 620 s |
| **cubed** | **30 (0.50 s)** | **598 s** |

Half a second to release into, and the cost is two tenths of a second on a
ten-minute skip.

The curve moved to `src/seek.hpp` and is covered by `tests/test_seek.cpp`. It
had to: the trigger seek reads an analogue axis, which the scripted preview
cannot drive and a screenshot cannot measure, so there was no way to check a
claim about it short of a console and a stopwatch. The test asserts the two
things that were actually complained about -- what one tap moves, and how long
the cancel window is open for -- plus that the rate never falls as the skip
grows or the trigger goes down, and that backwards matches forwards.

### Sidebar focus on Home

Measured on the console rather than claimed: the focus wash is rows 112–133 on
Home and 175–196 on Movies, height 22 on both. The remaining brightness
difference (35.4 against 37.5) is glyph coverage — "Home" is four letters and
"Movies" is six.

## 2026-09-13 — GPT artwork recovery

- Brett confirmed Avatar looks much smoother with the larger buffers.
- Reported blank home artwork recovered before the code change; console
  captures showed the cards and hero image loading again. The original
  transient cause is not established.
- Found and corrected a separate deterministic recovery bug: failed image
  requests were cached forever. Visible failed images now retry with bounded
  exponential backoff; 404s wait five minutes to avoid repeatedly fetching
  absent art. Pending/successful entries do not create duplicate work.
- Image requests explicitly ask for JPEG posters/backdrops and PNG logos,
  matching the compiled decoder and retaining logo transparency. Failures
  remain visible in diagnostics beyond the first ten image requests.
- Production-cache tests cover failure, cooldown, retry, in-flight deduplication,
  recovery, and missing-art backoff. Request tests check supported image formats.

## 2026-09-13 — GPT HDR and audio follow-through

- Collected the completed real Avatar TrueHD 7.1 probe: 90,880 samples/channel,
  all eight S16 channels exactly match the host reference. Decoder elapsed
  0.112488 s for 1.893 s of audio. Native AC-3 5.1 tones pass at the deployed
  512-frame default. Software codecs still await normal playback integration.
- User confirmed Silo's crackling seemed gone in the HE-AAC 5.1 live retest;
  21 Jump Street looked and sounded great. Keep longer-session validation open.
- Profile 8.1 “Chapter 10: The Dark Lord” plays its HDR10 base layer with HEVC
  copy, 372 shown pictures/15.50 seconds, no drops/decode failures/underruns.
- Fresh Avatar run at default 512 output blocks still has transport starvation:
  413 shown/20.02 seconds, 2.635 seconds of audio underrun, up to 2.3-second
  socket read delay, zero decode failures. Server remux is more than six times
  real time and server throttling is disabled. Audio reaches the four-second
  reservoir ceiling. Eight seconds plus 64 MiB compressed video then gave
  480 shown/20.00 seconds, no drops/underruns, and at least 5.077 seconds of
  audio buffered despite a 2.172-second read wait. A repeat gave 444 shown
  over 18.50 seconds, also no drops/underruns; five one-refresh holds remain.
  These windows support the larger buffers, not a library-wide smoothness claim.
- Trace tooling now reports the actual captured span: the 1,200-sample ring
  retains roughly 20 seconds at 60 Hz even if a longer duration is requested.
  Debug FPS also expires to zero when no new picture arrives for a second
  or playback is paused, instead of displaying stale healthy throughput.
  A paused debug capture confirms 0.00 FPS.
- Bitrate restart retest passes: 1 Mb/s encodes H.264 to 1280x544, Automatic
  restores 1920x816 H.264 copy, and debug Limit/Media fields agree with the
  selected request and independent server delivery checks. AAC resampling
  remains required for this source. Avatar was restored afterward.
- PS5 builds, host preview and playback regressions pass. The pinned software
  decoder build script was run successfully from its recorded configuration.

## 2026-09-12 — user confirmations and test setup

- TCL 55Q51K built-in TV speakers; no receiver/soundbar or separate surrounds.
  Investigate TV downmix/levels and crackling rather than assuming physical 5.1.
- PS5 uses Wi-Fi; user reports roughly 200 Mb/s download speed and a local
  Jellyfin server. Peak/average speed does not isolate the observed stalls.
  Separate transport delivery and client scheduling before blaming Wi-Fi.
- Avatar HDR10-base trial triggers the TV's HDR indicator, with apparently
  accurate HDR lighting; user still sees frame drops. Keep HDR mode confirmed
  separately from cadence and calibrated picture accuracy.
- User confirmed a separate UI agent was finishing work and authorized this
  audio/playback continuation. Repeat any measurement interrupted by deployment.

## 2026-09-12 — GPT software audio decoder feasibility

- Built an optional minimal FFmpeg 9.0.1 decoder bundle for PS5. E-AC-3
  stereo/5.1, DTS core 5.1 and TrueHD 5.1 decode on-console with correct tones,
  sample count and host-reference PCM. TrueHD S16 output matches exactly.
  Three seconds of TrueHD decode/convert took 0.148 seconds without video.
- Corrected the probe's rejection of TrueHD's initial no-consumption sync
  transition. Corrected an aliased DTS LFE fixture; both source-tone identity
  and host-reference checks are now required.
- Native AAC stereo and AC-3 stereo tone routing also pass. This does not
  close physical speaker/crackling or stereo output-clock bugs.
- Found that native AAC partial frames were discarded between PES packets.
  Added bounded ADTS reassembly and tests at every split point, including
  maximum-size frames, CRC headers, invalid headers and reset. A fresh
  18.50-second 21 Jump Street trace shows 427 pictures, zero decode failures,
  drops or audio underruns. Output is still slow: 17.819 seconds of audio
  submitted in that window, with 23.08 shown FPS. Output-block pacing is
  being measured separately. A 1024-frame output-block trial restored
  23.98 shown FPS and real-time audio with no drops/underruns, but had eight
  one-refresh holds. The 512-frame follow-up was invalidated by an external
  restart into build `21:27:41` during capture; no default was changed and
  the temporary marker was removed. Trace tooling now rejects changed-build
  recordings explicitly.
- See [software audio](docs/SOFTWARE_AUDIO.md) for build commands, measured
  cases and remaining production gates. Normal movie playback still uses
  server fallback for these three software codecs.

## 2026-09-12 — GPT transport/audio continuation

- Fixed a second cause of silent stereo playback: the video frame pool could
  block the transport thread before it reached more audio. A bounded flexible-
  memory video queue now separates transport/demux/audio feeding from hardware
  video decode. 21 Jump Street track 2 reaches the native decoder as 48 kHz AAC
  and now produces fresh stereo PCM instead of freezing after about 15 pictures.
- Removed PCM overflow dropping. The producer waits for output space and stop
  releases it before joining/freeing decoders. Queue wraparound, ordering,
  backpressure and cancellation tests pass, as do existing shutdown tests.
- Added socket/audio stage counters. Pacific Rim showed socket waits up to
  1.895 seconds and 2.32 seconds of audio underruns in a 15.50-second trace while
  FFmpeg was remuxing faster than playback. A 2 MiB receive-buffer request was
  accepted and the next trace had zero underruns and about 24.2 shown FPS.
  The repeat trace still caught a 1.54-second socket wait and 0.704 seconds of
  underruns at its end. With the four-second PCM reservoir, an 18.50-second
  Pacific Rim trace showed 443 pictures (23.94 FPS across the whole window),
  exactly 221 two-refresh and 221 three-refresh holds, no drops or underruns,
  despite a 1.41-second socket wait. Avatar still underruns; this is not a
  library-wide smoothness claim.
- Moved audio capture to submitted output blocks. Fixed the analyzer's lost
  silence runs: a real 1.91-second gap had previously been reported as zero.
- Production AC-3 and AAC 5.1 six-tone fixtures both pass all channel identities,
  including center/LFE and silent spare sides. Physical TV/receiver routing and
  the user's crackling report remain to be validated. These are native codec
  4/3 decodes to PCM, not TrueHD/DTS passthrough claims.
- Fresh 21 Jump Street pause/resume test: no audio or picture advancement
  while paused; resume has no drops or underruns, but stereo output consumes
  about 46.5k frames/second instead of 48k with a full buffer. This slower
  output clock remains an open timing bug.
- The guarded Dolby Vision HDR10-base experiment now plays Avatar Profile 7
  as copied HEVC Main10 on an HDR surface. No hardware decode failures were
  observed; network gaps remain. TV confirmation and Profile 8.1 testing
  remain pending. This is not Dolby Vision HDMI output.
- PS5 and Linux preview builds pass. Added deterministic queue and PCM
  backpressure regression tests. Existing interface changes committed during
  the user's break remain intact.

## 2026-09-12 - a Linux preview, tools to look at it, and an interface overhaul

Branch `visual-overhaul`. Built and driven through the new Linux preview, then
deployed and measured on the console (build 20:06:31).

**Console frame cost**, which the preview could not answer: a category grid
runs at 30 fps (loop 33.4 ms, draw 15.7 ms) and the search screen at 60 fps
(loop 16.7 ms, draw 6.5 ms). The grid was 30 fps before this branch too --
draw plus the tiled copy is about 20 ms against a 16.7 ms vblank, so the flip
lands on the second one. This branch made it cheaper: the same grid with the
old card drawing measures 11.1 ms a frame on the workstation against 9.4 ms
now, and the console's own primitive benchmark puts the opaque plate that used
to sit under every card at roughly 2 ms of it. The focused card's drop shadow,
which was the thing worth worrying about, is a rounding error beside the twelve
poster blits. Leads for the remaining shortfall are in the entries above.

`make host` builds `build/host/slopfin`. `gfx.cpp`, `app.cpp`, `text.cpp`,
`icons.cpp`, `images.cpp`, `keyboard.cpp` and `pad.cpp` compile unmodified for
both targets; `host/` implements the PS5 C ABI they already call over SDL2 and
POSIX, so there is no `#ifdef` in the renderer. The tiled copy and the
ARGB-to-ABGR swizzle run on Linux too and the host un-tiles at the end from the
documented layout, so a capture is what the console would scan out.
`host/host_player.cpp` stands in for the decoder with a real clock over the
item's own backdrop. See [docs/HOST_BUILD.md](docs/HOST_BUILD.md).

`tools/look.py` answers a visual question as a number or a magnification:
`zoom`, `scan`, `plate`, `geometry`, `sheet`, `diff`, `grid`, `contrast`,
`palette`. `tools/capture.sh` drives the preview headless from a script and
`tools/filmstrip.py` measures an animation. The filmstrip's first answer was
wrong and the tooling had to be fixed before it could be trusted: writing a PNG
costs tens of milliseconds, which stretched the frame it was taken on and made
the animation look three times slower than it runs. Headless now advances the
app's clock by one sixtieth of a second per presented frame.

With that, the reported grey rectangle under every poster was measurable. A
card whose artwork had not arrived was 99.4 percent `#2c2c2c`, and an opaque
plate was drawn under every card whether or not anything covered it. The plate
is gone: artwork draws onto the page masked to its own corners, a card with
nothing yet is a tint two shades off the background, and the picture fades in
over nine frames. The same region now measures `#101018`. Shadows belong to the
focused card alone. An episode in a library or a search takes its show's poster
rather than cropping a 16:9 still into a 2:3 box; Continue Watching keeps the
still, because there every card can be the same show.

Motion moved to `src/motion.hpp`: a spring with named tunings, replacing a
linear approach that snapped inside a third of a pixel. `tests/test_motion.cpp`
checks that scrolling never overshoots, that focus does, that everything
settles exactly, and that a quarter-second frame does not turn it into an
oscillator. On a filmstrip the focus now reaches its fastest at frame 2 and
settles by frame 11; the old curve was fastest on the first frame.

The right stick scrolls a category continuously, with the cursor following the
page rather than dragging it back, and a position bar on the right whose thumb
length is the visible fraction of the list. It appears on scroll and retires
after about three quarters of a second. Rows scrolled off the top fade under
the header in the page's own colour instead of being cut.

Search left the sidebar. Triangle opens a bar in whichever section is on
screen, the app's own keyboard (`src/keyboard.{hpp,cpp}`, with every key proven
reachable in `tests/test_keyboard.cpp`) spawns directly under it, enter puts
the term in the bar and results appear beneath as their own region over the
category. Scope follows the section: "Bat" from Home returned 45 results of
every kind and from a shows library returned 2 series. The system IME dialog is
no longer used for search; it is full-screen and cannot be put under a bar.
`jellyfin::search` gained a library and type scope, and dropped the `sortBy`
that was replacing the server's match ranking with an alphabetical one.

Playback controls: "up then right" to reach the settings row is gone. The row
carries a triangle in the pad's own green, lit on a disc while it holds the
directions; triangle toggles it and triangle or circle gives the directions
back to the timeline. Previous and Next episode sit below the timeline, offered
only when there is an episode on that side, and down goes to them instead of
hiding the controls -- only circle does that now. Verified end to end, S3:E9 to
S3:E10, through a new worker request that fetches the neighbour's media sources
and hands them to the render thread to start.

## 2026-09-12 — GPT flicker correction and measured remaining shortfall

The user confirmed no other agent is editing/deploying, restoring console
ownership for this work. The earlier coordination note below is historical.

The retained-frame optimization had a concrete invalidation bug: `frame()`
painted the menu background before `draw_player()` decided to reuse the previous
video picture. Playback now skips the menu background; video rendering covers
the picture and letterbox bars, and loading clears its own frame. This fix was
deployed after the user's live flicker report (build 06:25:54). The user confirmed
that flickering has stopped on the TV.

A fresh 15.50-second clean Pacific Rim HDR10 trace measured 60 render iterations/s,
18.45 decoded and converted pictures/s, 18.39 shown/s, one presentation drop
and zero decode failures. Median conversion was 3.44 ms; median drawing 0.118 ms.
The shortfall is real, but these counters do not identify a hardware limit.
Only 11.929 seconds of picture timestamps advanced during the trace. Follow up
on producer stalls, audio clock starvation and buffering before changing cadence.

Stereo AAC testing proved the negotiated sample-rate condition alone was
insufficient: 21 Jump Street track 2 still decoded at 44.1 kHz and was rejected
by the sink. The URL now forces audio encoding at 48 kHz for incompatible or
unknown source rates. Host tests/build pass; the console retest is in progress.
The repaired audio capture tool correctly fails the silent old path instead of
reusing stale PCM. A fresh AC-3 control capture has six active channels and no
clipped samples (capture observes queued PCM, not physical HDMI output).

## 2026-09-11 - one set of subtitles, and a stream the server stops re-encoding

The subtitles drawn twice, and the ones that stayed on screen after being
turned off, were the same bug. `PlaybackInfo` said the chosen SubRip track
would be delivered as an external file, which the app fetches and draws
itself, but the stream URL was then given that same track index. The
`/Videos/.../stream.ts` endpoint defaults `SubtitleMethod` to `Encode`, so
the server burned the track into the picture as well. One copy belonged to
the app and one to the video, at different sizes because one is scaled with
the picture. Turning subtitles off cleared only the app's copy; the burned
pair kept playing, which is what "it draws subtitles even when it's off"
looked like.

The stream now carries a track index only when the plan says the server
must burn it in, which is the case the app cannot draw: PGS and the other
image formats. Everything else asks for `SubtitleStreamIndex=-1`.

Burning subtitles forces a full re-encode, so this also stopped one. Silo
went from `hevc transcode` to `hevc copy` on the same episode, with the
console's decoder doing the work it should have been doing all along.

Two smaller things found alongside it:

- Starting a different title kept the previous title's cues loaded, because
  only the playback thread cleared them and it clears on its own plan. A
  title change now clears them in `player::start`; restarting the same title
  keeps them, so a seek does not blank the line while the server extracts the
  track again.
- The debug overlay says what the subtitles are doing - off, burned in by the
  server, or a text track with the number of cues held - because "on" and
  "off" cannot tell a stale cue set from a live one.

Verified on the console in a dense dialogue window: with track 2 one line is
drawn, and with subtitles off nothing is drawn and the overlay reads off with
no cues held.

## 2026-09-11 - Native AC-3 and Experimental HDR10

- AC-3 48 kHz hardware decoding now works end to end, including LFE and the
  corrected six-channel map. Jellyfin copies supported AC-3 instead of encoding
  it to AAC. Framing, TS signaling and channel output were tested.
- HDR10 surface registration is solved: native format `0x8100070422000000`
  needs the title HDR flag. An opt-in packed-PQ path and linear-light UI
  compositor now run on the console. SDR remains the normal default.
- Host playback tests pass, including new AC-3 framing and HDR color tests.
- This is not full Dolby/DTS/HDR completion: E-AC-3, DTS and TrueHD still use
  server fallback; passthrough, HDMI HDR validation and automatic mode changes
  remain outstanding. See `docs/AUDIO.md` and `docs/HDR.md`.

Follow-up: live SDR/HDR buffer-format changes now succeed in both directions,
and per-title switching is available behind `/data/slopfin-hdr-auto`. Pacific
Rim and The Amazing Spider-Man have played copied HDR10 video. A missing
subtitle-off URL parameter was triggering unexpected video transcoding; six
HEVC picture buffers were required for Pacific Rim. Smooth pacing, full 4K
presentation detail, and HDMI validation remain unresolved. HDR overlay black
scrims now use a precomputed linear-light blend table to reduce rendering cost.

## 2026-09-11 - player controls: back, a transport button, subtitles that hold still

Five things the interface got wrong, each confirmed fixed on the console with
a screenshot rather than by reading the code back.

- **Circle never left the player.** It spent the first press hiding the
  controls. Pausing pins the controls open, so while paused circle could not
  leave at all. Circle is now always back, and `down` from the scrubber row
  retires the controls instead. A deliberate dismissal sets `osd_dismissed`,
  which survives the pause pinning until the next wake.
- **No play/pause control.** A badge sits at the left of the controls row at
  `kTransportRow`, showing the action (a triangle while paused) the way every
  other player does, with a ring that pushes out of it for forty frames on
  each toggle. The old glyph that flashed in the centre of the picture is
  gone. The title starts at `kSafeX + kTransportBadge + 26` to clear it.
- **Subtitles jumped 368 px** whenever the controls appeared, moving the line
  being read mid-word. They hold one height now and the controls are drawn
  over the top of them, which is the order asked for.
- **Wrapped text indented a line** whenever the break landed on the space
  itself: `break_at == start` kept that space as the first character of the
  next line. `skip_spaces` in `text.cpp` drops it. Seen in the Dragon Ball
  DAIMA synopsis on "Goku and the others".
- **The detail screen's Back button** is gone; circle already does it. A
  series or season, which has no buttons of its own, now opens with its
  episode strip focused.

The sidebar activation that looked broken never was. Pressing left from a
card in the middle of a home row moves between cards; it only enters the
sidebar from the first card. The test sequence had never reached the
sidebar at all. Cross on a library opens its grid, and the active-section
indicator follows, both photographed.

`tools/press.sh` takes a third argument, the settle time before the frame is
pulled. The controls retire after four seconds and the old fixed wait could
never photograph them.

## 2026-09-11 - native MP3 decode and Dolby-family fallback validation

SlopFin now has a second native `sceAudiodec` audio path: MP3 codec id 2 with
the measured 8-byte parameter block. The MPEG-TS demuxer recognises MPEG audio
stream types `0x03` and `0x04`, and the audio submitter splits exact MP3 frames
before calling the platform decoder, the same way it already walks ADTS AAC.

The Jellyfin device profile still does not claim native AC-3/E-AC-3/DTS/TrueHD
decode. Those decoder modules are present on the console, but their standalone
public-shaped control blocks are not known yet. The correct current behaviour
is therefore to direct-copy video and transcode only unsupported compressed
audio to AAC.

Verified on the console against a 48 kHz stereo MP3 item:

| Signal | Result |
| --- | --- |
| Jellyfin delivery | HEVC remux, MP3 audio direct |
| Server reasons | `ContainerNotSupported`, `VideoCodecNotSupported` |
| Frame trace | 1 of 509 render iterations over 20 ms, 23.86 fps |
| Audio sink | 4.03 s, 193536 frames, 2 channels at 48 kHz |
| Audio analysis | distinct stereo, 0 clipped samples, 0 ms longest gap |

Verified on the console against an E-AC-3 5.1 item:

| Signal | Result |
| --- | --- |
| Jellyfin delivery | HEVC direct, audio AAC transcode |
| Server reasons | `ContainerNotSupported`, `AudioCodecNotSupported` |
| Frame trace | 0 of 509 render iterations over 20 ms, 24.02 fps |
| Audio sink | 4.00 s, 192000 frames, 8 channels at 48 kHz |
| Audio analysis | distinct active channels, 0 clipped samples, 0 ms longest gap |

## 2026-09-10 - audio

AAC is decoded by the console's audio decoder and played through the audio
output. A transport-stream audio packet usually carries several ADTS frames
back to back, so each frame's own length field is walked rather than assuming
one frame per packet.

The output thread blocks in `sceAudioOutOutput`, which is what paces it; there
is no sleep in that loop by design. When the ring runs dry it emits silence
rather than stalling, so the cadence, and the clock video will follow, keep
running.

Needed a link-only facade for `libSceAudiodec`, which the public SDK omits,
alongside the existing ones for CommonDialog and Videodec2.

Not yet done: video still follows a wall clock rather than the audio clock, so
the two can drift over a long title.

## 2026-09-10 - playback is smooth

Every remaining stutter was self-inflicted, and none of it was visible in a
screenshot. Recording a per-frame trace and reading the distribution found all
three in one sitting.

| Cause | Cost | Fix |
| --- | --- | --- |
| A screenshot written from the render thread every 150 frames | eight vblanks, every 2.5 s | Written only when a marker file asks |
| Four colour-conversion threads spinning on short sleeps | seven to eight vblanks at a time | Converted inline on the decode thread |
| Idle artwork threads waking 250 times a second | scheduler noise | Wake 40 times a second |

Result over a thousand frames: iterations longer than 20 ms fell from 0.67 per
cent to 0.10 per cent, and the picture cadence is 190 frames held for two
vertical blanks against 189 for three, which is correct 2:3 pulldown for
23.976 fps content on a 60 Hz display.

The frame trace and the screenshot are now both off unless requested, because
each costs several vertical blanks to write. `tools/trace.sh` records one and
`tools/analyse-trace.py` summarises it.

## 2026-09-10 - video plays correctly

The picture is clean and the frame rate is driven by the display.

### The bug

`Transfer-Encoding: chunked`. A live transcode has no content length, so the
server frames the body in chunks. The streaming reader handed those bytes
straight to the demultiplexer, chunk-length prefixes included, injecting a few
bytes of hexadecimal framing into the video every few kilobytes.

The non-streaming client already decoded chunking, and so does curl, which is
why ffmpeg decoded the same stream perfectly every time it was checked while
the console did not. Every other theory tested along the way was wrong:
surface count, surface recycling, frame alignment, memory protections, cache
maintenance, GPU tiling, field separation, picture-buffer size, profile, level
and the progressive flag.

Two things made it findable in the end. Stamping the frame slot before each
decode proved the decoder writes every row, and drawing a synthetic pattern
through the same path proved everything from conversion to screen was sound.
That left only the bytes arriving from the network.

### Frame pacing

Pacing used to happen in the decode thread with sleeps while the render thread
displayed whatever happened to be present. The two clocks were unrelated and
the picture hitched. Now a short queue of converted frames carries each
frame's position in the stream, the decoder blocks when the queue is full, and
the render thread shows whichever frame has fallen due.

## 2026-09-10 - playback runs; picture quality unresolved

Video plays. The stream is fetched, demultiplexed, decoded on the console's
hardware decoder, converted and displayed at the source frame rate.

| Measure | Value |
| --- | --- |
| Decode | 2-5 ms per frame |
| Colour conversion | 2 ms per frame across four threads |
| Rate | 23-25 fps, paced to the stream's own timestamps |

### What is proven correct

- **The stream.** ffmpeg decodes it pixel-perfect.
- **The demultiplexer.** Its access-unit count matches ffmpeg exactly, and
  ffmpeg decodes the elementary stream it produces pixel-perfect.
- **The plane layout.** Luma at rows 0-815, chroma at 816-1223, pitch 2048,
  confirmed by analysing a raw captured picture.
- **The colour conversion.** Converting a captured picture on a workstation
  with independent code reproduces the same result the console shows.
- **The decoder itself, for keyframes.** A captured keyframe is clean and
  sharp.

### What is wrong

Frames that predict from earlier ones degrade into blocky macroblock
patchwork. Ruled out by experiment: surface count, surface recycling, frame
alignment, memory protections, cache maintenance, GPU tiling, picture-buffer
size, profile and level, and the progressive flag. Matching the reference
client's configuration exactly did not change it.

The remaining difference from that client is presentation: it never reads the
decoded surface with the CPU, handing it to the GPU instead. Reading those
surfaces directly may simply not be supported, which would also explain why
dark frames look clean while bright ones do not.

### Two real bugs fixed on the way

1. **The `error` field is not fatal.** It is a per-picture concealment flag,
   set on a complete, usable picture. Treating it as failure discarded five
   frames in six, which is what made playback look broken rather than merely
   imperfect.
2. **Direct memory is type 12.** Passing the mapping protection where the type
   belongs produced memory the decoder wrote outside of and took the console
   down twice.

### Also

The quality ceiling is not just the decoder. The source is 4K HEVC 10-bit, and
the hardware decodes HEVC only at 8 bits, so it must be converted. The console
can decode 8-bit HEVC at 4K, so the conversion no longer has to drop to 1080p
H.264; the decoder now configures itself from whatever the stream declares.

## 2026-09-10 - browsing works end to end

Signed in, home rows, library grid, artwork, and a remote test loop.

### The bug that mattered

Only the smallest poster ever decoded. It was not the format and not the
server: **the process heap this runtime provides hands out about 2 MiB.** A
decoded 853x480 poster needs 1.6 MiB, so the first allocation won and every
later one failed. Flexible memory is a different pool with roughly 1.7 GiB
free, so `bigalloc` maps large buffers directly and stb_image's allocator is
pointed at it. Every poster decodes now.

Two smaller fixes on the way there: pixels are converted in place rather than
into a second full-size buffer, and worker threads get a 4 MiB stack because
the default is nowhere near enough to decode an image on.

### Working

| Area | State |
| --- | --- |
| Sign-in | Quick Connect, and username with the on-screen keyboard |
| Server | Entered by hand, probed before use, remembered |
| Home | Hero backdrop with title, metadata and overview, plus poster rows |
| Library | Six-across grid with focus and scrolling |
| Search | Keyboard entry with a results grid |
| Settings | Server details, change server, sign out |
| Artwork | Two worker threads, cache, series backdrop fallback for episodes |

### Remote test loop

The console needs no hands to iterate:

- `tools/cycle.sh` closes the running app, launches the new build, and returns
  a screenshot and the trace.
- `tools/press.sh "left down cross"` replays button presses one per frame.
- `tools/shot.sh` pulls the frame the app dumps to disk every few seconds.

The app closes itself with `sceLncUtilKillLocalProcess` when a marker file
appears, and CheatRunner's launch endpoint starts it again.

### Also fixed

- The focus outline drew whole circles at the corners instead of arcs. It is
  now the difference of two rounded rectangles.
- Television overscan: nothing sits within 96 pixels of an edge.
- Row spacing left labels colliding with the next row's title.

## 2026-09-10 - M1 foundation builds and links

Renamed from Prosperofin to SlopFin. Icon is the upstream Jellyfin mark.

Written from scratch, no SDL or RmlUi, so there is less to go wrong:

| Module | What it does |
| --- | --- |
| `gfx` | Double-buffered VideoOut at 1920x1080, alpha blending, rounded rects, gradients, cover-fit image blit, a clip stack for scrolling rows |
| `text` | Noto Sans through stb_truetype, glyph cache, ellipsizing and word wrap |
| `pad` | Edge-triggered buttons with auto-repeat on directions, stick mapped to the d-pad |
| `http` | HTTP/1.1 over BSD sockets, chunked decode, plain HTTP only |
| `json` | Recursive-descent parser with surrogate-pair handling |

### Four link problems, all solved

1. **std::string was undefined.** The boilerplate skeleton never used it. Fixed
   by linking the SDK's own libc++, libc++abi and libunwind.
2. **The build script rejected the archive paths.** Its filter allows only
   `[A-Za-z0-9_.-]`, and `libc++.a` contains `+`. The archives are vendored
   under `+`-free names by `tools/refresh-sdk-archives.sh`, which is what
   ProsperoTV does too.
3. **`pthread` could not be resolved** from the dependent-library specifiers
   embedded in libc++abi and libunwind. The stock link line has no `-L`; ours
   now adds the SDK lib directory.
4. **The FSELF converter refused the binary**, because linking libc++abi
   published C++ ABI symbols and an application must export nothing. The
   version script now makes every defined symbol local.

Also added `src/runtime_stubs.cpp` for the unwind-table bounds and libc's
assert hook, which the minimal crt does not provide.

### Known issues

- `make undeploy` fails with `550 Operation not permitted` while a title is
  nullfs-mounted. Delete the staged folder over FTP file by file instead.
- `.env` does not propagate `PS5_CLANG` to the build scripts, because GNU Make
  does not export variables read from an included file. Pass it on the command
  line.

## 2026-09-10 — M0 complete, toolchain proven end to end

- Host builds on CachyOS with system Clang 22. The boilerplate asks for Clang
  18 by name; `PS5_CLANG=/usr/bin/clang` satisfies it and the output is a valid
  signed FSELF.
- Clean-room `libc.prx` runtime generates reproducibly: two independent builds
  produced identical SHA-256 digests.
- Deploy over FTP to `/data/homebrew/<TITLE_ID>/` works. ShadowMountPlus picks
  the folder up on its 15-second scan, waits for the source to stabilise, then
  nullfs-mounts it into `/system_ex/app/`.
- **Hello World launches and renders on firmware 8.20.** Upstream only claims
  6.02 and 12.70, so this was the real risk in the whole project and it is now
  retired.
- Identity set: PPSA99001, SlopFin, media category
  (`applicationCategoryType` 65536). Deployed and installed.

### Known issues

- `make undeploy` fails with `550 Operation not permitted` while a title is
  nullfs-mounted. Unmount or reboot before removing a staged folder.
- `.env` does not propagate `PS5_CLANG` to the build scripts, because GNU Make
  does not export variables read from an included file. Pass it on the command
  line, or export it in the shell.

## 2026-09-11 — playback controls, surround, HDR decode, native-resolution work

### What works now

- **Playback controls.** Cross plays and pauses, left and right skip ten
  seconds, the shoulders thirty. Because every seek restarts the stream, a run
  of presses is gathered and committed once; the scrubber shows where it will
  land. Up and down move between the scrubber and a row of round buttons;
  circle backs out one layer at a time. Square toggles subtitles, triangle or
  options opens the track lists.
- **Track selection.** Each button opens a popover anchored above itself for
  subtitles, audio and quality, marking what is in force. A text subtitle is
  swapped without interrupting the picture; anything the server must burn in,
  an audio track, or a bitrate cap restarts the stream.
- **Subtitles.** Text formats are requested as external SubRip, parsed on a
  thread of their own, and drawn with a shadow. The first request for an
  embedded subtitle makes the server extract it, which fails until it finishes,
  so the fetch retries.
- **Progress reporting.** Verified against the server: SlopFin appears as a
  live session with its position, and titles appear in Continue Watching.
- **Surround.** 5.1 decoded and played over the eight-channel port, with the
  AAC channel order remapped to the standard speaker order.
- **HDR.** 4K HDR HEVC Main 10 direct-plays and is tone mapped on the console.
  This hardware stores plain 10-bit values in the low bits of each P010 sample,
  not the high bits the format implies.

### Surface resolution, measured

The interface is authored at 1920x1080 and the primitives scale to the real
surface, so glyphs are rasterised at the size they are drawn. Which sizes this
console accepts was then measured:

| Surface | Result |
| --- | --- |
| 1920x1080 | works |
| 2560x1440 | `RegisterBuffers` refuses it, so a 1440p panel cannot be matched |
| 3840x2160 | registers, then the process dies clearing the 33 MB staging buffer |

A probe write to the staging buffer's last word succeeds before that memset, so
the memory is addressable and committed lazily: touching all of it exceeds what
this application may hold. The default is therefore 1920x1080, in code rather
than in a marker file, and a refused size falls back instead of leaving the app
with no surface at all.

The console reports its output as 3840x2160 even against a 1440p panel, so
`sceVideoOutGetResolutionStatus` describes the port rather than the display.

### Verified on the console, not merely built

Driven through the remote harness, with the results read from a file the app
overwrites rather than from the trace, which drops lines:

- Pause, and seeking, which gathers a run of presses into one restart.
- Switching audio track: `apply kind 1 row 1 of 4` followed by a stream restart.
- The quality list: `apply kind 2 row 3 of 16` selecting 20 Mb/s, restarting.
- Subtitles: 360 cues parsed from 24710 bytes and drawn.
- Progress reporting: the server lists SlopFin as a live session with its
  position, and the title appears in Continue Watching.
- Forced transcode targets, checked against the server: automatic leaves the
  stream copied, H.264 and HEVC are honoured when asked for by name, and the
  channel limit and bitrate cap travel with the request.

### Interface

Every screen has now been looked at against real artwork rather than against
the dark placeholders they were laid out on, which is where all of these came
from: a wash that covered less than the text it was meant to sit under, type
sized for a monitor rather than a television, and layouts that left large empty
bands.

- **Home.** The hero wash reaches across the text, the rows sit under it rather
  than below an empty gap, and card labels read as labels.
- **Detail.** Full width, since the sidebar is not navigable there and was
  holding a logo and nothing else. The artwork fills the frame and fades, so an
  item with no episode strip no longer ends in a flat black half-page.
- **Playback.** A scrim whose solid part covers the title and buttons, a
  scrubber with elapsed and remaining times, a row of round buttons, and track
  lists that open as popovers from the button that owns them.
- **Debug.** Behind the info button, in a panel of its own at the top: what the
  file is, what is actually arriving including the measured bitrate, why those
  differ, and whether the pipeline is keeping up.

### Known issues

- A 4K surface cannot be cleared, so native 4K is unavailable until the staging
  buffer is allocated differently or avoided.
- Next and previous episode are not implemented.
- Audio track and bitrate changes restart the stream, which costs a second or
  two of picture.

## 2026-09-12 — GPT playback continuation

- Added the current GPT plan to `PLAN.md` and corrected its obsolete Main10 claim.
- TrueHD now falls back to native-decoded AC-3 5.1; direct native TrueHD module
  loads failed in this app context. See `docs/AUDIO.md` for the precise boundary
  and the successful six-speaker AC-3 capture.
- Split bitrate Limit, media-time payload measurement and Download throughput;
  rejected stale server delivery status and removed the hidden fixed-rate
  negotiation fallback. Frame trace recordings now start with an empty ring.
- HDR output now follows delivered copy/transcode status. Video transcodes use
  SDR metadata and decoder depth, rather than the original HDR file's flags.
- Native-size video blits bypass interpolation, letterboxing clears only the
  bars, unchanged unobscured pictures reuse staging and scanout buffers, and
  controls time out after four real seconds. HDR conversion uses a tested Q14
  matrix instead of per-pixel floating-point conversion and display-mode calls.
- All playback host regressions and five tooling integration tests pass. The
  undeploy dry-run test had a stale boilerplate title ID; it now checks the
  configured title. PS5 build/deployment succeeds with the existing vendored
  stb_truetype unused-function warnings.

### GPT codec/documentation continuation after scope expansion

- Confirmed Avatar: Fire and Ash is DV Profile 7.6 with HDR10-compatible base
  layer (DOVIWithEL, compatibility id 6, PQ/BT.2020). Kept unsupported DV/HLG
  ranges out of the live profile; documented the base-layer integration gate.
- Fixed AAC's sample-rate profile mismatch: 44.1 kHz AAC previously reached a
  sink that silently rejected non-48 kHz PCM. Host tests pass; the live server
  negotiates 48 kHz for 21 Jump Street's stereo track 2 with the appropriate
  sample-rate transcode reason. Actual console audio test remains pending.
- Default audio negotiation now honors DefaultAudioStreamIndex; explicit track
  selection overrides it. This prevents codec fallback decisions based on an
  unrelated first track.
- The 4 Mb/s Pacific Rim test played 1920x1088 SDR HEVC with AC-3 after the
  startup timeout correction. Debug showed Limit 4.0, Media 4.2 and Download
  4.1 Mb/s in the captured VBR window, and the server reported a 4 Mb/s target.
- Rewrote workspace/app entry points, plan, architecture, audio, setup/testing
  guidance and added compatibility, Dolby Vision and documentation indexes.
  Marked the old HDR handoff and inherited tooling/artifact claims explicitly.
- Another agent owns player/display edits and console deployments from the
  user's clarification onward. No subsequent deployment or competing FPS test
  was performed here; this agent's audio/profile changes need the owner's next
  integrated build. Do not attribute their newer cadence changes to these runs.


## 2026-09-18 — GPT-5.6 Sol / Remote Desktop Commander UI + playback pass

- Moved the per-series autoplay toggle to the far right of the playback module
  row and replaced the old Next Episode popup with a bottom-right streaming
  card: widescreen artwork, stronger text hierarchy, embedded face-button
  actions and an edge countdown instead of a floating utility bar.
- Replaced the shared flat white/translucent focus slab used by Settings,
  Account and Dashboard with a dark glass surface: soft shadow, subtle purple
  bloom, hairline rim, left accent rail and top highlight. Subtitle settings
  and single-action settings pages now use the same helper. The main sidebar
  already uses typography plus a moving accent rule and draws no box behind
  its labels.
- Added proper Skip Intro support. Jellyfin Intro media segments provide exact
  start/end bounds when available; named Intro/Opening/Opening Credits/Opening
  Theme/Title Sequence chapters fall back to the following chapter. There is
  deliberately no guessed fixed-duration fallback.
- Skip Intro appears only inside the detected interval and only over a bare
  picture; Cross seeks to the exact end and Circle dismisses it. The normal
  OSD keeps first claim on those buttons while visible.
- Verified on PS5 with X-Men '97: chapter fallback resolved 0:54.542 -> 2:04,
  Cross from inside the interval landed at 2:04, and E-AC-3 HDMI bitstream
  output remained open across the seek. Credits-aware Up Next still fired from
  its real Credits marker with about 112 seconds remaining.
- `make test-playback` and the native `SOFTWARE_AUDIO=1` PS5 build pass. The
  integrated build was deployed over FTP and launched on the console.

## 2026-09-26 — Intro detection coverage and client validation

- Root cause of absent buttons across most shows: Jellyfin 12.1 had no segment
  provider and zero stored media segments. Installed Intro Skipper 12.0.4.0,
  verified FFmpeg chromaprint support, restarted with no active playback, and
  started initial analysis (one worker, two FFmpeg threads). Automatic/new-item
  detection and nightly scanning remain enabled. Verified real Intro/Outro
  records through `/MediaSegments/{itemId}`; full-library analysis is ongoing.
- Extracted marker validation into `src/intro_metadata.hpp`. Added conservative
  OP labels, normalized chapter names, sorted/duplicate chapter handling, exact
  runtime limits, and rejection of invalid numeric bounds. Generic content
  chapters still bound intros; valid server markers override chapter fallback;
  credits fallback remains intact. No fixed-duration guessing.
- Skip Intro dismisses only after an accepted seek, and always consumes Cross
  so it cannot also trigger another playback action.
- `make test-playback` passed, including JSON-based intro regression cases.
  Native `SOFTWARE_AUDIO=1` PS5 build passed and was deployed. On-console
  verification used a detected 41–63 second Intro in *A Knight of the Seven
  Kingdoms* ("The Hedge Knight"): the prompt appeared during playback and
  Cross recorded `intro: skipped to 62 s` in the PS5 trace. Server-side
  detection is live; the initial library scan is still running.
- Setup, coverage limits, and remaining validation: `docs/INTRO_SKIPPING.md`.


## 2026-10-02 — Console controls, keyboard and intro UX

- Replaced pale text focus slabs with shared dark surfaces and cyan outlines
  across Settings, Dashboard, profile, login, sidebar and detail actions.
  Improved muted-label contrast; fallback search keys use quiet dark keycaps.
- Search now uses the PS5 system keyboard when available, preserving scoped
  search and edit/cancel paths. On-console entry returned a visible term and
  matching results. Remote Play cannot capture the native keyboard over this
  app; keyboard appearance/sound on the television was not directly inspected.
- Username sign-in now has editable username/password fields, optional password
  visibility, explicit Sign in, preserved input on failure and clear validation.
  Password entry stays private by default. Native username/password entry,
  show/hide and edit cancellation were verified on console without submitting
  credentials. Original sign-in settings were restored and verified. Real
  password authentication was not tested against the server.
- IME lifecycle regression covers search/password parameters, cancellation,
  failed-open retry, buffer clearing and non-ASCII text. Playback tests pass.
- Intro card stays available above playback controls: Cross over the bare
  picture; Square with controls open. Remaining duration and failed-seek retry
  feedback are visible. Console seeks accepted on South Park S1E1 (end 172.464s)
  and The Office S2E3 (end 80.351s). API sample coverage: 11/12 and 12/12.
- Host captures reviewed for Settings, Dashboard, search and login. Native
  SOFTWARE_AUDIO=1 build deployed. Existing Sep-26 paused-playback crash report
  was retained and dismissed locally, without sending it; no fix claimed here.

- Final shutdown validation reproduced a SIGSEGV from an intentional close,
  symbolized against its exact ELF at main.cpp's present call. The old tooling
  quit path closed VideoOut inside app::frame, then main presented another
  frame through the deleted flip queue. Teardown now belongs to the main loop:
  stop IME/player, release display, finish the session, request process removal,
  and park if termination returns. Native close/relaunch passed with playback
  active: teardown completed, no new crash report and the heartbeat was removed.
  The first check's strict report comparison raced dismissal of an existing
  report; the subsequent close check passed. This is separate from the retained Sep-26
  paused-playback report, whose full cause is not established by this check.


## 2026-10-03 — Grouped search and control alignment

- Retired the app search keyboard. Search always requests the PS5 system dialog,
  with right/top alignment at (1824,54); the OS owns final size/placement. Entered
  text, edit prefill and cancellation were exercised on console. System overlays
  are absent from SlopFin captures and Remote Play returned a black image with
  the keyboard open, so its actual placement/appearance remains a TV check.
- Search uses independently budgeted Movie (40), Series (40), Episode (60)
  requests. Results render as separate horizontal rails, in that order, with
  Up/Down between groups and Left/Right within a group. TV library searches
  include episodes, movie searches stay scoped to movies, and each category
  retains its ParentId filter. Duplicate IDs are filtered within a group.
- Episodes use landscape thumbnails, series name and season/episode context.
  Retry/empty/loading states preserve the editable search. Same-term confirmed
  searches refresh; reopening in a different library reruns with that scope.
  Stale results are rejected by term, library and type. Removed legacy keyboard
  drawing and its unused animation/input state from app.cpp.
- Control labels align to visible glyph bounds. Sidebar, Settings and profile
  labels keep stable font weight; focused backgrounds glide behind all labels
  instead of covering previously drawn text during transitions. Dashboard uses
  the same focus motion and centers single-line content rows. Chip captions and
  server/Continue captions also use visible-letter centering. The Settings
  Subtitles label measures 24/25 pixels of vertical padding in its 68px control.
- Verified host captures: Everything (129 results), Shows / office (1 series,
  13 episodes), episode detail opening, Movies, Shows / south park, no matches,
  Settings pages, profile, Dashboard navigation/content and a focus filmstrip.
  The script's host-only `search TERM` hook uses the real scoped API, without
  simulating the native keyboard. Initial extra capture attempts used a preview
  configuration that had been cleared by scripted navigation; those captures
  were rejected and the scoped/dashboard checks repeated with fresh private
  copies. Console credentials and HTTPS server remained unchanged.
- Native search G returned 40 movies, 30 series and 60 episodes; both vertical
  transitions and cancelled edit preserving G were captured. The final native
  SOFTWARE_AUDIO=1 build is deployed. Playback/IME/group-navigation/motion tests
  passed. A console Settings trace during repeated focus movement
  (8.49 seconds, 509 intervals) had no iterations over 20ms; no screenshot was
  requested during measurement.
- Evidence: captures/search-final, captures/search-verified-states,
  captures/focus-final3/focus-motion.gif, captures/dashboard-finished,
  captures/search-native-series.png, captures/search-native-episodes.png,
  captures/focus-native.csv. The keyboard overlay cannot be supplied as an
  accurate screenshot through the available capture paths.


## 2026-10-03 — Public source cleanup and package preparation

- Removed obsolete app keyboard/GTest remnants, unused state and repetitive
  comments; retained implementation constraints and attribution. Cached public
  C++ runtime archives replace tracked binaries; runtime/font/library notices
  are included in app distributions.
- Configuration has bounded reads, restrictive permissions and atomic writes.
  Reports remain local unless a receiver is explicitly configured and a user
  requests delivery. No implicit developer receiver or Jellyfin-token forwarding.
- Console helpers use local/environment configuration. Library names/IDs come
  from the server; long sidebars scroll and empty Home still opens navigation.
  Search grouping is cached between result changes.
- App/unit, local TLS/network, host, lint and native builds passed. Software
  TrueHD/E-AC-3/DTS decode fixtures matched reference PCM (RMS 0.0000). The
  existing software-audio folder build was deployed with HTTPS/login preserved.
- Native package uses pinned public PS5Upload tooling. Outer format checks pass;
  additional verification decoded 65 blocks and compared all 26 input files,
  including container artwork. A deliberately modified source file was rejected.
- Sony's URL installer installed isolated PPSA99002 on 8.20. Launch failed with
  `0x80020060` at PPR mount before app startup. Read-only probes show the running
  kstuff lacks the newer mount protocol; autoload configuration names 1.10.
  Official 1.11 was downloaded, SHA-256 verified and staged in the autoloader;
  the old payload and configuration are backed up locally. Final native launch
  waits for a user restart/jailbreak. A53 status was read only (stock/native).


## 2026-10-03 — Public repository and fresh native mount test

- Published the clean source root at https://github.com/BeLikeBrett/SlopFin.
  Original development history and private captures remain archived locally.
  README now includes an authorized real PS5 Home capture, codec paths,
  implementation details and explicit experimental limits.
- The first clean GitHub run found formatter-version differences between the
  local Clang 23 and CI Clang 18. Portable zero-initialization and a shortened
  benchmark lambda pass both formatters; the Linux preview rebuild passed.
- User restarted/jailbroke the console. Official kstuff-lite 1.11 is now loaded;
  read-only probes confirm its newer plaintext mount protocol is present.
- With no app mounts active, installed ppr-patch's exact retail 8.20 dynamic
  selector using its non-time-accelerated installer. Exact preflight and all
  mutation readbacks passed. No KMB write-range patch was applied.
- The isolated installed PPSA99002 launch progressed to app0/nest mount entries
  but stalled before SlopFin startup. Kernel logs stop in PPR package mounting;
  no SlopFin process exists. File operations and Remote Play authentication
  then timed out; the payload loader and kernel probes remained responsive.
  Native launch/playback remain unverified, and the folder is still recommended.
  No selector change is attempted while this mount is outstanding.

## 2026-10-03 — Instrumented package mount and published downloads

- A fresh boot loaded official kstuff-lite 1.11 source at
  `85fb88a526e58e357d6f1e626ce05af4acdb8b5d` with observation counters enabled.
  Its matching socket-only reader avoided console log-file writes. The exact
  retail 8.20 selector passed stock preflight and all install readbacks again.
- Isolated PPSA99002 still stalled before app startup. Two snapshots showed
  one successful plaintext profile match, one emulated verifyImage request,
  and both key-index traps applied without malformed outputs or copy failures.
  Hook stage 9 and one outstanding key pair stayed unchanged; mount return and
  cleanup were never reached. This narrows the blocker without identifying a
  specific A53 queue fault. A separate installed-native-app launch was rejected
  before mounting, so post-selector native regression remains unverified.
- Restored and read back the normal official 1.11 autoload entry before the
  repeat native launch. No selector was added to autoload. Recovery boot will
  use the normal runtime; no patch change is attempted during the stalled mount.
- Public main CI and tag CI passed lint, app/network/tooling tests, Linux build,
  runtime reproduction, software audio and native package verification. The
  prerelease at `releases/tag/01.000.000` contains folder ZIP, experimental PKG,
  matching FFmpeg source and checksums. Downloaded the final tag assets and
  verified all checksums; the 26-file folder contains no account configuration.

## 2026-10-03 — Rounded controls, avatar cropping and in-app updates

- Fixed Skip Intro's square shading: the gradient now shares the rounded fill's
  coverage mask. Audited other gradient callers; they are page scrims rather
  than rounded controls. Renderer tests verify excluded corners, symmetry and
  partial coverage. Reviewed a native full-resolution button crop.
- Re-tested successful intro seeks on South Park S1E1 (172.463958 s) and The
  Office S2E3 (80.351 s). Initial Office startup needed time to advance from
  the server's earlier keyframe before the intro range became active.
- Added circular crop editing with D-pad movement, L1/R1 and trigger zoom,
  Triangle reset, matching preview and bounded JPEG export. Native gallery,
  movement, zoom, reset and cancel reviewed. Synthetic host checks verify
  export matches the chosen region; no production avatar was uploaded.
- Search requests the native IME below the field and clears result artwork
  while editing. ABI placement checks pass; the OS overlay is absent from app
  captures, so final TV placement needs observation.
- Added Settings > Updates for GitHub folder releases, SHA-256 verification,
  bounded ZIP staging, a bundled installer helper, backup/rollback and relaunch.
  Native GitHub checking and helper backup/install/relaunch pass, with
  account settings unchanged. The installer preserves executable permissions;
  host checks cover installation and rollback modes. Full GitHub download
  validation is pending the new public release. Details and recovery are in docs/UPDATES.md.
- Host tests, Linux preview, lint and native app build pass. Native FPKG remains
  blocked at the package mount; this work does not establish a native launch.
