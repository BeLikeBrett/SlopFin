/*
 * SlopFin - playback progress reporting.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tells the server what is playing and where, which is what keeps Continue
 * Watching and resume positions correct across devices. Requests go out on a
 * thread of their own: a slow server must never be able to stall the picture.
 */

#ifndef SLOPFIN_REPORTER_HPP
#define SLOPFIN_REPORTER_HPP

#include <string>

namespace slopfin::reporter
{

struct Session
{
    std::string item_id;
    std::string media_source_id;
    std::string play_session;
    std::string play_method; /* DirectPlay, DirectStream or Transcode */
    int audio_index = -1;
    int subtitle_index = -1;
};

/* A play session began at `position` seconds. */
void begin(const Session &session, double position) noexcept;

/* The latest position and pause state; sent every ten seconds, and at once
   when the pause state changes. Cheap enough to call every frame. */
void update(double position, bool paused) noexcept;

/* Queue the final position immediately. Never waits on the network. */
void end(double position) noexcept;

/* Wait for events queued before this call, including an in-flight request.
 * Call only on a background worker or during shutdown, never on the render
 * thread while navigating back. False preserves local progress until retry. */
bool flush(unsigned timeout_ms) noexcept;

/* Call before changing/signing out of a Jellyfin identity. Pending requests
 * and retries are discarded; a request already in flight retains its captured
 * original credentials and can never acquire the next account's token. */
void invalidate() noexcept;

} // namespace slopfin::reporter

#endif
