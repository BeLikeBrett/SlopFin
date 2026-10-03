# Autoplay and idle prompts

**Settings → Playback** sets the default for playing the next episode.
During playback, the Autoplay control changes that preference for the current
series. SlopFin remembers each series separately across app restarts.

## Up Next

An Up Next card appears over the closing credits, showing the next episode.
With playback controls hidden, Cross starts it immediately and Circle dismisses
the card for this episode.

With autoplay on, the card counts down near the end and starts the next episode.
With autoplay off, it still offers the next episode but waits for you to choose.
The ordinary episode controls also let you move to the previous or next episode.

Jellyfin Outro segments determine where credits begin when available. Named
Credits/Outro chapters are the fallback; an unmarked episode uses 90 seconds
before the end. [Intro skipping](INTRO_SKIPPING.md) uses separate Intro metadata.

## “Are you still watching?”

Settings offers two independent checks:

- After **2–5 episodes** played without input, or Never. The default is three.
- After **30, 60 or 90 minutes, two or three hours** without input, or Never.
  The default is 90 minutes.

Cross continues and Circle stops. If nobody answers within a minute, playback
stops. Controller input resets the inactivity tracking.

After playback finishes, the end screen offers the next episode when available,
Watch again, and Leave. The player releases its stream and decoder while this
screen waits.
