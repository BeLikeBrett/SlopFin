# Search and playback validation — 2026-10-23, SlopFin 2.00 candidate

The native GPU/software-audio build stamped `Oct 23 2026 21:53:20` was
installed through the verified updater and restarted successfully. The process
ID changed and the protected account/settings SHA-256 remained identical.
Version remains `02.000.000` (2.00); GitHub has not been published.

- Full `make test`, `make host`, `make lint` and the native build passed. Shared
  Home/Discover voice snapshots cover preparation, download, listening, mute,
  processing and errors. Listening uses Cross Finish beside the field, with no
  visible Circle Cancel prompt. Circle still cancels capture. Idle Search keeps
  R3 Speak and the typing hint directly below the field.
- Discover has its own local voice ownership and result handoff, retains the
  selected catalogue scope, and cancels its capture when leaving. Triangle opens
  Search without starting the keyboard; filters stay outside Search. Regression
  checks cover keyboard dismissal, navigation, voice transitions and result scope.
- The user confirmed on PS5 that Cross opens the keyboard and Circle dismisses
  it while Search remains open. The final IME implementation shares fixed search
  coordinates matching the user's system screenshot on every opening. Native
  keyboard overlays cannot be validated from application-only frame dumps.
- Native playback checks exercised actual Play and Resume. Exiting at 46.421
  seconds immediately retained that early point locally and resumed from it.
  A later exit at 630.713 seconds matched the live Jellyfin playback position
  exactly. Jellyfin's configured 5% minimum can discard early server bookmarks;
  local early progress is retained in the running client, not a durable bookmark
  journal. The title's original watched state and server progress were restored
  after testing; server policy was not changed.
- Ordered final reporting retries transient failures, flushes pending stops
  before refreshing progress rows, and isolates queued account credentials.
  Worker tests cover fractional timestamps, retries, timeout, account changes
  and bounded queue behavior. Stop follows decoder teardown to avoid late starts.
- Final native Home Search shows the inline typing hint and R3 Speak. Discover
  R3 opens the real local capture path, respects the muted controller microphone,
  and displays Cross Finish beside the field without a Circle Cancel prompt.
  Circle cancelled capture back to idle Search without leaving it. This final
  check established capture/controls, not a new spoken accuracy result.
- The 45-file folder ZIP is 12,281,352 bytes, with 54,603,395 bytes extracted.
  Current and actual 1.08, 1.09, 1.12, 1.13 and 1.17 updater readers accepted it;
  the current stager produced the folder used for this native installation.

Evidence: `/tmp/slopfin-playback-fix/`, `build/tests/voice-search-*.png` and
`build/tests/discover-voice-*.png`. No new HDMI/audio-output claims are made.

---

# Search and media controls validation — 2026-10-23, SlopFin 2.00 candidate

The native 4K GPU/software-audio build stamped `Oct 23 2026 09:15:18` was
installed with the verified updater on PS5 firmware 8.20. The helper restarted
the client with a new process ID; the protected account/settings file hash
was unchanged. Version remains `02.000.000` (2.00).

- Full `make test`, `make host`, `make lint` and the native build passed. After
  the final search-control refinements, the focused voice UI regression and
  clang-tidy checks passed again. Cross opens the keyboard from an empty
  search without starting capture; R3 explicitly starts speech. Existing
  result navigation and opening titles remain available.
- User-approved Resume groups a solid progress ring, label and remaining time
  in one transparent resting action. The PC client captures include Start over
  and the compact Circle Close hint in file details. GPU/CPU comparisons passed
  at 720p, 1080p, 1440p and 4K, with a maximum channel difference of 1/255.
- Watched-state checks cover authoritative Jellyfin responses, stale resume
  removal, cached rows and detail pages, focus when actions disappear, failure
  handling, and per-episode Autoplay ordering after Debug at the far right.
- Local speech checks cover the verified first-use download, real model loading,
  reference-audio transcription, cancellation/restart, silence gating and memory
  sanitizer runs. A spoken PS5 check confirmed controller input and on-console
  transcription; the user reported it works well. Unusual short names and
  repeated phrases can still need correction. Broader natural-voice accuracy
  and additional microphones are not established by this check.
- Console captures show the subtle R3 label, inline Cross typing hint and
  bottom-right Circle Close prompt. Search never starts recording just by
  opening it. Audio and recognised queries are not added to diagnostic logs.
- The final 45-file folder ZIP is 12,266,418 bytes, with 54,579,795 bytes
  extracted. Current and actual legacy updater readers from 1.08, 1.09, 1.12,
  1.13 and 1.17 accepted it within their existing size/count limits. The voice
  model is downloaded separately and persists outside the update folder.

Evidence: `/tmp/slopfin-voice-ui/` and `build/tests/voice-search-*.png`.
GitHub remains unpublished pending release approval.

---

# Validation record — 2026-10-23, SlopFin 2.00 subtitle and library fixes

The native 4K GPU candidate built `Oct 23 2026 04:40:37` was installed on PS5
firmware 8.20 through the bundled verified updater. The helper restarted the
app with a new process ID; the protected account/settings file SHA-256 was
identical before and after installation. Version remains `02.000.000` (2.00).

- `make test`, `make host`, `make lint` and the native GPU/software-audio build
  passed. Targeted checks cover six real font families and both weights,
  independent colour persistence and resets, subtitle metadata parsing,
  duplicate-track labels, latest-series queries and accelerated held scrolling.
- Renderer fingerprints differ across all twelve subtitle faces, including
  face indices above seven. The expanded glyph cache key prevents those faces
  from aliasing older faces. Monospace spacing is checked independently.
- GPU/CPU comparison with the new font faces passed at 720p, 1080p, 1440p and
  4K. Maximum channel difference was 1/255 at 4K and zero at the other sizes.
- The final 43-file folder ZIP is 11,187,353 bytes, with 52,027,995 bytes
  extracted. Current and actual legacy updater parsers from 1.08, 1.09, 1.12,
  1.13 and 1.17 accepted and staged it within their published size/count limits.
- Native captures show transparent resting Resume actions with retained
  progress, the expanded subtitle controls and the new packaged fonts. A held
  down input traversed TRON's long properties list to its lower rows; subtitle
  labels retain Spanish (Castilian), Spanish (Latin American) and other names.
- Live read-only Jellyfin queries reproduced missing Beerus: HidePlayedInLatest
  excluded that watched series. The replacement series query includes it first
  by DateLastContentAdded, without altering account settings or watched status.

Evidence: `/tmp/slopfin-font-options/`, `/tmp/slopfin-subtitle-polish/`, and
`build/tests/subtitle-*.png`. No new physical HDMI/audio-output claims are made
by these UI checks. GitHub has not been updated or released.

---

# Subtitle and focus validation — 2026-10-23, SlopFin 2.00 candidate

- Play/Resume uses a dark resting face. White follows the actual focused media
  action; moving to Start over, Watched or Details clears the Play highlight.
- Text edges use native-resolution font contours and one combined coverage
  mask per line. Overlapping letters cannot double the selected edge opacity.
  A bounded cache reuses font geometry and the last 12 line masks. Soft shadows
  use a feathered contour. Font fill and picture-subtitle artwork retain their
  existing rendering paths.
- Edge thickness (Fine/Normal/Strong) and opacity (Subtle/Balanced/Solid) are
  saved independently. Missing fields in older configurations use the previous
  thickness and opacity. Existing edge indexes are unchanged. Text reset clears
  the new fields without changing picture preferences; picture reset leaves
  text settings alone.
- Settings show identical native-size samples on bright and dark backgrounds.
  Irrelevant edge/background controls are dimmed and skipped, the right stick
  navigates selectable rows, and the list scrolls with its existing spring
  motion. Long text wraps at word or UTF-8 boundaries; “Near the top” now uses
  an actual top anchor.
- Full `make test`, native GPU/audio build and `make lint` passed. The expanded
  controller suite passed separately after adding right-stick coverage. GPU/CPU
  comparisons of outlines and shadows passed at 720p/1080p/1440p/4K, with maximum
  channel difference 1/255 and no differences above 2/255. Host visual captures
  cover every edge type and all five font sizes on bright/dark backgrounds.

Native console checks used build `Oct 23 2026 00:29:39`, followed by the
startup-preparation build `Oct 23 2026 04:04:58`. The final install/restart
kept the protected settings file byte-for-byte unchanged before further UI tests.
The bundled helper installed and restarted the app with a different PID.
After interacting with Settings, the config serialization includes the two new
fields; semantic persistence/reset is covered by the legacy and current config
tests. The console showed the new Outline preview and a dark Resume button
while Start over held the white focus highlight.

The 12-second settings interaction capture recorded a 16.683 ms median and
17.910 ms p99, with three iterations over 20 ms while cycling styles and
picture previews (maximum 61.069 ms). Fresh font geometry is cached; common
subtitle glyphs are now prepared at startup and when leaving subtitle settings,
before playback. This does not promise that every first-time preview is free
of preparation cost.

The initial playback-start capture included buffering and 10 dropped frames;
a separate warm 12.25-second 4K HEVC/TrueHD/text capture showed all 294 frames
(24.01 fps), zero dropped frames, decode failures, audio errors, underruns or
rebuffer events. These are different measurement windows, not interchangeable
results. After startup preparation, a fresh playback check (recording began
three seconds after the play request) showed 23.94 fps across 13.37 seconds,
with zero dropped frames, decode failures, audio errors, underruns or rebuffer
changes. This is recorded in `ps5-prepared-text-analysis.txt`. ASan/UBSan passed
native-resolution contour, alpha-overlap, UTF-8 wrapping and image presentation
checks.
Evidence: `/tmp/slopfin-subtitle-polish/` and `build/tests/subtitle-*.png`.

---

# Validation record — 2026-10-22, SlopFin 2.00 candidate

The candidate retains native 4K GPU menus and the existing playback pipeline.
Console checks below used firmware 8.20. The final native executable reports
`Oct 22 2026 22:51:14`; version metadata is `02.000.000`.

| Check | Result and limits |
| --- | --- |
| `make test`, `make host`, `make lint`, GPU native build with `SOFTWARE_AUDIO=1` | Passed. Includes actual app shutdown ordering, safe helper close markers, legacy configuration save/reload, controller navigation and local verified-TLS tests. Native container integrity checks passed. |
| GPU/CPU compositor comparison, 720p/1080p/1440p/4K | Passed; maximum channel difference 1/255. Backdrop scrims stay on the GPU without CPU synchronization. |
| Backdrop submission benchmark, host | CPU composition averaged 31.356 ms and uploaded 33.178 MB per frame; direct GPU drawing averaged 0.052 ms and uploaded 0.346 MB. These are host submission costs, not PS5 GPU speed ratios. |
| PS5 idle menu, 780 iterations / 13 s | Loop median 16.684 ms, p99 16.779 ms; maximum 18.560 ms; zero iterations over 20 ms. |
| PS5 tab switching, 12 Home/Discover changes / 17 s | Loop median 16.684 ms, p99 16.800 ms; maximum 18.708 ms; zero iterations over 20 ms. No screenshots during timing capture. |
| PS5 4K HEVC HDR10, TrueHD CPU audio, PGS overlay route | 314 decoded/converted/shown frames over 13.11 s (23.95 fps), zero dropped frames, decode failures, audio underruns, audio errors or rebuffer events. Diagnostic screenshots do not establish HDMI signalling or speaker output. |
| PS5 menu after HDR playback, final candidate | 750 iterations; median 16.684 ms, p99 16.811 ms, maximum 18.600 ms; zero iterations over 20 ms after the playback display returned to the GPU menu. Text subtitle playback separately fetched 1,162 cues successfully without video conversion. |
| PS5 Circle after direct playback | Reproduced the empty detail model and “Nothing to show”. All six playback exit branches now share one return function, which preserves a populated destination or opens the playing item from its known metadata, with a Home fallback. Fresh-console direct playback returned to the full TRON title page. Episode → Circle restored the Edgerunners series page. Host controller checks also verify OSD dismissal and retained series viewport. |
| PS5 helper install/restart, final candidate | Installed the staged release through the bundled helper, closed only after display/decoder cleanup, restarted automatically with a new process ID. Protected account/settings file SHA-256 stayed identical to the original 1.17 installation. This did not download an unpublished release from GitHub. |
| Release ZIP compatibility | The actual 31-file release ZIP fits existing 32 MiB / 128 MiB / 256-entry limits. Actual updater code from tags 1.08, 1.09, 1.12, 1.13 and 1.17 selected and staged it successfully on the host. Rollback was fault-injected at every replacement position; configuration remained unchanged. Archive tests also passed ASan/UBSan. |
| Picture subtitle and audio negotiation regression | Host PGS decode/timing/clear/reset/bounds tests and TrueHD fallback/HDR negotiation tests passed. Earlier DVD sizing/placement and text/picture visual evidence remains separate from these new checks. |

Older native shutdown code can block in its launcher termination RPC after
releasing the display, preventing its updater from proceeding. The 2.00 helper
handoff fixes future closes, but cannot repair a running older executable.
If an old app reports “The app did not close”, close it from the PS5 system
menu and replace the complete folder once, retaining `/data/slopfin/config.json`.
Versions 1.07 and earlier already require the documented manual upgrade.

Evidence for this audit is retained locally under `/tmp/slopfin-2-audit/`.
No GitHub release was published. Native package installation, other firmware,
receiver/speaker output and a public 2.00 download remain outside this check.

---

# Validation record — 2026-10-05

This separates new checks from existing console evidence. Firmware 8.20 remains
the main console setup; this record does not certify other firmware or HDMI devices.

## New checks for 01.000.003

| Check | Result | What it establishes |
| --- | --- | --- |
| `make test` | Passed | Settings save/reload and request preferences; TrueHD CPU decoding takes priority over server conversion when both audio settings are on; delivery descriptions distinguish CPU, platform and receiver decoding; Square skips with/without timeline and Cross does not; open panels suppress skipping; existing playback/update regressions. |
| `bash tools/test-software-audio.sh` | Passed | TrueHD 5.1, E-AC-3 stereo/5.1 and DTS 5.1 decoded PCM matched the independent reference with RMS error **0.0000**. Fragmented input, TS, EOF, cancellation and reopening passed. This is a Linux decoder check, not PS5 HDMI/speaker validation. |
| Synthetic settings/intro render captures | Reviewed | Shared renderer has readable settings, menu scrolling and one Square intro prompt; no personal account, server or media data used. |
| `make host`, PS5 build with `SOFTWARE_AUDIO=1`, `make lint` | Passed locally | Signed native containers have valid integrity and zero static FSELF errors. Release workflow also verifies the build. No Linux check establishes console output behaviour. |
| Final console updater download/install/restart | Pending | FTP was reachable but elfldr on port 9021 refused connections. A previous updater result file alone does not prove this build passed. |

## Earlier console evidence

- H.264 / HEVC Main/Main10 and selected 4K sources: [video pipeline](VIDEO_DECODE.md).
- Dolby Vision Profile 7 / 8.1 compatible base-layer decode:
  [base-layer evidence](DOLBY_VISION.md). Native Dolby Vision output is unsupported.
- HDR surface registration and both output transitions on firmware 8.20:
  [HDR evidence](HDR.md). HDMI signalling/brightness still need physical verification.
- CPU audio fixtures and limited movie/soak trials:
  [software audio](SOFTWARE_AUDIO.md). `tests/test_delivery_summary.cpp` records the
  Avatar HEVC/TrueHD-copy console session used for its fixtures.
- AC-3 channel ordering, compressed-audio byte comparison and **audible native
  passthrough** on the test TV: AC-3 5.1, E-AC-3 7.1 and DTS 5.1 tones, with DTS
  codec badge, recorded on 2026-09-15. [Audio](AUDIO.md) links the original listening
  log. TrueHD passthrough trials produced beeping/silence and did not pass.
- Intro skips on two episodes, including with controls visible:
  [Intro checks](../INTRO_SKIPPING.md). The new consistent Square routing is tested
  separately in `tests/test_player_controls.cpp`.

A known decoding result can be stated as tested while receiver/speaker/display
output remains pending. Avoid a blanket “experimental” label that hides that distinction.
