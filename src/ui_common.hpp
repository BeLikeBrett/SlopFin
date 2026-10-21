/*
 * SlopFin - layout and controls shared by every screen.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Moved out of app.cpp  when the profile and dashboard
 * screens got files of their own, so a hint chip or the safe area means the
 * same thing wherever it is drawn.
 */

#ifndef SLOPFIN_UI_COMMON_HPP
#define SLOPFIN_UI_COMMON_HPP

#include "gfx.hpp"
#include "icons.hpp"
#include "text.hpp"

#include <algorithm>
#include <initializer_list>
#include <string>

namespace slopfin::ui
{

/* Televisions overscan, so nothing meaningful sits within 5 percent of an edge. */
inline constexpr int kSafeX = 96;
inline constexpr int kSafeY = 54;
inline constexpr int kSidebarWidth = 336;
inline constexpr int kContentX = kSidebarWidth + 48;
inline constexpr int kContentRight = gfx::kWidth - kSafeX;
inline constexpr int kContentWidth = kContentRight - kContentX;
inline constexpr int kGridTop = 250;

/*
 * The tab bar across the top of every browsing screen: the wordmark, Home and
 * Discover in the middle, search and the profile on the right, all on one
 * line. A page's own title starts under it.
 */
inline constexpr int kTopBarY = kSafeY;
inline constexpr int kTopBarHeight = 72;
inline constexpr int kPageTitleY = 164;

/*
 * Home and Discover are two places, and each has its own light: the colour
 * the page glows at the top, and the gradient its selection is drawn in.
 */
struct Theme
{
    gfx::Color page_top;
    gfx::Color accent_from;
    gfx::Color accent_to;
};
inline constexpr Theme kHomeTheme{gfx::rgb(0x10, 0x1d, 0x33), gfx::rgb(0x2f, 0x7c, 0xff),
                                  gfx::rgb(0x00, 0xc2, 0xe6)};
inline constexpr Theme kDiscoverTheme{gfx::rgb(0x29, 0x12, 0x3f), gfx::rgb(0x8b, 0x5c, 0xf6),
                                      gfx::rgb(0xe0, 0x4f, 0xc2)};
/* Between the two, for the moment one turns into the other. 0 Home, 1 Discover. */
gfx::Color mix(gfx::Color from, gfx::Color to, float amount) noexcept;
Theme theme_at(float discover) noexcept;

/* The page glows from the top and settles to this by kPageRamp. */
inline constexpr gfx::Color kPageBottom = gfx::rgb(0x0d, 0x0d, 0x12);
inline constexpr int kPageRamp = 460;
/* The page's colour at one row, so a fade over a scrolling list matches it. */
gfx::Color page_at(const Theme &theme, int y) noexcept;

/*
 * A capsule of choices with the chosen one on a lifted, coloured pill.
 * `chosen` is fractional while the pill slides from one choice to another.
 * Returns the width used; segmented_width answers the same without drawing.
 */
struct Segmented
{
    std::initializer_list<const char *> labels;
    int height = 52;
    int size = 22;    /* label pixel size */
    int padding = 26; /* either side of each label */
};
int segmented_width(const Segmented &control) noexcept;
int draw_segmented(int x, int y, const Segmented &control, float chosen, const Theme &theme,
                   bool focused) noexcept;

/*
 * A row of "button — what it does" hints.
 *
 * Drawn from the pad's own shapes rather than written out, because "triangle"
 * is a word and the thing on the controller is a shape: a viewer matches the
 * shape without reading anything.
 */
struct ButtonHint
{
    icons::Icon icon;
    const char *label;
    /* A shoulder or trigger has no shape of its own, only a name. Set this and
       the hint draws a small labelled pill instead of a glyph. */
    const char *pad_text = nullptr;
};

gfx::Color hint_colour(icons::Icon icon) noexcept;

/* One button and what it does, on a pill of its own. Returns the width used. */
int draw_button_chip(int x, int y, icons::Icon icon, const std::string &label, bool lit) noexcept;

/* The top bar's size of chip: the height of the profile badge, so the two
   sit on one line as a pair. Returns the width used. */
inline constexpr int kTopChipHeight = 72;
int draw_top_chip(int x, int y, icons::Icon icon, const std::string &label) noexcept;
int top_chip_width(const std::string &label) noexcept;

/* The width draw_button_chip would use. */
int button_chip_width(const std::string &label) noexcept;

/* A legend of hints in a row. Returns the x after the last one. */
int draw_button_hints(int x, int y, std::initializer_list<ButtonHint> hints) noexcept;

/* A dark control with a crisp cyan outline. Shared by lists and actions. */
enum class FocusGroup
{
    none,
    sidebar,
    settings,
    setup,
    account,
    dashboard_nav,
    dashboard_rows,
    dashboard_actions,
    discover_toolbar,
    discover_tabs,
    discover_sheet,
    discover_episodes,
    count
};
/* Call once per rendered frame. A new screen snaps into place; navigation glides. */
void begin_frame(float seconds) noexcept;
void focus_pill(int x, int y, int w, int h, FocusGroup group = FocusGroup::none,
                int context = 0) noexcept;
void control_label(int x, int y, int w, int h, std::string_view label, int size,
                   text::Weight weight, gfx::Color color) noexcept;

/* A raised surface with a soft shadow. */
void panel(int x, int y, int w, int h) noexcept;

/*
 * The focus a stick-scrolled grid keeps: the same column, on a row wholly on
 * screen, so nothing has to scroll back into place when the stick is let go.
 */
inline int stick_focus(int index, int columns, int rows, float scroll, int row_height,
                       int viewport) noexcept
{
    if (columns <= 0 || rows <= 0 || row_height <= 0)
        return index;
    const int top = static_cast<int>(scroll);
    int first = (top + row_height - 1) / row_height;
    int last = (top + viewport) / row_height - 1;
    /* No whole row fits: settle for the one at the top. */
    if (last < first)
        first = last = top / row_height;
    last = std::min(last, rows - 1);
    first = std::min(first, last);
    return std::clamp(index / columns, first, last) * columns + index % columns;
}

/* The search field Home and Discover share, with the Triangle chip beside it
   and a one-line answer after that. The keyboard opens just beneath it. */
inline constexpr int kSearchFieldY = kPageTitleY - 6;
inline constexpr int kSearchFieldH = 72;
inline constexpr int kSearchFieldW = 832;
inline constexpr int kSearchKeyboardY = kSearchFieldY + kSearchFieldH + 14;
struct SearchField
{
    std::string text;
    std::string placeholder;
    std::string scope; /* what the search is bounded to, inside the field */
    std::string note;  /* "Searching", "12 results" */
    bool typing = false;
};
void draw_search_field(int x, int y, int w, const SearchField &field) noexcept;

} // namespace slopfin::ui

#endif
