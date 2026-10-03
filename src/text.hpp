/*
 * SlopFin - TrueType text rendering.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Glyphs are rasterised on demand into a cache; the UI only ever asks for
 * a string at a pixel size.
 */

#ifndef SLOPFIN_TEXT_HPP
#define SLOPFIN_TEXT_HPP

#include "gfx.hpp"

#include <cstdint>
#include <string_view>

namespace slopfin::text
{

enum class Weight : std::uint8_t
{
    regular = 0,
    medium,
    bold,
    /* Atkinson Hyperlegible, offered for subtitles that need to be easy to read.
       Falls back to regular when the face is missing. */
    readable,
    readable_bold,
};

/* Loads the packaged Noto Sans and Atkinson Hyperlegible faces from /app0/assets. */
bool initialize() noexcept;

/* Width in pixels the string would occupy. */
int measure(std::string_view utf8, int pixel_size, Weight weight) noexcept;

/* Line top that centers the visible glyphs inside a control, not the font em box. */
int centered_y(int y, int height, std::string_view utf8, int pixel_size, Weight weight) noexcept;

/* Draws with (x, y) as the left edge of the baseline-independent top line. */
void draw(int x, int y, std::string_view utf8, int pixel_size, Weight weight,
          gfx::Color color) noexcept;

/* Draws truncated with an ellipsis when wider than max_width. */
void draw_ellipsized(int x, int y, int max_width, std::string_view utf8, int pixel_size,
                     Weight weight, gfx::Color color) noexcept;

/* Word-wraps into max_width, drawing at most max_lines. Returns lines drawn. */
int draw_wrapped(int x, int y, int max_width, int max_lines, int line_height, std::string_view utf8,
                 int pixel_size, Weight weight, gfx::Color color) noexcept;

/* Distance from the top of a line to the next line's top. */
int line_height(int pixel_size) noexcept;

} // namespace slopfin::text

#endif
