/*
 * SlopFin - interface motion.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Everything that moves on screen moves through here, so the whole interface
 * shares one sense of weight instead of each screen inventing a rate.
 *
 * What was here before was a linear approach -- move a fixed fraction of the
 * remaining distance each frame, and snap once inside a third of a pixel. That
 * is exponential decay: it leaves instantly at full speed and then crawls,
 * which is the opposite shape of anything physical and is why the interface
 * read as flat however the rate was tuned. A spring leaves from rest,
 * accelerates, and arrives having already slowed down.
 *
 * This is pure arithmetic on purpose. tests/test_motion.cpp runs it in
 * milliseconds; a console round trip to look at an animation takes minutes and
 * cannot measure overshoot at all.
 */

#ifndef SLOPFIN_MOTION_HPP
#define SLOPFIN_MOTION_HPP

#include <algorithm>
#include <cmath>

namespace slopfin::motion
{

/*
 * Tuning, named rather than numbered so a call site says what it wants. The
 * damping ratio is what decides the character: 1.0 arrives without ever
 * passing the target, below 1.0 overshoots and comes back.
 */
struct Feel
{
    float stiffness = 170.0f;
    float ratio = 1.0f;
};

/* Scrolling: no overshoot ever. A list that bounces past where it was asked
   to go reads as a mistake rather than as life. */
inline constexpr Feel kScroll{150.0f, 1.0f};
/* Sliding a row of cards sideways: the same, a little quicker. */
inline constexpr Feel kSlide{200.0f, 1.0f};
/* A card taking the focus: a small overshoot, so it arrives with a tick of
   weight rather than easing to a stop. */
inline constexpr Feel kFocus{260.0f, 0.72f};
/* Something appearing or fading: quick and dead flat. */
inline constexpr Feel kFade{320.0f, 1.0f};
/* A selection travelling between two things it can sit on -- a key, a tab.
   Quick, and it arrives with a little more than it needs, which is what makes
   moving along a row of keys feel answered rather than merely tracked. */
inline constexpr Feel kSelect{340.0f, 0.70f};
/* Text controls glide without overshooting adjacent labels. */
inline constexpr Feel kControlFocus{700.0f, 1.0f};

/* A scalar spring that settles completely, allowing idle player frames to
 * skip redraws instead of animating subpixel residuals indefinitely. */
class Spring
{
  public:
    Spring() = default;
    explicit Spring(float start) noexcept : value_(start), target_(start)
    {
    }

    [[nodiscard]] float value() const noexcept
    {
        return value_;
    }
    [[nodiscard]] float target() const noexcept
    {
        return target_;
    }
    [[nodiscard]] float velocity() const noexcept
    {
        return velocity_;
    }
    [[nodiscard]] bool settled() const noexcept
    {
        return settled_;
    }

    /* Sets where it is going without disturbing where it is. */
    void aim(float target) noexcept
    {
        if (target != target_)
        {
            target_ = target;
            settled_ = false;
        }
    }

    /* Puts it there at once: opening a screen, or a list being replaced. */
    void jump(float to) noexcept
    {
        value_ = to;
        target_ = to;
        velocity_ = 0.0f;
        settled_ = true;
    }

    /*
     * One frame. `dt` is clamped because a spring integrated over a long step
     * is not slow, it is unstable: a dropped frame would fling the value past
     * the target and oscillate. Sub-stepping keeps the same arithmetic honest
     * at any frame rate the console happens to deliver.
     */
    void step(const Feel &feel, float dt) noexcept
    {
        if (settled_)
            return;
        dt = std::clamp(dt, 0.0f, 0.1f);
        const float damping = 2.0f * feel.ratio * std::sqrt(feel.stiffness);

        int steps = 1;
        float slice = dt;
        while (slice > 1.0f / 120.0f && steps < 16)
        {
            slice *= 0.5f;
            steps *= 2;
        }
        for (int i = 0; i < steps; ++i)
        {
            const float force = (target_ - value_) * feel.stiffness - velocity_ * damping;
            velocity_ += force * slice;
            value_ += velocity_ * slice;
        }

        /* Close enough, and slow enough, that another frame of it would not be
           visible. Snapping here is what lets an unchanged screen stay
           unchanged. */
        if (std::fabs(target_ - value_) < epsilon_ && std::fabs(velocity_) < epsilon_ * 60.0f)
        {
            value_ = target_;
            velocity_ = 0.0f;
            settled_ = true;
        }
    }

    /* How near counts as arrived. A scroll measured in pixels and a fade
       measured 0 to 1 do not agree about that. */
    void precision(float epsilon) noexcept
    {
        epsilon_ = epsilon;
    }

  private:
    float value_ = 0.0f;
    float target_ = 0.0f;
    float velocity_ = 0.0f;
    float epsilon_ = 0.05f;
    bool settled_ = true;
};

/* Curves, for the things driven by a countdown rather than by a target. */
inline float ease_out_cubic(float t) noexcept
{
    t = std::clamp(t, 0.0f, 1.0f);
    const float inverse = 1.0f - t;
    return 1.0f - inverse * inverse * inverse;
}

inline float ease_in_out_cubic(float t) noexcept
{
    t = std::clamp(t, 0.0f, 1.0f);
    if (t < 0.5f)
        return 4.0f * t * t * t;
    const float inverse = -2.0f * t + 2.0f;
    return 1.0f - inverse * inverse * inverse / 2.0f;
}

/* Straight-line blend, for a colour or an alpha that has no business
   overshooting. */
inline float mix(float from, float to, float t) noexcept
{
    return from + (to - from) * std::clamp(t, 0.0f, 1.0f);
}

/* 0 at `from`, 1 at `to`, eased between. Used for anything positional that is
   driven by a frame count. */
inline float ramp(float value, float from, float to) noexcept
{
    if (to <= from)
        return value >= to ? 1.0f : 0.0f;
    return ease_out_cubic((value - from) / (to - from));
}

} // namespace slopfin::motion

#endif
