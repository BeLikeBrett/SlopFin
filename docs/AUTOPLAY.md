# Autoplay, the next-episode card, and "Are you still watching?"

*(Opus 5, 2026-09-16, at Brett's request: "building true auto play … when an
episode ends show offer a 'next episode' thing … Also need a 'are you still
watching' mechanic for when you go AFK and fall asleep".)*

## What happens

| When | What is on screen |
| --- | --- |
| Closing credits begin | The card slides up at the bottom right: the next episode's still, its number and name, **Cross** plays it now, **Circle** sends it away for this episode. Jellyfin Outro media segments win; a named Credits/Outro chapter is next; unmarked episodes fall back to 90 seconds from the end. |
| 5 seconds left, or the stream ends | The bar under the title fills as it counts, the heading reads "Next episode in 4" |
| The count reaches zero | The next episode starts |
| Three episodes with nothing pressed | Playback pauses and asks whether anyone is watching; **Cross** carries on, **Circle** stops |
| A minute with no answer | Playback stops and the title's page comes back, so an empty room does not hold a transcode open |
| The title finishes | Nothing goes black: the next episode's artwork fills the screen with **Play next episode**, **Watch again** and **Leave**, and a film ends on its own artwork with the last two. It waits there until something is pressed |

Pressing anything during an episode starts the count of untouched episodes
again -- somebody is clearly there.

## Skip Intro

Episodes can also carry a real intro interval. SlopFin resolves it in this
order:

1. Jellyfin MediaSegments with Type: Intro -- its StartTicks and EndTicks are
   authoritative.
2. A named chapter such as **Intro**, **Opening**, **Opening Credits**,
   **Opening Theme**, or **Title Sequence**. The following chapter is the end,
   and the interval is accepted only when that boundary is plausible.
3. No marker means **no button**. SlopFin deliberately does not guess a fixed
   number of seconds for intros.

While playback is inside that interval a compact **Skip Intro** control appears
at the bottom right after the normal OSD retires. **Cross** seeks to the exact
end marker; **Circle** dismisses it for that episode. If the timeline is up,
Cross/Circle belong to the timeline first so hiding controls cannot accidentally
skip or dismiss anything.

The chapter fallback was proved on-console with X-Men '97: an Intro chapter at
0:54.542 ending at Scene 2 at 2:04.000. Starting inside the interval and pressing
Cross landed at 2:04, with the HDMI E-AC-3 output preserved across the seek.

## The choices

Settings -> Playback:

- **Play the next episode automatically** -- On or Off. This is the default for
  series that have not been overridden. While an episode is playing, the
  skip-forward module in the player toggles autoplay for that **series**. That
  per-series choice persists across its later episodes and app restarts without
  changing any other show. With autoplay off the card still offers the next
  episode, but nothing counts down and nothing starts by itself.
- **Ask if you are still watching** -- Never, or after 2, 3, 4 or 5 episodes
  played one after another with nothing pressed. Three is the default, which is
  what the streaming services settled on.
- **Ask when nothing has been pressed** -- Never, 30, 60, 90 minutes, 2 or 3
  hours, for the other way people fall asleep: one long thing left running.
  Ninety minutes is the default. The question itself always gives up after a
  minute and stops playback.

The defaults are stored in `/data/slopfin/config.json` under `autoplay`; show
specific enable/disable overrides are stored under `autoplaySeries` keyed by
Jellyfin series id.

## Where it lives

`src/autoplay.hpp` holds the decisions and nothing else: it is a small state
machine taking the seconds left, whether the stream has ended, whether a button
was pressed, and the viewer's choices, and returning what to do. That is what
makes `tests/test_autoplay.cpp` able to run a whole evening of viewing through
it in a millisecond -- including the run of three episodes, the press that
resets it, the unanswered question, and a film, which has no next episode and
so is offered nothing.

`src/app.cpp` draws the card, Skip Intro and the still-watching question and
drives time-sensitive overlays from **the drawing, not the input** -- a prompt
that takes the input for itself must not stop the credits running out.
`jellyfin.cpp` resolves both ends of an Intro and the start of an Outro when an
episode's full record is fetched. Media Segments win; named chapters fill gaps.

The Up Next surface is a bottom-right dark-glass media card rather than a
generic dialog: widescreen still, **UP NEXT** hierarchy, season/episode line,
embedded Play now / Not now actions and a thin countdown edge. Autoplay sits at
the far right of the player module row because it is a per-series preference,
not a stream-format control.

## Traps found while building it

- **A reused frame hides everything drawn over it.** Playback skips redrawing
  when the picture has not changed; the card and the question both had to be
  added to that test or they never appeared at all.
- **The crash-report offer must not open over a film.** It did, and it swallowed
  the player's input while it was up. It now waits until browsing.
- **`play:` from the tooling was not the same as pressing Play**: it never
  loaded the neighbouring episodes, so nothing that depends on them could be
  tested remotely. It now does what the detail screen does.

## Trying it without waiting

- `tools/press.sh "play:<episode id>:<seconds>"` with the start a few seconds
  before the end brings the card up in the ordinary way; the trace records
  `autoplay: card up, next yes, remaining 20, ended no`.
- Writing `/data/slopfin-ask-test` brings up "Are you still watching?" at once.

## The end of a title releases it (Opus 5, 2026-09-16)

A stream that ran out used to be left open: the decoder, its half a gigabyte of
direct memory, the socket and the audio port all stayed as they were for as
long as the end card sat there. Starting anything else then built a second
decoder beside the first, and the console killed the process outright -- no
signal, no report beyond the session file, which is exactly the "crashes to the
home screen" Brett described.

`app.cpp` now latches `View::finished` the moment a title runs out, calls
`player::stop()` there, and draws the end card from the latch rather than from
the player's state. Four end-to-end cycles (a film ending, then an episode
ending into its autoplay, twice over) run without a restart and with memory
flat -- `free 1359 MiB, images 300 MiB` at every sample.

The crash handler also runs on its own stack now (`sigaltstack`): a fault on a
thread that has run out of stack used to take the handler down with it and
leave nothing written.
