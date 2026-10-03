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

} // namespace slopfin::ui

#endif
