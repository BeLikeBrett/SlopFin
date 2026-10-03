/*
 * SlopFin - what happens when an episode runs out.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Time-based next-episode and continued-watching decisions, independent of
 * rendering. Preview the next episode during credits; start its countdown
 * near EOF and ask for confirmation after prolonged unattended playback.
 */

#ifndef SLOPFIN_AUTOPLAY_HPP
#define SLOPFIN_AUTOPLAY_HPP

#include <algorithm>
#include <cstdint>

namespace slopfin::autoplay
{

/* What the viewer chose in Settings. */
struct Choices
{
    bool enabled = true;
    /* Consecutive episodes played without a button pressed before the app
       asks whether anyone is watching. 0 means never ask. */
    int ask_after = 3;
    /*
     * And the other way anyone falls asleep: a long film, or one episode left
     * running, with nothing pressed for this many minutes. 0 means never.
     * Ninety minutes is what the streaming services settled on.
     */
    int ask_idle_minutes = 90;
};

/* When no real credits/outro marker exists, this is the conservative
   fallback. It used to be 22 seconds, which made the offer appear after most
   TV credits were already over. Real markers always win. */
inline constexpr double kCardLead = 90.0;
inline constexpr double kCountdown = 5.0;

/* What the player knows this frame. */
struct Frame
{
    bool playing_episode = false; /* an episode, with another after it */
    bool has_next = false;
    bool ended = false;           /* the stream reached its end */
    double remaining = 1e9;       /* seconds left, when the length is known */
    bool credits_known = false;   /* the title carries an outro/credits marker */
    bool credits_started = false; /* playback has crossed that marker */
    bool user_pressed = false;    /* any button this frame */
};

enum class Action : std::uint8_t
{
    none,
    play_next,       /* start the next episode now */
    ask_still_there, /* stop and ask before going on */
};

class State
{
  public:
    void reset() noexcept
    {
        card_ = false;
        dismissed_ = false;
        counting_ = false;
        left_ = kCountdown;
        asking_ = false;
        ask_left_ = 0.0;
        idle_ = 0.0;
    }

    /* A new title started because someone chose it, not because of us. */
    void chosen_by_hand() noexcept
    {
        reset();
        streak_ = 0;
    }

    /* The next episode started on its own. */
    void played_automatically() noexcept
    {
        reset();
        ++streak_;
    }

    /* Stopped and asked: the answer decides whether anything else plays. */
    void asked() noexcept
    {
        asking_ = true;
        ask_left_ = kAskTimeout;
    }

    [[nodiscard]] bool card_visible() const noexcept
    {
        return card_;
    }
    [[nodiscard]] bool counting() const noexcept
    {
        return counting_;
    }
    /* Seconds still to run, for the ring; 0 when it is not counting. */
    [[nodiscard]] double seconds_left() const noexcept
    {
        return counting_ ? std::max(0.0, left_) : 0.0;
    }
    [[nodiscard]] bool asking() const noexcept
    {
        return asking_;
    }
    [[nodiscard]] double ask_seconds_left() const noexcept
    {
        return std::max(0.0, ask_left_);
    }
    [[nodiscard]] int streak() const noexcept
    {
        return streak_;
    }
    [[nodiscard]] bool dismissed() const noexcept
    {
        return dismissed_;
    }

    /* The viewer put the card away: nothing starts by itself this episode. */
    void dismiss() noexcept
    {
        dismissed_ = true;
        card_ = false;
        counting_ = false;
    }

    /* "Yes, still here": carry on and start counting again from zero. */
    void still_there() noexcept
    {
        asking_ = false;
        streak_ = 0;
        idle_ = 0.0;
    }

    /* Minutes with nothing pressed, for the question's own wording. */
    [[nodiscard]] double idle_seconds() const noexcept
    {
        return idle_;
    }

    /*
     * One frame. `dt` is seconds. Returns what the player should do, and
     * leaves behind what the screen should show.
     */
    Action step(double dt, const Frame &frame, const Choices &choices) noexcept
    {
        if (asking_)
        {
            /* The question waits a minute for an answer, then gives up: an
               empty room should not hold a transcode open all night. */
            ask_left_ -= dt;
            if (frame.user_pressed)
                ask_left_ = kAskTimeout;
            return ask_left_ <= 0.0 ? Action::ask_still_there : Action::none;
        }
        /* A button pressed means somebody is there: the run starts over, and
           the question is that much further away. */
        if (frame.user_pressed)
        {
            streak_ = 0;
            idle_ = 0.0;
        }
        else if (frame.playing_episode)
            idle_ += dt;

        /* Nothing pressed for a long time, whatever is playing. */
        if (choices.ask_idle_minutes > 0 && frame.playing_episode && !frame.ended &&
            idle_ >= choices.ask_idle_minutes * 60.0)
        {
            asking_ = true;
            ask_left_ = kAskTimeout;
            card_ = false;
            counting_ = false;
            return Action::none;
        }
        if (!frame.playing_episode || !frame.has_next)
        {
            card_ = false;
            counting_ = false;
            return Action::none;
        }
        if (dismissed_)
            return Action::none;

        /* An explicit credits marker is authoritative. If one exists, do not
           second-guess it with a generic time-from-end threshold. */
        const bool near_end = frame.ended || frame.credits_started ||
                              (!frame.credits_known && frame.remaining <= kCardLead);
        card_ = near_end;
        const bool count_now = choices.enabled && (frame.ended || frame.remaining <= kCountdown);
        /* Turning autoplay off from the player must stop an in-flight
           countdown without hiding the manual Next Episode offer. */
        if (!choices.enabled)
            counting_ = false;
        if (count_now && !counting_)
        {
            counting_ = true;
            /* At the very end the whole countdown runs; walking into it from
               the credits, only what is actually left of the episode. */
            left_ = frame.ended ? kCountdown : std::min(kCountdown, std::max(0.0, frame.remaining));
        }
        if (!counting_)
            return Action::none;
        left_ -= dt;
        if (left_ > 0.0)
            return Action::none;

        counting_ = false;
        card_ = false;
        /* Time to go on -- unless this would be one episode too many with
           nobody pressing anything. */
        if (choices.ask_after > 0 && streak_ + 1 >= choices.ask_after)
        {
            asking_ = true;
            ask_left_ = kAskTimeout;
            return Action::none;
        }
        return Action::play_next;
    }

    /* The card is up, and the viewer asked for it now. */
    [[nodiscard]] bool can_play_now() const noexcept
    {
        return card_ && !dismissed_;
    }

    static constexpr double kAskTimeout = 60.0;

  private:
    bool card_ = false;
    bool dismissed_ = false;
    bool counting_ = false;
    double left_ = kCountdown;
    bool asking_ = false;
    double ask_left_ = 0.0;
    /* Episodes played without anyone pressing anything. */
    int streak_ = 0;
    /* Seconds since anything was pressed. */
    double idle_ = 0.0;
};

} // namespace slopfin::autoplay

#endif
