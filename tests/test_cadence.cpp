/*
 * SlopFin - the display cadence.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The pattern of hold lengths is pure arithmetic, so it is checked here in
 * milliseconds rather than through a three-minute console round trip. Each
 * expectation is against what the rates themselves require, not against
 * another run of the same code: 59.94 over 23.976 is 2.5, so the only pattern
 * that shows every picture is an alternating three and two.
 */

#include "cadence.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAILED: %s\n", what);
    std::exit(1);
}

/* Runs a perfectly behaved pipeline: every picture ready when wanted, sound
   exactly in step, one display interval per step. */
std::vector<int> holds(double source_fps, double display_hz, int intervals)
{
    slopfin::player::Cadence cadence;
    cadence.configure(source_fps, display_hz);
    cadence.begin();

    std::vector<int> lengths;
    int held = 0;
    for (int i = 0; i < intervals; ++i)
    {
        ++held;
        if (cadence.advance(1.0, 0.0, true))
        {
            lengths.push_back(held);
            held = 0;
        }
    }
    return lengths;
}

void film_on_sixty()
{
    const std::vector<int> lengths = holds(23.976, 59.94, 600);
    check(!lengths.empty(), "23.976 on 59.94 shows pictures at all");

    int twos = 0;
    int threes = 0;
    for (const int length : lengths)
    {
        check(length == 2 || length == 3, "23.976 on 59.94 holds only two or three intervals");
        twos += length == 2 ? 1 : 0;
        threes += length == 3 ? 1 : 0;
    }
    /* 2.5 intervals a picture means the two lengths appear equally often. */
    const int difference = twos > threes ? twos - threes : threes - twos;
    check(difference <= 1, "23.976 on 59.94 alternates three and two evenly");

    /* And the pattern must actually alternate rather than merely balance. */
    for (std::size_t i = 1; i < lengths.size(); ++i)
        check(lengths[i] != lengths[i - 1], "23.976 on 59.94 never repeats a hold length");

    /* Every picture the source has, over the run. */
    const double seconds = 600.0 / 59.94;
    const double shown = static_cast<double>(lengths.size()) / seconds;
    check(shown > 23.9 && shown < 24.1, "23.976 on 59.94 shows 23.976 pictures a second");
}

void broadcast_on_sixty()
{
    const std::vector<int> lengths = holds(29.97, 59.94, 600);
    for (const int length : lengths)
        check(length == 2, "29.97 on 59.94 holds exactly two intervals");
    check(lengths.size() == 300, "29.97 on 59.94 shows half as many pictures as intervals");
}

void pal_on_sixty()
{
    /* 59.94 over 25 is 2.3976, so threes and twos in no even ratio. Neither
       length may be exceeded, and every picture must still be shown. */
    const std::vector<int> lengths = holds(25.0, 59.94, 1200);
    for (const int length : lengths)
        check(length == 2 || length == 3, "25 on 59.94 holds two or three intervals");
    const double seconds = 1200.0 / 59.94;
    const double shown = static_cast<double>(lengths.size()) / seconds;
    check(shown > 24.9 && shown < 25.1, "25 on 59.94 shows 25 pictures a second");
}

void high_rate_source()
{
    /* 59.94 over 50 is 1.2, so most pictures get one interval and every fifth
       gets two. Every picture is still shown. */
    const std::vector<int> lengths = holds(50.0, 59.94, 600);
    for (const int length : lengths)
        check(length == 1 || length == 2, "50 on 59.94 holds one or two intervals");
    const double seconds = 600.0 / 59.94;
    const double shown = static_cast<double>(lengths.size()) / seconds;
    check(shown > 49.5 && shown < 50.5, "50 on 59.94 shows 50 pictures a second");
}

void faster_than_the_display()
{
    /* A 120 fps source cannot be shown whole on a 59.94 Hz display. One
       picture per interval drops the fewest and keeps the rest evenly
       spaced. */
    const std::vector<int> lengths = holds(120.0, 59.94, 300);
    for (const int length : lengths)
        check(length == 1, "120 on 59.94 holds one interval");
    check(lengths.size() == 300, "120 on 59.94 shows one picture per interval");
}

void a_missed_vblank_is_not_lost()
{
    /* Two intervals in one step must count as two, or a dropped vertical blank
       would slow the picture down permanently. */
    slopfin::player::Cadence cadence;
    cadence.configure(23.976, 59.94);
    cadence.begin();

    int taken = 0;
    for (int i = 0; i < 100; ++i)
        taken += cadence.advance(2.0, 0.0, true) ? 1 : 0;
    /* 100 steps of two intervals is 200 intervals, which is 80 pictures. */
    check(taken >= 79 && taken <= 81, "a missed vertical blank still advances the phase");
}

void a_starved_decoder_does_not_burst()
{
    /* While no picture is ready the phase must not bank credit, or the stall
       would be followed by a burst that looks worse than the stall did. */
    slopfin::player::Cadence cadence;
    cadence.configure(23.976, 59.94);
    cadence.begin();

    for (int i = 0; i < 60; ++i)
        check(!cadence.advance(1.0, 0.0, false), "nothing is taken while nothing is ready");

    int taken = 0;
    for (int i = 0; i < 10; ++i)
        taken += cadence.advance(1.0, 0.0, true) ? 1 : 0;
    check(taken <= 5, "a starved decoder is not followed by a burst");
}

void drift_is_corrected_without_breaking_the_pattern()
{
    /* A picture running ahead of the sound must be slowed, but by nudging the
       phase: the hold lengths must stay two and three throughout. */
    slopfin::player::Cadence cadence;
    cadence.configure(23.976, 59.94);
    cadence.begin();

    int held = 0;
    int taken = 0;
    for (int i = 0; i < 600; ++i)
    {
        ++held;
        if (cadence.advance(1.0, 0.030, true))
        {
            check(held == 2 || held == 3, "correcting drift keeps two and three");
            held = 0;
            ++taken;
        }
    }
    /* Ahead of the sound, so fewer pictures than the undisturbed 240. */
    check(taken < 240, "a picture ahead of the sound is slowed down");
    check(taken > 200, "a picture ahead of the sound is not stopped");
}
} // namespace

int main()
{
    film_on_sixty();
    broadcast_on_sixty();
    pal_on_sixty();
    high_rate_source();
    faster_than_the_display();
    a_missed_vblank_is_not_lost();
    a_starved_decoder_does_not_burst();
    drift_is_corrected_without_breaking_the_pattern();
    std::puts("Display cadence tests passed");
    return 0;
}
