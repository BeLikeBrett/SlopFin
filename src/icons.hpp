/*
 * SlopFin - interface icons.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Drawn from signed distance functions rather than bitmaps, so every icon is
 * anti-aliased and sharp at whatever size it is asked for.
 */

#ifndef SLOPFIN_ICONS_HPP
#define SLOPFIN_ICONS_HPP

#include "gfx.hpp"

namespace slopfin::icons
{

enum class Icon : unsigned char
{
    play = 0,
    pause,
    skip_back,
    skip_forward,
    subtitles,
    audio,
    quality,
    info,
    check,
    jellyfin,
    /* The DualSense face buttons, drawn as outlines the way the pad marks
       them. A hint that shows the shape needs no words under it. */
    ps_triangle,
    ps_circle,
    ps_cross,
    ps_square,
    search,
    /* The shoulder triggers, as the pad marks them: a rounded tab with the
       label drawn over it by the caller. */
    r2,
    chevron_left,
    chevron_right,
    count,
};

/* Sony's own colours for the four shapes, so a hint is recognised before it is
   read. */
namespace button
{
inline constexpr gfx::Color triangle = gfx::rgb(0x4b, 0xd6, 0xb0);
inline constexpr gfx::Color circle = gfx::rgb(0xf0, 0x71, 0x78);
inline constexpr gfx::Color cross = gfx::rgb(0x7a, 0xa7, 0xff);
inline constexpr gfx::Color square = gfx::rgb(0xef, 0x8b, 0xc6);
} // namespace button

/* Draws `icon` filling the size-by-size square whose top-left corner is x, y. */
void draw(Icon icon, int x, int y, int size, gfx::Color color) noexcept;

/* The same, swept from one colour to another, for the Jellyfin mark. */
void draw_gradient(Icon icon, int x, int y, int size, gfx::Color from, gfx::Color to) noexcept;

/* The coverage mask itself, for tests: size*size bytes, 0..255. */
const unsigned char *mask(Icon icon, int size) noexcept;

} // namespace slopfin::icons

#endif
