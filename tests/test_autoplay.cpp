/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * SlopFin - an evening of viewing, run through the autoplay rules.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Expected behaviour is the behaviour of the apps this copies: the card comes
 * up with the credits, the countdown is short, one press takes it early and
 * another sends it away, and after a few untouched episodes it asks whether
 * anyone is there. (Opus 5, 2026-09-16.)
 */

#include "autoplay.hpp"

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

using slopfin::autoplay::Action;
using slopfin::autoplay::Choices;
using slopfin::autoplay::Frame;
using slopfin::autoplay::State;

/* Plays an episode from `from` seconds left to its end, a frame at a time. */
Action play_out(State &state, const Choices &choices, double from, bool &card_seen,
                double &card_appeared_at, bool press_at_start = false)
{
    Frame frame;
    frame.playing_episode = true;
    frame.has_next = true;
    frame.remaining = from;
    frame.user_pressed = press_at_start;
    for (int i = 0; i < 60 * 60; ++i) /* a minute is plenty */
    {
        const double dt = 1.0 / 60.0;
        frame.remaining = std::max(0.0, frame.remaining - dt);
        frame.ended = frame.remaining <= 0.0;
        const Action action = state.step(dt, frame, choices);
        frame.user_pressed = false;
        if (state.card_visible() && !card_seen)
        {
            card_seen = true;
            card_appeared_at = frame.remaining;
        }
        if (action != Action::none)
            return action;
    }
    return Action::none;
}
} // namespace

int main()
{
    const Choices standard;

    /* One episode: the card appears with the credits, the countdown runs, and
       the next episode starts by itself. */
    {
        State state;
        state.chosen_by_hand();
        bool seen = false;
        double at = 0.0;
        const Action action = play_out(state, standard, 30.0, seen, at);
        check(seen, "the card appears before the end");
        check(at > slopfin::autoplay::kCountdown && at <= slopfin::autoplay::kCardLead + 0.05,
              "and it appears with the credits, not in the last breath");
        check(action == Action::play_next, "the next episode starts by itself");
    }

    /* A real credits marker controls the offer even when it is much earlier
       than the generic fallback. */
    {
        State state;
        state.chosen_by_hand();
        Frame frame;
        frame.playing_episode = frame.has_next = true;
        frame.credits_known = true;
        frame.remaining = 180.0;
        (void)state.step(1.0 / 60.0, frame, standard);
        check(!state.card_visible(), "a known marker prevents an early fallback offer");
        frame.credits_started = true;
        (void)state.step(1.0 / 60.0, frame, standard);
        check(state.card_visible(), "the card appears exactly when marked credits begin");
        check(!state.counting(), "credits can begin before the final autoplay countdown");
    }

    /* Put the card away and nothing starts. */
    {
        State state;
        state.chosen_by_hand();
        Frame frame;
        frame.playing_episode = frame.has_next = true;
        frame.remaining = 10.0;
        (void)state.step(1.0 / 60.0, frame, standard);
        check(state.card_visible(), "the card is up at ten seconds");
        state.dismiss();
        bool seen = false;
        double at = 0.0;
        const Action action = play_out(state, standard, 9.0, seen, at);
        check(!state.card_visible() && action == Action::none, "a dismissed card stays away");
    }

    /* Switching autoplay off in the player cancels a countdown already in
       progress, but keeps the manual Next Episode offer visible. */
    {
        State state;
        state.chosen_by_hand();
        Frame frame;
        frame.playing_episode = frame.has_next = true;
        frame.remaining = 4.0;
        (void)state.step(1.0 / 60.0, frame, standard);
        check(state.card_visible() && state.counting(),
              "the final countdown starts while autoplay is on");
        Choices off = standard;
        off.enabled = false;
        (void)state.step(1.0 / 60.0, frame, off);
        check(state.card_visible() && !state.counting(),
              "turning autoplay off stops the countdown only");
    }

    /* Autoplay off: the card still offers, but nothing counts down. */
    {
        Choices off;
        off.enabled = false;
        State state;
        state.chosen_by_hand();
        bool seen = false;
        double at = 0.0;
        const Action action = play_out(state, off, 25.0, seen, at);
        check(seen, "the offer is still made with autoplay off");
        check(!state.counting() && action == Action::none, "but nothing starts on its own");
    }

    /* Three episodes with nobody pressing anything, then the question. */
    {
        State state;
        state.chosen_by_hand();
        int played = 0;
        Action action = Action::none;
        for (int episode = 0; episode < 6; ++episode)
        {
            bool seen = false;
            double at = 0.0;
            action = play_out(state, standard, 24.0, seen, at);
            if (action == Action::play_next)
            {
                ++played;
                state.played_automatically();
                continue;
            }
            break;
        }
        check(played == 2, "two episodes follow the one that was chosen");
        check(state.asking(), "and then it asks whether anyone is watching");
        check(action == Action::none, "nothing plays behind the question");
    }

    /* A button pressed during an episode means someone is there. */
    {
        State state;
        state.chosen_by_hand();
        for (int episode = 0; episode < 5; ++episode)
        {
            bool seen = false;
            double at = 0.0;
            const Action action = play_out(state, standard, 24.0, seen, at, true /* a press */);
            check(action == Action::play_next, "it keeps going while someone is pressing buttons");
            state.played_automatically();
        }
        check(!state.asking(), "and never asks");
    }

    /* Unanswered, the question stops playback rather than running all night. */
    {
        State state;
        state.chosen_by_hand();
        state.asked();
        Frame frame;
        Action action = Action::none;
        for (int i = 0; i < 60 * 70 && action == Action::none; ++i)
            action = state.step(1.0 / 60.0, frame, standard);
        check(action == Action::ask_still_there, "an unanswered question ends the night");
    }

    /* Answered, it carries on and the count starts again. */
    {
        State state;
        state.chosen_by_hand();
        state.asked();
        state.still_there();
        check(!state.asking() && state.streak() == 0, "a yes clears the run");
    }

    /* A film, or the last episode of a season: no card, nothing automatic. */
    {
        State state;
        state.chosen_by_hand();
        Frame frame;
        frame.playing_episode = true;
        frame.has_next = false;
        frame.remaining = 2.0;
        frame.ended = false;
        const Action action = state.step(1.0 / 60.0, frame, standard);
        check(!state.card_visible() && action == Action::none,
              "nothing is offered with nothing to play");
    }

    /* Left running with nothing pressed, it asks on the clock as well. */
    {
        Choices choices;
        choices.ask_idle_minutes = 90;
        State state;
        state.chosen_by_hand();
        Frame frame;
        frame.playing_episode = true;
        frame.has_next = false;
        frame.remaining = 1e9;
        for (int i = 0; i < 60 * 60 * 89 && !state.asking(); ++i)
            (void)state.step(1.0 / 60.0, frame, choices);
        check(!state.asking(), "it is quiet for the first eighty-nine minutes");
        for (int i = 0; i < 60 * 120 && !state.asking(); ++i)
            (void)state.step(1.0 / 60.0, frame, choices);
        check(state.asking(), "and asks once ninety have passed");
    }

    /* A press keeps the clock at zero. */
    {
        Choices choices;
        choices.ask_idle_minutes = 1;
        State state;
        state.chosen_by_hand();
        Frame frame;
        frame.playing_episode = true;
        frame.remaining = 1e9;
        for (int i = 0; i < 60 * 300; ++i)
        {
            frame.user_pressed = (i % 600) == 0; /* something every ten seconds */
            (void)state.step(1.0 / 60.0, frame, choices);
        }
        check(!state.asking(), "someone pressing buttons is never asked");
    }

    /* Turned off, it never asks however long it sits. */
    {
        Choices choices;
        choices.ask_after = 0;
        choices.ask_idle_minutes = 0;
        State state;
        state.chosen_by_hand();
        Frame frame;
        frame.playing_episode = frame.has_next = true;
        frame.remaining = 1e9;
        for (int i = 0; i < 60 * 60 * 200; ++i)
            (void)state.step(1.0 / 60.0, frame, choices);
        check(!state.asking(), "off means off");
    }

    if (g_failures == 0)
        std::printf("autoplay: all checks passed\n");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
