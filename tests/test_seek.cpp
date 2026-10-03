/*
 * SlopFin - host tests for the seek curve.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Two complaints drove this curve and neither is visible in a screenshot: a
 * tap moved a different distance every time, and letting go on +/-0 to call a
 * skip off was a coin toss. Both are arithmetic, so both are checked here
 * rather than on a console with a stopwatch.
 */

#include "seek.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

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

constexpr double kFrame = 1.0 / 60.0;
/* app.cpp: a skip smaller than this counts as not having moved. */
constexpr double kCancelWindow = 0.5;
/* app.cpp: the continuous scrub waits this long so a tap stays a tap. */
constexpr int kHoldBeforeScrub = 20;

/* Holds full press from `from`, and reports where it got to after `seconds`. */
double hold(double from, double seconds, double pressure = 1.0, double direction = 1.0)
{
    double pending = from;
    int held = 0;
    const int frames = static_cast<int>(seconds * 60.0);
    for (int f = 0; f < frames; ++f)
    {
        ++held;
        if (held <= kHoldBeforeScrub)
            continue;
        pending += direction * slopfin::seek::seek_rate(pressure, held, pending) * kFrame;
    }
    return pending;
}
} // namespace

int main()
{
    using namespace slopfin::seek;

    /* A tap is one exact second, and small enough that a tap the wrong way can
       be taken back by a tap the right way without leaving the cancel window
       unreachable. */
    check(kTriggerTap == 1.0, "a trigger tap is one second");
    check(kTriggerTap > kCancelWindow, "a tap moves further than the cancel window");
    check(kSeekTap > kTriggerTap, "a direction is the coarser of the two");
    check(kShoulderStep > kSeekTap, "a shoulder is the coarsest of the three");
    check(kShoulderStep == 120.0, "a shoulder steps two minutes");

    /* At the origin the rate is the crawl, no matter how long the hold has
       been building. This is the whole point of the curve: the ramp must not
       reach the cancel point. */
    for (const int held : {1, 60, 210, 600})
        check(std::fabs(seek_rate(1.0, held, 0.0) - kSeekFineRate) < 1e-9,
              "at the origin the rate is the crawl, held " + std::to_string(held));

    /* Nothing about it may go backwards: further out is never slower, and
       pressing harder is never slower. */
    for (const int held : {1, 60, 210})
    {
        double previous = -1.0;
        for (int step = 0; step <= 160; ++step)
        {
            const double pending = step * 0.25;
            const double rate = seek_rate(1.0, held, pending);
            check(rate >= previous - 1e-9,
                  "rate never falls as the skip grows, held " + std::to_string(held));
            previous = rate;
        }
        previous = -1.0;
        for (int step = 0; step <= 20; ++step)
        {
            const double travel = step * 0.05;
            const double rate = seek_rate(travel, held, 30.0);
            check(rate >= previous - 1e-9, "rate never falls as the trigger goes down");
            previous = rate;
        }
    }

    /* It is symmetric: skipping back behaves as skipping forward does. */
    for (int step = 1; step < 60; ++step)
        check(std::fabs(seek_rate(1.0, 120, step * 0.5) - seek_rate(1.0, 120, -step * 0.5)) < 1e-9,
              "backwards and forwards travel at the same rate");

    /* Verify the cancellation window remains reachable after a long seek. */
    {
        double pending = -120.0;
        int held = 400; /* well past kRampFrames: the ramp is at its ceiling */
        int inside = 0;
        for (int f = 0; f < 6000 && pending < 2.0; ++f)
        {
            ++held;
            pending += seek_rate(1.0, held, pending) * kFrame;
            if (std::fabs(pending) <= kCancelWindow)
                ++inside;
        }
        std::printf("seek: %d frames inside the cancel window (%.2f s)\n", inside, inside / 60.0);
        check(inside >= 20, "at least a third of a second to release on the cancel point");
    }

    /* The crawl must not turn a long skip into a chore: five minutes is still
       reached in about two seconds of holding. */
    {
        const double reached = hold(kTriggerTap, 3.0);
        std::printf("seek: three seconds of holding reaches %.0f s\n", reached);
        check(reached >= 300.0, "three seconds of holding crosses five minutes");
    }

    /* A gentle press stays gentle: it must not cross a minute in a second. */
    check(hold(kTriggerTap, 1.0, 0.3) < 60.0, "a light press creeps");

    /* A held direction gets the same curve, so the two controls agree. */
    check(seek_rate(0.62, 60, 5.0) > 0.0, "a direction hold produces a rate");

    if (g_failures == 0)
        std::printf("seek: all checks passed\n");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
