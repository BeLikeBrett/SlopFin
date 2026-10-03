/*
 * SlopFin - host tests for interface motion.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * These assert the shape of the movement, not that two runs agree with each
 * other: that a scroll never passes the place it was sent to, that a focus
 * spring does pass it and comes back, that everything stops, and that a
 * dropped frame does not turn a spring into an oscillator. None of that can be
 * seen in a screenshot and all of it decides whether the interface feels
 * right.
 */

#include "motion.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
int g_failures = 0;

void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::printf("FAIL: %s\n", what);
    ++g_failures;
}

constexpr float kFrame = 1.0f / 60.0f;

/* Runs a spring to rest and reports the furthest it ever went. */
float run(slopfin::motion::Spring &spring, const slopfin::motion::Feel &feel, int max_frames,
          int &frames_taken)
{
    float furthest = spring.value();
    frames_taken = 0;
    for (int i = 0; i < max_frames && !spring.settled(); ++i)
    {
        spring.step(feel, kFrame);
        furthest = std::fabs(spring.value()) > std::fabs(furthest) ? spring.value() : furthest;
        ++frames_taken;
    }
    return furthest;
}
} // namespace

int main()
{
    using namespace slopfin::motion;

    /* A fresh spring is already where it belongs and costs nothing. */
    {
        Spring spring;
        check(spring.settled(), "a new spring starts settled");
        spring.step(kScroll, kFrame);
        check(spring.value() == 0.0f, "a settled spring does not drift");
    }

    /* Scrolling must never pass the target: a list that bounces past the row
       it was sent to looks like a fault. */
    {
        Spring spring;
        spring.precision(0.05f);
        spring.aim(420.0f);
        int frames = 0;
        const float furthest = run(spring, kScroll, 600, frames);
        check(furthest <= 420.0f + 0.001f, "kScroll never overshoots");
        check(spring.settled(), "kScroll settles");
        check(spring.value() == 420.0f, "kScroll lands exactly on the target");
        check(frames > 6 && frames < 90, "kScroll takes between 6 and 90 frames");
    }

    /* Focus is meant to overshoot, or it has no weight to it. */
    {
        Spring spring;
        spring.precision(0.002f);
        spring.aim(1.0f);
        int frames = 0;
        const float furthest = run(spring, kFocus, 600, frames);
        check(furthest > 1.0f, "kFocus overshoots");
        check(furthest < 1.20f, "kFocus overshoot stays under twenty percent");
        check(spring.settled() && spring.value() == 1.0f, "kFocus still settles exactly");
        check(frames < 60, "kFocus arrives within a second");
    }

    /* A fade has no business overshooting: a card cannot be more than opaque. */
    {
        Spring spring;
        spring.precision(0.002f);
        spring.aim(1.0f);
        int frames = 0;
        const float furthest = run(spring, kFade, 600, frames);
        check(furthest <= 1.0f + 0.001f, "kFade never passes full");
        check(frames < 40, "kFade is quick");
    }

    /*
     * A long frame must not make it unstable. Integrating 250 ms in one step
     * with these constants would fling the value far past the target and ring;
     * the sub-stepping is what stops that, and this is the only place it gets
     * checked.
     */
    {
        Spring spring;
        spring.precision(0.05f);
        spring.aim(1000.0f);
        for (int i = 0; i < 40; ++i)
            spring.step(kScroll, 0.25f);
        check(spring.value() <= 1000.0f + 0.001f, "a quarter-second step does not overshoot");
        check(spring.settled(), "a quarter-second step still settles");
    }

    /* Re-aiming mid-flight keeps the speed it already had rather than
       restarting, which is what makes a held direction feel continuous. */
    {
        Spring spring;
        spring.precision(0.05f);
        spring.aim(400.0f);
        for (int i = 0; i < 6; ++i)
            spring.step(kScroll, kFrame);
        const float moving = spring.velocity();
        check(moving > 0.0f, "the spring is moving before it is re-aimed");
        spring.aim(800.0f);
        check(spring.velocity() == moving, "re-aiming keeps the velocity");
        check(!spring.settled(), "re-aiming wakes it up");
    }

    /* jump is for a screen being replaced: there instantly, and quiet. */
    {
        Spring spring;
        spring.aim(500.0f);
        spring.step(kScroll, kFrame);
        spring.jump(0.0f);
        check(spring.value() == 0.0f && spring.velocity() == 0.0f, "jump lands and stops");
        check(spring.settled(), "jump settles");
    }

    /* Text focus arrives promptly without bouncing into the next row. */
    {
        Spring spring(0.0f);
        spring.aim(84.0f);
        float previous = 0.0f;
        for (int i = 0; i < 60; ++i)
        {
            spring.step(kControlFocus, kFrame);
            check(spring.value() >= previous && spring.value() <= 84.001f,
                  "control focus is monotonic and bounded");
            previous = spring.value();
            if (i == 11)
                check(spring.value() > 79.0f, "control focus reaches the row in about 200 ms");
        }
        check(spring.settled(), "control focus stops redrawing when settled");
        spring.aim(0.0f);
        for (int i = 0; i < 60; ++i)
            spring.step(kControlFocus, i == 2 ? 0.09f : kFrame);
        check(spring.settled() && spring.value() == 0.0f, "reverse focus survives a dropped frame");
    }

    /* The curves are curves: monotonic, and pinned at both ends. */
    {
        check(ease_out_cubic(0.0f) == 0.0f, "ease_out_cubic starts at zero");
        check(ease_out_cubic(1.0f) == 1.0f, "ease_out_cubic ends at one");
        check(ease_out_cubic(0.5f) > 0.5f, "ease_out_cubic front-loads");
        check(ease_in_out_cubic(0.5f) > 0.49f && ease_in_out_cubic(0.5f) < 0.51f,
              "ease_in_out_cubic is symmetric about the middle");
        float previous = -1.0f;
        for (int i = 0; i <= 20; ++i)
        {
            const float t = static_cast<float>(i) / 20.0f;
            const float value = ease_in_out_cubic(t);
            check(value >= previous, "ease_in_out_cubic never goes backwards");
            previous = value;
        }
        check(ease_out_cubic(-3.0f) == 0.0f && ease_out_cubic(9.0f) == 1.0f,
              "the curves clamp outside 0..1");
        check(ramp(5.0f, 0.0f, 10.0f) > 0.0f && ramp(0.0f, 0.0f, 10.0f) == 0.0f,
              "ramp maps a range onto the curve");
        check(mix(10.0f, 20.0f, 0.5f) == 15.0f, "mix is a straight line");
    }

    if (g_failures == 0)
        std::printf("motion: all checks passed\n");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
