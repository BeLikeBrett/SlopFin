# Intro skipping

SlopFin reads Jellyfin's `/MediaSegments/{itemId}` when loading episode details.
Valid `Intro` start/end ticks take priority over named chapters. During that
interval, **Skip intro** appears with a remaining-time label. Cross skips to
its end over the bare picture, and Circle dismisses the prompt. With playback
controls open, the card sits above the controls and Square skips; Cross retains
its pause/select role. Open track/settings panels hide the card until closed. A failed seek no longer consumes
the prompt permanently or reports success. Reopen the episode after new server
markers are generated; markers are loaded with episode details, not polled.

## Detection and coverage

The server must generate timestamps. Installing the client alone does not analyze
episode audio. Install a compatible [Intro Skipper](https://github.com/intro-skipper/intro-skipper)
release, run **Detect and Analyze Media Segments**, and retain automatic detection
and a recurring scan for new content. See Jellyfin's
[media-segment documentation](https://jellyfin.org/docs/general/server/metadata/media-segments/).

On 2026-09-26 Brett's Jellyfin 12.1 server had no segment provider and zero
stored segments. Intro Skipper 12.0.4.0 was installed and loaded with a restart
while no playback was active. Its initial scan was started with one parallel
worker and two FFmpeg threads; automatic detection and the default nightly scan
remain enabled. Generated Intro/Outro timestamps were verified through the same
API SlopFin uses. The initial library-wide scan is ongoing, not complete.

Detection coverage depends on the media and provider: unique/changing intros,
short or unusual openings, and insufficient comparable episodes can still lack
markers. There is no guarantee for every episode. SlopFin never guesses an intro
duration or treats an unnamed first chapter as an intro.

## Chapter fallback and validation

- Explicit Intro/Opening/Opening Credits/Opening Theme/Title Sequence and OP/OP1/
  OP 2 chapter labels are recognized, ignoring case and surrounding whitespace.
- The next distinct chapter timestamp bounds the intro, including generic scene
  chapters. Unsorted entries and duplicate timestamps are handled safely.
- Cold opens, recaps, generic chapter labels, and explicit intro-end markers do
  not become skip ranges. A missing end cannot be inferred from the runtime.
- Nonfinite, negative, reversed, and out-of-runtime ranges are rejected. Chapter
  fallback ranges must be longer than one second and at most five minutes.
- Closing-credit chapter markers remain available to Up Next when server Outro
  metadata is absent.

`make test-playback` includes `tests/test_intro_metadata.cpp` with actual Jellyfin
JSON response shapes, malformed bounds, cold-open offsets, chapter boundaries,
runtime limits, and server-over-chapter precedence. The native build uses
`make PS5_CLANG=/usr/bin/clang SOFTWARE_AUDIO=1`.

The 2026-09-26 client changes passed host tests and the native software-audio
build, were deployed to the PS5, and were verified on-console with a detected
Intro segment in *A Knight of the Seven Kingdoms* ("The Hedge Knight").
The prompt appeared inside the reported 41–63 second interval; pressing Cross
produced `intro: skipped to 62 s` in the PS5 trace and removed the prompt.
The earlier chapter fallback check is in `PROGRESS.md`.

## 2026-10-02 controller and coverage checks

The PS5 client was checked with South Park S1E1 (139.056–172.464 seconds)
and The Office S2E3, “Office Olympics” (48.669–80.351 seconds). Both accepted
the skip and resumed beyond the intro. Cross and the controls-visible Square
path were exercised. The card was moved clear of the playback module row and
is drawn after its scrim, so the controls cannot obscure it. A rejected seek
keeps the prompt available and shows a retry message.

Read-only API sampling found Intro markers in 11 of the first 12 South Park
episodes and all of the first 12 The Office episodes. South Park S1E9 had no
stored Intro marker in that sample. These are sample results, not full-library
coverage or independent verification of every detected timestamp.
