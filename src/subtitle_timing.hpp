/*
 * SlopFin - per-title subtitle timing, in words rather than signs.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Most players offer "subtitle delay: +0.5 s" and leave the viewer to work out
 * whether plus means earlier or later. Here the choices name what the viewer
 * is seeing -- subtitles late, or subtitles early -- and the state is a
 * sentence. Underneath, one number: how many seconds sooner the subtitles
 * appear than the file says. Positive shows them sooner.
 *
 * Header-only so the rounding and the wording are tested on the host.
 */

#ifndef SLOPFIN_SUBTITLE_TIMING_HPP
#define SLOPFIN_SUBTITLE_TIMING_HPP

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace slopfin::subtitle_timing
{

inline constexpr double kStep = 0.1;   /* seconds per press, or per repeat while held */
inline constexpr double kLimit = 30.0; /* a file further out than this is the wrong file */

/* One press. `sooner` is the "subtitles late" choice. Kept on a tenth of a
   second so repeated presses never collect floating-point dust. */
inline double nudge(double offset, bool sooner) noexcept
{
    const double moved = offset + (sooner ? kStep : -kStep);
    const double tenths = std::round(moved / kStep) * kStep;
    return std::clamp(tenths, -kLimit, kLimit);
}

/* The clock a subtitle cue is looked up against. Showing a cue 0.4 s sooner
   means asking for the cue that belongs 0.4 s further on. */
inline double cue_clock(double film_seconds, double offset) noexcept
{
    return film_seconds + offset;
}

inline bool is_original(double offset) noexcept
{
    return std::fabs(offset) < kStep / 2.0;
}

/* "Original timing", "Showing 0.4 s sooner", "Showing 1.2 s later". */
inline std::string describe(double offset)
{
    if (is_original(offset))
        return "Original timing";
    char text[48];
    std::snprintf(text, sizeof(text), "Showing %.1f s %s", std::fabs(offset),
                  offset > 0.0 ? "sooner" : "later");
    return text;
}

} // namespace slopfin::subtitle_timing

#endif
