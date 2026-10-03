/*
 * SlopFin - host tests for subtitle appearance choices.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The one promise that matters most: a viewer who never opens the page sees
 * exactly what they saw before. The expected defaults are the literal values
 * draw_subtitle_lines used before this setting existed.
 */

#include "subtitle_style.hpp"

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
} // namespace

int main()
{
    using namespace slopfin::subtitle_style;

    /* Before: size 42, Weight::medium, palette text (white), four 2 px offsets
       in black, 132 px above the bottom, no box. */
    const Style before = resolve(Choices{});
    check(before.pixel_size == 42, "default size is the old 42 px");
    check(before.font == Font::standard && !before.bold, "default face is the old one");
    check(before.colour == 0xffffffffu, "default colour is white");
    check(before.edge == Edge::outline && before.edge_px == 2,
          "default edge is the old 2 px outline");
    check(before.box_alpha == 0, "no box by default");
    check(before.bottom_margin == 132, "default height is the old 132 px");

    Choices big;
    big.size = 3;
    big.font = 1;
    big.background = 2;
    big.position = 2;
    const Style large = resolve(big);
    check(large.pixel_size == 66 && large.edge_px == 3, "the outline grows with the letters");
    check(large.font == Font::readable, "easy-to-read face");
    check(large.box_alpha == 0xf0, "solid box");
    check(large.bottom_margin > before.bottom_margin, "higher sits higher");

    /* Stored values from an older or corrupted file must still land on a real choice. */
    Choices odd;
    odd.size = 9;
    odd.colour = -1;
    const Style safe = resolve(odd);
    check(safe.pixel_size == kSizes[4], "out-of-range size wraps to a real one");
    check(safe.colour == kColours[5], "negative colour wraps to a real one");

    check(summary(Choices{}) == "Medium  \xc2\xb7  Standard  \xc2\xb7  White  \xc2\xb7  Outline",
          "default summary");
    check(summary(big).find("Solid") != std::string::npos, "a box is mentioned when on");

    /*
     * A choice already written to older builds keeps its meaning: the lists
     * grew by appending, never by inserting. These are the indexes as the old
     * build stored them.
     */
    check(kColours[1] == 0xffffe45cu && kColours[2] == 0xfff4ead5u, "colour 1 is still yellow");
    check(static_cast<Edge>(1) == Edge::shadow && static_cast<Edge>(2) == Edge::none,
          "edge 2 is still None, which is what the console has saved");
    check(kBoxAlphas[1] == 0xa0 && kBoxAlphas[2] == 0xf0, "the two box strengths are unchanged");
    check(kSizes[0] == 34 && kMargins[2] == 292, "the old sizes and heights kept their places");

    /*
     * Boxes must not touch: two translucent boxes that overlap paint that band
     * twice, which is the stack of shaded slabs observed in on a two-line cue.
     * The gap is checked at every size and spacing.
     */
    for (int size = 0; size < 5; ++size)
        for (int spacing = 0; spacing < 3; ++spacing)
        {
            Choices c;
            c.size = size;
            c.spacing = spacing;
            c.background = 1;
            const Style s = resolve(c);
            check(s.box_height < s.line_step,
                  "a per-line box is shorter than the line pitch at size " + std::to_string(size));
            /* The letters sit inside their own box, top and bottom. */
            check(s.box_rise > 0 && s.box_rise + s.pixel_size <= s.box_height + s.box_rise,
                  "the box covers the letters at size " + std::to_string(size));
            c.box = 1;
            const Style band = resolve(c);
            check(band.box == Box::band && band.box_height == band.line_step,
                  "a band is one unbroken rectangle");
        }

    if (g_failures == 0)
        std::printf("subtitle style: all checks passed\n");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
