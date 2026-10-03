/*
 * SlopFin - how text subtitles look, as chosen in Settings.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every option is a short named list rather than a number to tune, because a
 * viewer choosing on a TV needs to see a handful of good choices, not a
 * slider. Index 0 of every list is today's look -- 42 px Noto Sans Medium,
 * white, a two-pixel outline, 132 px up from the bottom -- so nothing changes
 * for anyone who never opens the page.
 *
 * Only text subtitles (SubRip, MP4 text, and ASS shown as plain text) are
 * drawn by the app. Picture subtitles (Blu-ray PGS, DVD) are images drawn into
 * the video by the server and cannot be restyled; the one useful choice for
 * them -- prefer a text track instead -- is `Choices::picture`.
 *
 * Header-only so the choices and the defaults are tested on the host.
 */

#ifndef SLOPFIN_SUBTITLE_STYLE_HPP
#define SLOPFIN_SUBTITLE_STYLE_HPP

#include <algorithm>
#include <cstdint>
#include <string>

namespace slopfin::subtitle_style
{

enum class Font : std::uint8_t
{
    standard, /* Noto Sans, the rest of the app's type */
    readable, /* Atkinson Hyperlegible, drawn for low-vision legibility */
};

/* Order is the order these have always been stored in; a new one is appended
   rather than inserted, so a saved choice keeps its meaning. */
enum class Edge : std::uint8_t
{
    outline,
    shadow,
    none,
    heavy, /* a thicker outline, for a bright picture */
};

/* Where a box is painted, when one is asked for. */
enum class Box : std::uint8_t
{
    per_line, /* a rounded box hugging each line, with a gap between them */
    band,     /* one rectangle behind the whole cue, edge to edge of the text */
};

/* The choices, as stored: one index per list. */
struct Choices
{
    int size = 1;       /* Small, Medium, Large, Extra large, Huge */
    int font = 0;       /* Standard, Easy to read */
    int bold = 0;       /* Normal, Bold */
    int colour = 0;     /* White, Yellow, Soft cream, Sky, Green, Grey */
    int edge = 0;       /* Outline, Drop shadow, None, Thick outline */
    int background = 0; /* None, See-through, Solid */
    int box = 0;        /* Behind each line, One band */
    int spacing = 1;    /* Tight, Normal, Roomy */
    int position = 0;   /* Bottom, A little higher, Higher, Near the top */
    int picture = 0;    /* Draw into the picture, Use a text track when there is one */
};

/* What drawing needs, resolved from the choices. */
struct Style
{
    int pixel_size = 42;
    Font font = Font::standard;
    bool bold = false;
    std::uint32_t colour = 0xffffffffu; /* 0xAARRGGBB */
    Edge edge = Edge::outline;
    int edge_px = 2;
    std::uint8_t box_alpha = 0; /* 0 means no box */
    Box box = Box::per_line;
    int bottom_margin = 132;
    /* Baseline-to-baseline distance, and the box that fits inside it without
       touching the next one: a translucent box that overlaps its neighbour
       paints that band twice and reads as a stack of shaded slabs. */
    int line_step = 52;
    int box_height = 47;
    int box_pad_x = 14;
    /* Where the box starts, relative to the line's drawing position. */
    int box_rise = 3;
};

inline constexpr const char *kSizeNames[] = {"Small", "Medium", "Large", "Extra large", "Huge"};
inline constexpr const char *kFontNames[] = {"Standard", "Easy to read"};
inline constexpr const char *kBoldNames[] = {"Normal", "Bold"};
inline constexpr const char *kColourNames[] = {"White", "Yellow", "Soft cream",
                                               "Sky",   "Green",  "Grey"};
inline constexpr const char *kEdgeNames[] = {"Outline", "Drop shadow", "None", "Thick outline"};
inline constexpr const char *kBackgroundNames[] = {"None", "See-through", "Solid"};
inline constexpr const char *kBoxNames[] = {"Behind each line", "One band"};
inline constexpr const char *kSpacingNames[] = {"Tight", "Normal", "Roomy"};
inline constexpr const char *kPositionNames[] = {"Bottom", "A little higher", "Higher",
                                                 "Near the top"};
inline constexpr const char *kPictureNames[] = {"Draw into the picture",
                                                "Use a text track when there is one"};

inline constexpr int kSizes[] = {34, 42, 54, 66, 78};
/* The first three are the original list, in their original order. */
inline constexpr std::uint32_t kColours[] = {0xffffffffu, 0xffffe45cu, 0xfff4ead5u,
                                             0xffa8d8ffu, 0xffa8f0b4u, 0xffc0c4ccu};
inline constexpr int kMargins[] = {132, 212, 292, 380};
inline constexpr std::uint8_t kBoxAlphas[] = {0, 0xa0, 0xf0};
/* Baseline pitch as a percentage of the type size. */
inline constexpr int kSpacings[] = {112, 125, 145};

inline int wrap(int value, int count) noexcept
{
    return ((value % count) + count) % count;
}

inline Style resolve(const Choices &c) noexcept
{
    Style s;
    s.pixel_size = kSizes[wrap(c.size, 5)];
    s.font = wrap(c.font, 2) == 1 ? Font::readable : Font::standard;
    s.bold = wrap(c.bold, 2) == 1;
    s.colour = kColours[wrap(c.colour, 6)];
    s.edge = static_cast<Edge>(wrap(c.edge, 4));
    /* The outline keeps the same weight relative to the letters at any size. */
    s.edge_px = std::max(2, s.pixel_size / 20);
    if (s.edge == Edge::heavy)
        s.edge_px = std::max(3, s.pixel_size / 12);
    s.box_alpha = kBoxAlphas[wrap(c.background, 3)];
    s.box = wrap(c.box, 2) == 1 ? Box::band : Box::per_line;
    s.bottom_margin = kMargins[wrap(c.position, 4)];
    s.line_step = s.pixel_size * kSpacings[wrap(c.spacing, 3)] / 100;
    /*
     * The box is shorter than the pitch, so two lines' boxes never touch --
     * they used to be exactly one line apart and a translucent one overlapped
     * the next, painting a darker band across every join .
     * A band is one rectangle behind everything, so it is not split at all.
     */
    s.box_pad_x = s.pixel_size / 3;
    const int gap = std::max(4, s.line_step - s.pixel_size * 112 / 100);
    s.box_height = s.box == Box::band ? s.line_step : s.line_step - gap;
    s.box_rise = (s.box_height - s.pixel_size) / 2 + s.pixel_size / 8;
    return s;
}

/* One line for the Settings list: "Medium  ·  Standard  ·  White  ·  Outline". */
inline std::string summary(const Choices &c)
{
    const char *const dot = "  \xc2\xb7  ";
    std::string out = std::string{kSizeNames[wrap(c.size, 5)]} + dot + kFontNames[wrap(c.font, 2)];
    if (wrap(c.bold, 2) == 1)
        out += std::string{dot} + "Bold";
    out += std::string{dot} + kColourNames[wrap(c.colour, 6)] + dot + kEdgeNames[wrap(c.edge, 4)];
    if (wrap(c.background, 3) != 0)
        out += std::string{dot} + kBackgroundNames[wrap(c.background, 3)] + " " +
               kBoxNames[wrap(c.box, 2)];
    return out;
}

} // namespace slopfin::subtitle_style

#endif
