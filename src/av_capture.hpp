/*
 * SlopFin - display and sound capability capture.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Settings > Diagnostics > Check display & sound. Asks the console, read-only,
 * what the connected TV, monitor or receiver says it can take -- audio formats
 * and channels, video modes, HDR -- and what the PS5's own sound settings are,
 * and writes it all to one numbered report in /data. Nothing here changes an
 * output mode or opens an audio port.
 *
 * The reports are raw on purpose: they are the evidence the capability parser
 * is built and tested against, one per display.
 */

#ifndef SLOPFIN_AV_CAPTURE_HPP
#define SLOPFIN_AV_CAPTURE_HPP

#include <string>

namespace slopfin::av_capture
{

/* Runs the capture; takes about two seconds (it measures the refresh rate).
   Render thread, never during playback. Returns what to show the viewer:
   where the report went, or why it could not be written. */
std::string run() noexcept;

} // namespace slopfin::av_capture

#endif
