/*
 * SlopFin - controller input.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Edge-triggered buttons plus auto-repeat on the directions, which is what a
 * ten-foot UI actually needs.
 */

#ifndef SLOPFIN_PAD_HPP
#define SLOPFIN_PAD_HPP

#include <cstdint>

namespace slopfin::pad
{

enum class Button : std::uint8_t
{
    up = 0,
    down,
    left,
    right,
    cross,
    circle,
    square,
    triangle,
    l1,
    r1,
    options,
    l2,
    r2,
    l3,
    r3,
    touchpad,
    count,
};

bool initialize() noexcept;

/* Samples the pad; call once per frame before reading. */
void poll() noexcept;

/* True on the frame the button went down, and again while auto-repeating. */
bool pressed(Button button) noexcept;

/* True while held, no repeat logic. */
bool held(Button button) noexcept;

/* True on a frame any button at all went down: "somebody is there". */
bool any_pressed() noexcept;

/*
 * The right stick's vertical travel, -1 at the top to 1 at the bottom, and
 * exactly zero inside the dead zone. Separate from the directions because it
 * is not one: a long list is scrolled by how far the stick is pushed, not by
 * how many times something was pressed.
 */
float scroll_axis() noexcept;

/*
 * How far each trigger is pressed, 0 at rest to 1 at the stop.
 *
 * The pad reports these as an eight-bit travel, not as a button, and the
 * difference matters for anything continuous: a trigger that is a button can
 * only say "seeking" where one that is an axis can say how fast.
 */
float trigger_left() noexcept;
float trigger_right() noexcept;

/*
 * The physical resistance of the triggers.
 *
 * `seek` loads them progressively: almost free at the top of the travel and
 * firm at the bottom, matching what the travel is driving -- a gentle press
 * creeps and a hard one crosses a film, so the hand is told where in that
 * range it is without looking. `none` releases them.
 */
enum class TriggerFeel : std::uint8_t
{
    none = 0,
    seek,
};

void set_trigger_feel(TriggerFeel feel) noexcept;

/* Whether this firmware offers trigger effects at all, and what the last
   attempt to set one returned. For the settings screen and the trace. */
bool trigger_effects_available() noexcept;
int last_trigger_result() noexcept;

/*
 * Development aid: makes the next poll report this button as pressed, so a
 * remote script can drive the interface without a controller.
 */
void inject(Button button) noexcept;
/*
 * Holds a button down for `frames`, so the remote tooling can exercise the
 * things that are deliberately held rather than pressed -- the dashboard's
 * one-way actions, the seek ramp.
 */
void inject_hold(Button button, int frames) noexcept;

} // namespace slopfin::pad

#endif
