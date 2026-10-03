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

/* The session ended at `position`; also flushes anything still queued. */
void end(double position) noexcept;

} // namespace slopfin::reporter

#endif
