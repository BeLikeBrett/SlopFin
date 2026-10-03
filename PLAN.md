# SlopFin — GPT plan

Updated 2026-09-13. This is the active work plan. `PROGRESS.md` records dated
results; `docs/COMPATIBILITY.md` is the current capability table. Older handoffs
and upstream research are evidence to investigate, not current support claims.

## Goal

Reliable playback across Brett's library and other PS5/display/audio setups: correct HDR or an honest fallback,
working stereo/surround audio, responsive controls, and frame pacing that stays
close to the source cadence. Prefer console hardware decode; use measured
software decode where practical, then explicit server fallback. A codec is not
supported merely because a firmware module or symbol exists.

Portable defaults and user observations are specified in
[Playback defaults](docs/PLAYBACK_DEFAULTS.md). Auto must use validated output
capabilities; experimental marker flags are not capability detection.

## Current baseline

- Native H.264 and HEVC Main/Main10 decode; MPEG-TS delivery.
- Native AAC, MP3 and AC-3 decode to PCM. TrueHD-to-AC3 fallback verified.
  512-frame output restores stereo pacing; user reports Silo crackling gone.
  CPU streaming and output fixtures now work for E-AC-3/DTS/TrueHD.
  E-AC-3 HDR playback passes a clean trace; TrueHD Avatar is functional but choppy.
- HDR10 surface switching works experimentally. HDMI metadata, display
  capabilities, peak brightness and smoothness still require validation.
- Browse, search, details, subtitles, track selection, seeking, quality controls
  and progress reporting are implemented. Their presence is not proof that
  every file or channel layout works.
- Bitrate diagnostics now separate selected limit, media payload rate and
  download rate. A 4 Mb/s HDR-to-SDR transcode was verified after fixing startup
  timeout. Repeated trace recordings no longer mix old samples.

## Work packages and acceptance gates

| Priority | Package | Next work | Done when |
| --- | --- | --- | --- |
| P0 | Stereo/surround reliability | Validate ADTS reassembly on more titles; finish output-block pacing tests; investigate physical AC-3/AAC 5.1 crackling after passing PCM tone mapping | Stereo and surround produce continuous correctly timed PCM; each tone reaches its intended speaker; seek/pause/track changes remain reliable |
| P0 | HDR10 and DV base layers | Classify actual transfer, DV profile and base-layer compatibility; test Profile 8.1 then Profile 7 base-layer extraction without modifying originals | Server copies compatible video, only HDR10-compatible data reaches the decoder, TV/HDR state and reference colors agree |
| P0 | Bitrate/restart correctness | Exercise menu changes in both directions, stale session rejection, cold transcode startup and codec/channel overrides | Selected limit agrees with negotiation; diagnostics distinguish targets from VBR measurements; failed requests never secretly choose different settings |
| P1 | Portable playback settings | Implement persistent Auto/Fast start/More buffering; capability-aware HDR Auto with SDR and experimental HDR10 override; separate audio output policy | Settings affect coordinated startup/resume and negotiation; unknown capabilities fall back honestly; device changes and bounded memory are handled |
| P1 | Playback/control performance | Measure clean playback and overlays separately at 23.976/24/25/29.97/50/59.94 fps, SDR/HDR, 1080p/4K decode | Repeatable distributions, no sustained under-delivery, no audio starvation; retained frames never retain old overlays |
| P1 | Native E-AC-3/DTS/TrueHD | Resolve TrueHD movie starvation; extend passing DTS-core/E-AC-3 movie and listening checks to seek/pause/track changes and fallback; preserve opt-in scope | Known fixtures decode with correct channel order/rate/levels and acceptable CPU/memory use; real titles pass seek/pause/track/soak tests |
| P1 | Documentation consolidation | Audit authored docs across the workspace, correct current claims, label inherited tooling/reference docs and historical results | One current capability table and plan, valid entry-point links, no contradictory current playback claims |
| P1 | Intro coverage (2026-09-26) | Intro Skipper installed and initial server scan running; client parser and failed-seek handling hardened; detected Intro seek verified on console | Complete initial scan and inspect uncovered episodes; no every-episode guarantee |
| P2 | Full-resolution 4K presentation | Reduce conversion/scaling/memory bandwidth; assess GPU conversion and direct decoded-surface presentation | Full 4K detail verified against an independent reference at sustained source cadence |
| Research | Dolby Vision over HDMI | Determine display/HDMI APIs, capability signalling and metadata requirements; distinguish real DV output from HDR10 fallback | No support claim without actual TV/DV mode and signal validation; feasibility and licensing remain open |

## Audio strategy

1. Hardware/platform decoders first: AAC/MP3/AC-3 are verified; others are research.
2. Software decoder integration is a separate task requiring codec framing,
   resampling, allocation/threading integration and real CPU measurements.
   Firmware CPU-module presence does not mean this client can already call it.
3. Keep server fallback operational throughout. AC-3 is lossy and limited to
   5.1 here; decoded PCM does not imply Atmos/DTS:X passthrough.
4. Never advertise a sample rate or channel layout the output path discards.

## HDR/Dolby Vision strategy

- PQ/BT.2020 is not HLG. Do not add every Jellyfin video-range enum to the profile.
- Profile 8.1 and Profile 7 can contain an HDR10-compatible base layer. Prove the
  selected file's compatibility and discard DV-only data before advertising it.
- Pure DV/Profile 5 must not be interpreted as ordinary HDR10.
- HLG, SDR-compatible DV and HDR10+ dynamic metadata need their own handling;
  advertising them as implemented would hide failures rather than fix them.
- Native DV HDMI output is distinct from decoding its HDR10 base layer. No
  claimed licensing bypass or TV trick is treated as an implementation.
- Never replace/re-encode library originals for a playback experiment.

## Validation and coordination

Build in `app/` with `PS5_CLANG=/usr/bin/clang`. Run `make test-playback`,
relevant tooling tests and the PS5 build. Use server delivery/FFmpeg logs,
independent reference decodes, speaker captures and isolated frame traces.
Record build stamp, source/track, selected quality, output mode and overlay
state with measurements. Console screenshots alone do not validate HDMI HDR.

One writer/deployer owns a player/console experiment at a time. Preserve other
agents' cadence/display work; do not blend simultaneous deployments into a
performance comparison. Reference repositories stay upstream references;
the workspace index explains their scope instead of rewriting their history.

Deferred product features: next/previous episode, voice-search service, broader
containers/codecs and receiver capability-aware bitstream output.

---

## Opus 5 status notes (2026-09-14)

*Appended by Opus 5. The plan above is the GPT agent's and is unchanged. These
notes record where later work moved its items; the GPT agent may re-prioritise.*

- **P1 Native TrueHD: "Resolve TrueHD movie starvation".** Did not reproduce on
  the current tree in four trials and two unattended soaks (31 minutes): audio
  underrun 0.000 s. The remaining rebuffers were delivery gaps from the server's
  media disk, fixed on the server. Evidence: [software audio](docs/SOFTWARE_AUDIO.md),
  Opus 5 continuation.
- **P1 Playback/control performance, 4K decode.** Serial 4K HEVC (no slices,
  tiles or WPP) and every server `hevc_nvenc` transcode decoded at 14-17 fps at
  pipeline depth 1. Depth 3 plays them at 23.8-24.0 fps; H.264 1080p verified too.
  Evidence: [video decode](docs/VIDEO_DECODE.md). Not yet measured: frame rates
  above 24 fps, and decode headroom beyond the film's own rate.
- **Not in the plan, done:** Continue Watching and Next Up update while the app
  is open ([progress](PROGRESS.md), 2026-09-14).
- **Open, found in passing:** a seek leaves the previous stream's connection open
  on the console; the last two pictures of a stream are not drained at depth 3.

## Opus 5 plan for the next session (written 2026-09-14)

*Appended by Opus 5 from Brett's five requests. Each item records what the code
does today (read, not assumed), the design, how it will be verified, and what
needs Brett's answer first. Suggested order: 3, 5, 1, 2, 4 -- smallest and most
self-contained first; 4 is the largest.*

### 1. Category transitions that feel slow, smooth and satisfying

**Why it looks like jumping today** (`app.cpp`):

- `card_reveal` runs each card for 15 frames (250 ms) on a cubic ease-out, so
  about 70% of its 22 px rise happens in the first four frames: a jump, then a
  stop. The rise is cast to whole pixels, so the last few frames stair-step.
- Switching category clears the old cards at once (`grid_items.clear()`,
  line ~4106) and shows "Loading" for the server round trip, then deals the new
  cards. Blank, flash, jump. With posters now warm in RAM the art is ready
  instantly, so the reveal is the only motion there is.
- The stagger is fixed per column (2.2 frames) and row (3.6), so two cards
  (Dragonball) arrive together while a full grid ripples unevenly.

**Design:**

- **Hand over instead of replacing.** Keep the old cards and fade them out
  (about 150 ms, no movement) while the request is in flight; the new ones deal
  in as they arrive. "Loading" only appears if nothing has arrived after 400 ms.
- **Longer, gentler entrance per card:** about 550 ms, opacity leading the
  movement (fully opaque by ~60%), a smaller rise (about 12-14 px) with a slight
  scale from 0.97, on a softer deceleration (a critically damped curve like
  `motion::kFade`, or quintic ease-out -- chosen by filmstrip, not by eye).
- **Count-aware cascade:** one diagonal sweep from the top-left across the
  cards actually on screen, with a fixed total budget (about 300 ms) divided
  among them, clamped to 1.5-4 frames per card. Two cards still arrive one after
  the other; twenty do not take forever. Off-screen cards arrive settled.
- The section title fades with the first card. Home keeps its `home_epoch` key,
  so live Continue Watching refreshes still never re-deal.

**Verify:** `tools/filmstrip.py` for acceleration and settle (no stair-step in
the final frames); a `tools/trace.sh` window on the console across repeated
switches, since alpha-blended cards cost render time; and a short screen
recording from the Linux preview for Brett, as with the first reveal.
**Brett decides:** two recorded variants side by side (for example a 450 ms
"snappy-smooth" and a 650 ms "relaxed") and he picks.

**Status 2026-09-14:** built and committed (not deployed). Filmstrips found two
existing flashes as well -- a one-frame full-opacity flash of new cards, and
"Loading" on every switch -- both fixed. The entrance, count-aware cascade and
loading delay are in; the old cards are still cleared rather than faded out.
Variants of 0.45 s and 0.65 s were recorded and sent to Brett. Once he picks:
set `SLOPFIN_REVEAL_FRAMES`, deploy, and take a console `tools/trace.sh` across
repeated switches for render cost.

### 2. Subtitle sync, then a delay control that makes sense

**What the code does:** the text cue shown is `subtitles::current(position)`,
where the position is the timestamp of the picture actually on screen
(`player.cpp`, `frame()`), and the SubRip file is requested with absolute title
times (`.../Subtitles/<index>/0/Stream.srt`). Nothing in that path explains an
offset, so **measure before changing anything.**

**Result of A (2026-09-14): found and fixed.** Every stream the server muxes
runs 1.400 s ahead of the film -- ffmpeg's MPEG-TS muxer shifts timestamps by
twice its 0.7 s default mux delay -- and SlopFin took stream time as film time,
so every subtitle showed 1.4 s early. Measured on four titles against the source
files, confirmed in the console's own log, corrected in `src/stream_clock.hpp`.
The original measurement plan is kept below for the record.

**A. Find out what "kind of off" is.** Three titles covering the paths that
differ: a remux copy with an embedded SubRip track, a file with an external
`.srt`, and a server transcode; plus one ASS track converted to SubRip. For a
line that starts after silence, compare speech onset (audio captured from the
sink) with the moment its cue is drawn (frame trace), and cross-check the same
line in Jellyfin Web on the PC as the outside reference. The pattern decides the
fix:

- the same offset everywhere: a client clock bug (audio versus picture lead,
  output latency) -- fix globally;
- only on some delivery paths: the start-time mapping -- in particular
  `title_seconds()` when timestamps are not copied, where the stream starts at
  the keyframe before the requested position, and container start-time offsets;
- only on some files: the subtitle file itself -- the delay control below is
  the answer.

**B -- implemented 2026-09-14** as described below, per title; "Sync to the next
line" remains a stretch goal.

**B. The delay control**, in the player's Subtitles panel, text subtitles only
(burned-in subtitles are part of the picture and cannot be shifted):

- Two plain choices describing what the viewer sees, not the maths:
  **"Subtitles late -- show sooner"** and **"Subtitles early -- show later"**,
  each 0.1 s per press, faster when held.
- The current state as a sentence, never a signed number: "Showing 0.4 s
  sooner", or "Original timing". A Reset row.
- The panel sits clear of the subtitle area, so the line being adjusted stays
  visible while adjusting.
- Stretch goal, likely the most intuitive of all: **"Sync to the next line"** --
  press Cross the moment the next line is spoken, and the offset is set from
  that press (limited to +/-10 s).
- Remembered per title on the console, with the offset shown in the debug
  overlay.

**Decided (Brett, 2026-09-14):** remembered per title.

### 3. Triangle opens straight onto the settings row

*Revised 2026-09-14 after Brett's answer. The first draft proposed remapping the
controls; that was wrong and is withdrawn.*

**Today** (`handle_player_input`): triangle does nothing while the controls are
hidden -- on purpose, per the comment at line ~330 -- and while they are shown
it switches focus between the timeline and the settings row.

**Wanted:** exactly the same, with one change. **Triangle while the controls are
hidden shows them with the settings row (subtitles, audio, quality) already
focused**, instead of doing nothing. Everything else stays as it is: triangle
still toggles between the row and the timeline once the controls are up, Up is
still the timeline, Down still the episode buttons, Circle still steps back.

**Done 2026-09-14**, verified in the preview.

### 4. Subtitle appearance settings, grouped by kind

**What the library holds** (5,307 movies and episodes on brettserver): SubRip
14,993 tracks (1,080 of them external files), ASS 3,139, PGS 1,757, mov_text
705, DVD 336.

**What the app does today:** it draws only SubRip -- 42 px Noto Sans Medium,
white, a 2 px black outline drawn as four offset copies, fixed 132 px above the
bottom (`draw_subtitle_lines`). Other text formats (ASS, mov_text) are converted
to SubRip by the server, so ASS styling, positioning and signs are lost. PGS and
DVD are listed as `Encode` (`jellyfin.cpp` ~712), so the server burns them into
the picture -- a full video re-encode, at 4K on the P4.

**Settings -> Subtitles, in two groups named for people, not codecs:**

- **Text subtitles** (SubRip, MP4 text, and styled ASS shown as plain text):
  everything is drawn by the app, so all of it is cheap to offer -- size (four
  presets), font (Noto Sans, plus one high-legibility face such as Atkinson
  Hyperlegible, OFL-licensed), weight, colour presets (white, yellow, soft
  cream -- no free colour picker), edge (outline, drop shadow, none),
  background box (off, translucent, solid), vertical position, and an automatic
  lift while the controls are showing. A live sample line over a backdrop shows
  every change as it is made.
- **Picture subtitles** (Blu-ray PGS, DVD): these are images, so they cannot be
  restyled. The useful setting is what to do with them: burn in (today), or
  prefer a text track of the same language when one exists, avoiding the
  re-encode. Drawing PGS on the console instead of burning it in (it is simple
  run-length bitmaps) is a research item, recorded but not in scope.
- **Styled ASS kept as styled** needs an ASS renderer (libass with FreeType,
  HarfBuzz, FriBidi): large, and recorded as research only.

**Verify:** preview captures with `tools/look.py` for size and position;
`tools/trace.sh` with a large, background-boxed cue showing, because every cue
already disables frame reuse and adds text rendering to each frame.
**Also:** NOTICE.md does not credit the bundled Noto Sans (OFL) today; add it
along with any new font.

### 5. A Details button on the title screen

**Today** the detail buttons are Play/Resume, Start from beginning, Watched,
Version and Go to series (`detail_actions`). The detail request already asks
for `fields=MediaSources` (`jellyfin.cpp:463`), so the file facts arrive and
are simply not parsed. Checked on Backrooms: Path, Container `mkv`, Size
77,884,008,652 bytes, Bitrate, and per stream codec, profile, level, bit depth,
frame rate, `VideoRangeType` and `VideoDoViTitle` ("Dolby Vision Profile 7.6").

**Design:** a rightmost **Details** button opening a properties sheet that
follows the selected version, scrolled with the D-pad, closed with Circle:

- **File:** name, folder, container, size (GB plus exact bytes), overall
  bitrate, runtime, date added (`DateCreated`, one field to add to the request).
- **Video:** codec and profile, resolution, frame rate, bit depth, HDR or
  Dolby Vision type.
- **Audio (n)** and **Subtitles (n):** language, format and profile, channels,
  default/forced/SDH, embedded or external.
- **On this PS5:** the negotiation SlopFin would run on Play -- direct, remux or
  transcode, and why -- requested only when the sheet opens. This is the
  section that explains surprises, such as a PGS track forcing a re-encode.

**Status 2026-09-14:** File, Video, Audio and Subtitles are built, tested against
Backrooms' server values and checked in the preview on Avatar. **"On this PS5"
is not built yet**: before Play it needs its own PlaybackInfo negotiation, and
the delivery line the player shows today only exists once a stream is running.
Next step for it: request PlaybackInfo when the sheet opens and describe the
server's own TranscodeReasons, rather than guessing copy versus transcode.

### Opus 5 status and open items (2026-09-14, late)

*Appended by Opus 5.*

- **Done:** all five requests. Category transitions (B, fade-out, both flashes
  fixed), per-title subtitle timing, the subtitle clock fix, triangle,
  Details with "On this PS5", Settings as screens, subtitle appearance.
- **Open -- one long draw per category switch.** About 22 ms, once per switch,
  measured on the console. Not in the grid or fade-out draw (timed); something
  else in that frame. Next: time the frame's other stages the same way.
- **Open -- frames dropped soon after launch.** 24 s after launch both the
  pre-transition build and the current one drop many frames (238 and 432 long
  frames in 20 s); long after launch the same switches are nearly clean. Suspect
  the post-launch artwork warm and first-draw work; measure before changing.
- **Open -- the overlay's "HDR" can claim more than the TV receives.** For an
  HDR title SlopFin changes its own buffer to HDR10
  (`sceVideoOutSubmitChangeBufferAttribute2`, result 0) whatever the PS5's HDR
  setting is, and "HDR10 mode acknowledged" only means that change was accepted.
  Brett has PS5 HDR off and still sees HDR in the overlay. Asking the console
  what the HDMI output really is failed on this firmware:
  `sceVideoOutGetCurrentOutputMode` 0x80290018 and
  `sceVideoOutGetDeviceCapabilityInfo` 0x80290001, both returning zeros for an
  HDR and an SDR title alike. Until a working query is found, the TV's own HDR
  indicator is the only authority; the overlay should say "HDR10 requested"
  rather than imply the TV is in HDR. Relevant to the GPT agent's HDR Auto work.
  **Update, same day:** the overlay now says "HDR10 requested (TV not
  confirmed)"; at Brett's request the console's `/data/slopfin-hdr-auto`
  marker is renamed to `slopfin-hdr-auto.disabled-by-opus5`, so HDR sources are
  tone mapped to SDR by the app (24.00 fps on Backrooms and Avatar after the
  playback thread was moved off the render core).
  **Update, 2026-09-15 (Opus 5):** step 1 is done and the marker is retired.
  SlopFin reads the PS5's HDR setting (`sceVideoOutGetOutputStatus` byte 4) at
  each title: On When Supported gives HDR10 buffers for HDR titles, Off gives
  SDR. Backrooms measured 24.01 fps shown, 0 dropped in HDR10. Still open:
  1. *Does the TV switch?* Nothing on this firmware reports the HDMI link
     (AvSetting monitor queries refuse a payload; Remote Play's HDR stream is
     tagged PQ even on SDR screens). Brett's TV indicator is the check.
  2. *If it does not:* Brett's registry has Deep Colour (`VIDEOOUT_color_depth`)
     at 0 against a factory 1 and resolution (`VIDEOOUT_mode`) at 4 against a
     factory 19. Those are worth reading in the menu before touching
     `sceVideoOutConfigureOutputMode_`, which once wedged video output.
- **Done (Opus 5, 2026-09-15, late): AC-3, E-AC-3 and DTS play as bitstream in
  the player** (`src/iec61937.hpp`, `audio.cpp`); awaiting Brett's ears on real
  titles. Next: TrueHD, below.
- **In progress (Opus 5, 2026-09-15): Dolby/DTS bitstream to HDMI.** Proven at
  the TV: AC-3 and E-AC-3 play as bitstream from SlopFin, DTS shows the TV's DTS
  badge. Now: pass the server's copied AC-3 and E-AC-3 straight through in the
  player instead of decoding to PCM (Brett's request). Open: TrueHD reaches the
  TV as a recognised Dolby stream but is silent; the disc player builds its own
  MAT frames and uses the Sys calls -- leads in `tools/bitstream/README.md`.
- **Future (Brett, 2026-09-14): proper sign-in, sign-out and profile
  switching.** Settings -> Account is the place for it; today it only signs out.
  **Update, 2026-09-15 (Opus 5):** a profile badge and menu (Profile, Dashboard,
  Sign Out), a Profile screen (picture from captures or USB, password) and an
  administrator dashboard exist; see [profile and dashboard](docs/PROFILE_AND_DASHBOARD.md).
  Still open: switching between users without signing out, and running the
  password change and dashboard actions against a server where that is safe.


## 2026-10-02 UI and intro continuation

Completed: shared dark/cyan focus controls, native PS5 search keyboard,
editable credential form, persistent intro affordance above the OSD and retry
feedback. Host/IME regressions and console seeks on South Park/The Office
verified; see PROGRESS.md. Remaining: real credential authentication test with
an authorized test account, TV review of native keyboard appearance/sound,
full intro detection coverage and the retained Sep-26 paused-playback crash.

First-launch follow-up (2026-10-02): empty server default, explicit address review,
address normalization and validation implemented. DNS names, actual HTTPS and
server base paths added through the platform transport. IPv6 remains future work.


Search/focus follow-up (2026-10-03): native-only search keyboard with top-right
placement request, independent Movie/Series/Episode result rails and category
scoping implemented. Visible-glyph centering and shared focus springs applied
to Settings, sidebar, profile and Dashboard. Console entry/cancel/group navigation
and Settings timing verified; native overlay appearance/placement needs TV
review because app captures omit it and Remote Play streams black.


## Public distribution and native packaging (2026-10-03)

Completed: source/comment cleanup, dependency/license separation, private and
atomic configuration, opt-in report delivery, portable console tools, dynamic
library navigation, network/host/native checks and a reproducible optional
native package with full payload verification. Ship a recommended folder ZIP
and an experimental native package. Native installation passed on 8.20; mount
and launch must be retested after a restart with current kstuff/A53 support.
Do not promote package installation or format checks to playback validation.
