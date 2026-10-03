/*
 * SlopFin - host tests for per-title subtitle timing.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The control exists because signed delay settings confuse people about which
 * way is which. So the direction is checked against what a viewer sees: after
 * "subtitles late", a cue must come up earlier in the film than before.
 */

#include "subtitle_timing.hpp"
#include "subtitles.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;

void check(bool condition, const std::string &what)
{
    if (condition)
        return;
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
}
} // namespace

int main()
{
    using namespace slopfin::subtitle_timing;
    const std::vector<slopfin::subtitles::Cue> cues{{10.0, 12.0, "Hello."}};

    /* "Subtitles late" must make the line appear sooner in the film. */
    {
        double offset = 0.0;
        for (int i = 0; i < 5; ++i)
            offset = nudge(offset, true);
        check(slopfin::subtitles::active(cues, cue_clock(9.6, offset)) == "Hello.",
              "after five 'late' presses the line is up 0.5 s before its time");
        check(slopfin::subtitles::active(cues, cue_clock(9.4, offset)).empty(),
              "but not a whole 0.6 s before it");
    }

    /* "Subtitles early" must hold the line back. */
    {
        const double offset = nudge(nudge(0.0, false), false);
        check(slopfin::subtitles::active(cues, cue_clock(10.1, offset)).empty(),
              "after two 'early' presses the line is not up at its own time");
        check(slopfin::subtitles::active(cues, cue_clock(10.2, offset)) == "Hello.",
              "and appears 0.2 s later");
    }

    /* A hundred presses one way and back lands exactly on original. */
    {
        double offset = 0.0;
        for (int i = 0; i < 100; ++i)
            offset = nudge(offset, true);
        for (int i = 0; i < 100; ++i)
            offset = nudge(offset, false);
        check(is_original(offset) && offset == 0.0, "no floating-point dust after 200 presses");
    }

    check(nudge(29.95, true) == kLimit, "sooner stops at the limit");
    check(nudge(-kLimit, false) == -kLimit, "later stops at the limit");

    check(describe(0.0) == "Original timing", "zero reads as original");
    check(describe(0.4) == "Showing 0.4 s sooner", "positive reads as sooner");
    check(describe(-1.2) == "Showing 1.2 s later", "negative reads as later");

    if (g_failures == 0)
        std::printf("subtitle timing: all checks passed\n");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
