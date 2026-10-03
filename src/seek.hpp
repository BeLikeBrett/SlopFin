/*
 * SlopFin - the seek curve.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pure arithmetic, in its own unit so it can be checked without a console and
 * without a controller. The trigger seek reads an analogue axis, which the
 * scripted preview cannot drive and a screenshot cannot measure; the only
 * honest way to know this curve does what it claims is to run it.
 * tests/test_seek.cpp does, over the two cases that actually get complained
 * about: what one tap moves, and whether the cancel point can be released on.
 */

#ifndef SLOPFIN_SEEK_HPP
#define SLOPFIN_SEEK_HPP

#include <algorithm>

namespace slopfin::seek
{

/* Seek rate combines directional hold duration and analogue trigger travel. */
/* A single press of a direction, before any holding: small enough to land on
   a line of dialogue. */
inline constexpr double kSeekTap = 10.0;

/*
 * A single tap of a trigger. The directions are the coarse control and step by
 * ten; the triggers are the fine one and step by one, which is inside the half
 * second that counts as not having moved, so a tap either side of the cancel
 * point can be walked back with another tap.
 */
inline constexpr double kTriggerTap = 1.0;

/*
 * One press of a shoulder. The flat one of the three: the triggers aim, the
 * directions step, and this crosses a chunk of programme at a size that stays
 * the same however many times it is pressed, so four presses is eight minutes
 * without having to watch the bar.
 */
inline constexpr double kShoulderStep = 120.0;

/* Slow scrubbing near its origin to make cancellation precise. */
inline constexpr double kSeekSlowZone = 10.0;
inline constexpr double kSeekFineRate = 1.0;

inline double seek_rate(double travel, int held, double pending) noexcept
{
    constexpr double kCreep = 8.0;        /* a gentle press, at the start */
    constexpr double kFloor = 45.0;       /* full press, the moment it goes down */
    constexpr double kCeiling = 900.0;    /* full press, held */
    constexpr double kRampFrames = 210.0; /* about three and a half seconds */

    const double pressure = std::clamp(travel, 0.0, 1.0);
    const double weighted = pressure * pressure;
    const double ramp = std::clamp(static_cast<double>(held) / kRampFrames, 0.0, 1.0);
    /* Eased, so the build is slow to start and then gathers. */
    const double opened = kFloor + (kCeiling - kFloor) * ramp * ramp;
    const double rate = kCreep + (opened - kCreep) * weighted;

    /* Ease the rate toward a crawl near the origin in either direction. */
    const double distance = pending < 0.0 ? -pending : pending;
    const double nearness = std::clamp(distance / kSeekSlowZone, 0.0, 1.0);
    const double eased = nearness * nearness * nearness;
    return kSeekFineRate + (rate - kSeekFineRate) * eased;
}

} // namespace slopfin::seek

#endif
