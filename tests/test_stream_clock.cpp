/*
 * SlopFin - host tests for the stream-to-film clock.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Subtitles showed 1.4 s early on every title. The expected values here are
 * not derived from the code under test: each pair is a picture's time in the
 * source file (ffprobe on the source server) and the timestamp the same picture
 * carried in Jellyfin's stream.
 */

#include "stream_clock.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
int g_failures = 0;

void near(double got, double want, const std::string &what)
{
    if (std::fabs(got - want) <= 0.0005)
        return;
    std::printf("FAIL: %s: got %.4f, want %.4f\n", what.c_str(), got, want);
    ++g_failures;
}
} // namespace

int main()
{
    using slopfin::player::film_seconds;

    /* Copied timestamps through the server's muxer: the measured pairs. */
    near(film_seconds(168.233, 168.686, true, true), 166.833, "FROM S04E03, hevc copy");
    near(film_seconds(599.233, 600.0, true, true), 597.833, "X-Men '97 S02E08, hevc copy");
    near(film_seconds(595.451, 600.0, true, true), 594.051, "Silo S01E06, h264 copy");
    near(film_seconds(601.416, 600.0, true, true), 600.016, "The Wire S01E13, re-encoded");

    /* A file played directly was never muxed by the server: no shift. */
    near(film_seconds(42.0, 0.0, true, false), 42.0, "direct play is left alone");

    /* Relative timestamps still add the requested start, and lose the shift. */
    near(film_seconds(1.4, 300.0, false, true), 300.0, "relative stream at its first picture");

    /* The opening of a film never goes negative. */
    near(film_seconds(0.5, 0.0, true, true), 0.0, "clamped at zero");

    if (g_failures == 0)
        std::printf("stream clock: all checks passed\n");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
