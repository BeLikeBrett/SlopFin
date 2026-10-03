/*
 * SlopFin - how long each picture is held on screen.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A picture rate and a display rate rarely divide. 23.976 pictures a second on
 * a 59.94 Hz output is two and a half display intervals each, which can only be
 * shown as an alternating three and two; 29.97 is exactly two. This helper
 * models cadence at the current display rate; it does not establish which
 * refresh-rate changes the platform permits.
 *
 * The rule used to be: every display interval, take the newest picture whose
 * timestamp has passed. That yields the right pattern only while nothing
 * jitters, and turns each late arrival into a picture held too long or dropped.
 * Measured on a 23.976 title it gave 69 two-interval holds against 126 threes,
 * and 22.65 pictures a second against a source of 23.976.
 *
 * This experimental helper free-runs against the display and uses the audio
 * clock only for trimming. It is currently not used by player.cpp: the console
 * experiment stalled while the producer under-delivered. Keep its arithmetic
 * tests separate from claims about the live player's behavior.
 */

#ifndef SLOPFIN_CADENCE_HPP
#define SLOPFIN_CADENCE_HPP

#include <cstddef>

namespace slopfin::player
{
class Cadence
{
  public:
    /* Both rates in Hz. Either unknown gives one picture per interval, which
       is the old behaviour and the right answer for a stream we know nothing
       about. */
    void configure(double source_fps, double display_hz) noexcept
    {
        source_fps_ = source_fps > 0.0 ? source_fps : 0.0;
        display_hz_ = display_hz > 0.0 ? display_hz : 0.0;
        step_ = source_fps_ > 0.0 && display_hz_ > 0.0 ? source_fps_ / display_hz_ : 1.0;
        /* A source faster than the display cannot be shown whole; one picture
           per interval drops the fewest and keeps the rest evenly spaced. */
        if (step_ > 1.0)
            step_ = 1.0;
    }

    void reset() noexcept
    {
        phase_ = 0.0;
    }

    /* The phase is started with the first picture so the pattern is in step
       from the very first one rather than settling into it. */
    void begin() noexcept
    {
        phase_ = 0.0;
    }

    [[nodiscard]] double phase() const noexcept
    {
        return phase_;
    }
    [[nodiscard]] double step() const noexcept
    {
        return step_;
    }

    /* Half a picture's worth of time: the point at which drift is worth
       correcting by trimming the phase. */
    [[nodiscard]] double half_frame() const noexcept
    {
        return source_fps_ > 0.0 ? 0.5 / source_fps_ : 0.020;
    }

    /*
     * Past this the picture and the sound are not drifting apart, they are
     * apart, and no amount of trimming will close it in a reasonable time. The
     * caller jumps to whichever picture the sound is actually at and starts the
     * phase again. A seek, a stall and the first seconds of playback all land
     * here; steady playback never should.
     */
    [[nodiscard]] static bool needs_resync(double error) noexcept
    {
        constexpr double kApart = 0.150;
        return error > kApart || error < -kApart;
    }

    /*
     * One step of the display.
     *
     * `intervals` is how many display intervals have passed, which is not
     * always one: a missed vertical blank is exactly when this needs to know.
     * `error` is how far the picture on screen sits ahead of the sound, in
     * seconds. `next_ready` says whether the following picture has arrived.
     *
     * Returns true when the next picture should be taken.
     */
    bool advance(double intervals, double error, bool next_ready) noexcept
    {
        if (!(intervals >= 1.0))
            intervals = 1.0;
        phase_ += step_ * intervals;

        /*
         * Drift is corrected by nudging the phase rather than yanking the
         * picture, so the error comes out over several pictures and the
         * three-two pattern survives. The nudge is deliberately small: a
         * quarter-step correction shifts the rate by 25 percent, which turns
         * 23.976 into four-interval holds -- the very judder this exists to
         * remove. Five percent takes about half a second to absorb half a
         * frame and never leaves the two-and-three pattern.
         */
        constexpr double kTrim = 0.05;
        const double tolerance = half_frame();
        if (error > tolerance)
            phase_ -= step_ * kTrim;
        else if (error < -tolerance)
            phase_ += step_ * kTrim;

        if (phase_ >= 1.0 && next_ready)
        {
            phase_ -= 1.0;
            /* Never bank more than one picture of credit, or a stall would be
               followed by a burst that looks worse than the stall. */
            if (phase_ >= 1.0)
                phase_ = 0.0;
            return true;
        }
        if (phase_ > 1.0)
            phase_ = 1.0;
        return false;
    }

  private:
    double source_fps_ = 0.0;
    double display_hz_ = 0.0;
    double step_ = 1.0;
    double phase_ = 0.0;
};
} // namespace slopfin::player

#endif
