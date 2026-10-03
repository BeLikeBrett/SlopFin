/*
 * SlopFin - layout and controls shared by every screen.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ui_common.hpp"

#include "text.hpp"
#include "motion.hpp"
#include <array>
#include <cmath>

namespace slopfin::ui
{
using text::Weight;

gfx::Color hint_colour(icons::Icon icon) noexcept
{
    switch (icon)
    {
    case icons::Icon::ps_triangle:
        return icons::button::triangle;
    case icons::Icon::ps_circle:
        return icons::button::circle;
    case icons::Icon::ps_cross:
        return icons::button::cross;
    case icons::Icon::ps_square:
        return icons::button::square;
    case icons::Icon::r2:
        return gfx::rgba(0xff, 0xff, 0xff, 0xcc);
    default:
        return gfx::palette::text_dim;
    }
}

/* Shared labeled controller action chip with a background readable over artwork. */
int top_chip_width(const std::string &label) noexcept
{
    return 26 + 30 + 14 + text::measure(label, 25, Weight::medium) + 28;
}

int draw_top_chip(int x, int y, icons::Icon icon, const std::string &label) noexcept
{
    const int width = top_chip_width(label);
    gfx::rounded_rect(x, y, width, kTopChipHeight, kTopChipHeight / 2,
                      gfx::rgba(0x00, 0x00, 0x00, 0x8c));
    gfx::stroke_rounded_rect(x, y, width, kTopChipHeight, kTopChipHeight / 2, 1,
                             gfx::rgba(0xff, 0xff, 0xff, 0x24));
    icons::draw(icon, x + 26, y + (kTopChipHeight - 30) / 2, 30, hint_colour(icon));
    text::draw(x + 26 + 30 + 14, text::centered_y(y, kTopChipHeight, label, 25, Weight::medium),
               label, 25, Weight::medium, gfx::rgba(0xff, 0xff, 0xff, 0xdc));
    return width;
}

int button_chip_width(const std::string &label) noexcept
{
    return 18 + 24 + 10 + text::measure(label, 21, Weight::medium) + 18;
}

int draw_button_chip(int x, int y, icons::Icon icon, const std::string &label, bool lit) noexcept
{
    constexpr int kGlyph = 24;
    constexpr int kHeight = 44;
    const int text_w = text::measure(label, 21, Weight::medium);
    const int width = 18 + kGlyph + 10 + text_w + 18;

    gfx::rounded_rect(x, y, width, kHeight, kHeight / 2,
                      lit ? gfx::with_alpha(icons::button::triangle, 0x33)
                          : gfx::rgba(0x00, 0x00, 0x00, 0x8c));
    gfx::stroke_rounded_rect(x, y, width, kHeight, kHeight / 2, 1,
                             lit ? gfx::with_alpha(icons::button::triangle, 0x99)
                                 : gfx::rgba(0xff, 0xff, 0xff, 0x1e));
    icons::draw(icon, x + 18, y + (kHeight - kGlyph) / 2, kGlyph,
                lit ? hint_colour(icon) : gfx::with_alpha(hint_colour(icon), 0xcc));
    text::draw(x + 18 + kGlyph + 10, text::centered_y(y, kHeight, label, 21, Weight::medium), label,
               21, Weight::medium, lit ? gfx::palette::text : gfx::rgba(0xff, 0xff, 0xff, 0xc4));
    return width;
}

int draw_button_hints(int x, int y, std::initializer_list<ButtonHint> hints) noexcept
{
    constexpr int kGlyph = 22;
    for (const ButtonHint &hint : hints)
    {
        if (hint.pad_text != nullptr)
        {
            const int pill_w = text::measure(hint.pad_text, 16, Weight::bold) + 18;
            gfx::rounded_rect(x, y - 2, pill_w, kGlyph + 4, 7, gfx::rgba(0xff, 0xff, 0xff, 0x1c));
            gfx::stroke_rounded_rect(x, y - 2, pill_w, kGlyph + 4, 7, 1,
                                     gfx::rgba(0xff, 0xff, 0xff, 0x4a));
            text::draw(x + 9, y + 3, hint.pad_text, 16, Weight::bold,
                       gfx::rgba(0xff, 0xff, 0xff, 0xe0));
            x += pill_w + 9;
            text::draw(x, y + 1, hint.label, 20, Weight::regular,
                       gfx::rgba(0xff, 0xff, 0xff, 0x9a));
            x += text::measure(hint.label, 20, Weight::regular) + 30;
            continue;
        }
        icons::draw(hint.icon, x, y, kGlyph, hint_colour(hint.icon));
        x += kGlyph + 9;
        text::draw(x, y + 1, hint.label, 20, Weight::regular, gfx::rgba(0xff, 0xff, 0xff, 0x9a));
        x += text::measure(hint.label, 20, Weight::regular) + 30;
    }
    return x;
}

namespace
{
struct FocusState
{
    motion::Spring x, y, w, h;
    unsigned long seen = 0;
    int context = -1;
    bool ready = false;
};
std::array<FocusState, static_cast<std::size_t>(FocusGroup::count)> focus_states;
unsigned long focus_frame = 0;
float focus_dt = 1.0f / 60.0f;

} // namespace

void begin_frame(float seconds) noexcept
{
    ++focus_frame;
    focus_dt = seconds;
}

void focus_pill(int x, int y, int w, int h, FocusGroup group, int context) noexcept
{
    if (group != FocusGroup::none)
    {
        auto &state = focus_states[static_cast<std::size_t>(group)];
        if (!state.ready || state.context != context || state.seen + 1 < focus_frame)
        {
            state.x.jump(x);
            state.y.jump(y);
            state.w.jump(w);
            state.h.jump(h);
            state.ready = true;
        }
        state.context = context;
        state.x.aim(x);
        state.y.aim(y);
        state.w.aim(w);
        state.h.aim(h);
        state.x.step(motion::kControlFocus, focus_dt);
        state.y.step(motion::kControlFocus, focus_dt);
        state.w.step(motion::kControlFocus, focus_dt);
        state.h.step(motion::kControlFocus, focus_dt);
        state.seen = focus_frame;
        x = static_cast<int>(std::lround(state.x.value()));
        y = static_cast<int>(std::lround(state.y.value()));
        w = static_cast<int>(std::lround(state.w.value()));
        h = static_cast<int>(std::lround(state.h.value()));
    }
    gfx::rounded_rect(x, y, w, h, 12, gfx::palette::focus_surface);
    gfx::stroke_rounded_rect(x, y, w, h, 12, 2, gfx::palette::focus_border);
}

void control_label(int x, int y, int w, int h, std::string_view label, int size,
                   text::Weight weight, gfx::Color color) noexcept
{
    text::draw_ellipsized(x, text::centered_y(y, h, label, size, weight), w, label, size, weight,
                          color);
}

void panel(int x, int y, int w, int h) noexcept
{
    gfx::rounded_rect(x + 6, y + 10, w, h, 20, gfx::rgba(0x00, 0x00, 0x00, 0x66));
    gfx::rounded_rect(x, y, w, h, 20, gfx::palette::surface);
}

} // namespace slopfin::ui
