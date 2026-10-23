/*
 * SlopFin - layout and controls shared by every screen.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ui_common.hpp"

#include "text.hpp"
#include "motion.hpp"
#include <algorithm>
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

int button_chip_width(const std::string &label, icons::Icon icon) noexcept
{
    const int glyph = icon == icons::Icon::r3 ? 32 : 24;
    return 18 + glyph + 10 + text::measure(label, 21, Weight::medium) + 18;
}

int draw_button_chip(int x, int y, icons::Icon icon, const std::string &label, bool lit) noexcept
{
    const int kGlyph = icon == icons::Icon::r3 ? 32 : 24;
    constexpr int kHeight = 44;
    const int width = button_chip_width(label, icon);

    gfx::rounded_rect(x, y, width, kHeight, kHeight / 2,
                      lit ? gfx::with_alpha(icons::button::triangle, 0x33)
                          : gfx::rgba(0x00, 0x00, 0x00, 0x8c));
    gfx::stroke_rounded_rect(x, y, width, kHeight, kHeight / 2, 1,
                             lit ? gfx::with_alpha(icons::button::triangle, 0x99)
                                 : gfx::rgba(0xff, 0xff, 0xff, 0x1e));
    icons::draw(icon, x + 18, y + (kHeight - kGlyph) / 2, kGlyph,
                lit ? hint_colour(icon) : gfx::with_alpha(hint_colour(icon), 0xcc));
    if (icon == icons::Icon::r3)
    {
        // Keep the stick silhouette, with a quiet label on its cap.
        constexpr int size = 12;
        const int cap_y = y + (kHeight - kGlyph) / 2 + 3;
        text::draw(x + 18 + (kGlyph - text::measure("R3", size, Weight::bold)) / 2,
                   text::centered_y(cap_y, 18, "R3", size, Weight::bold), "R3", size, Weight::bold,
                   gfx::rgba(0x14, 0x1c, 0x27, 0xee));
    }
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
    std::array<float, 4> from{}, current{}, target{};
    float elapsed = 0.14f;
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
        const std::array<float, 4> target{static_cast<float>(x), static_cast<float>(y),
                                          static_cast<float>(w), static_cast<float>(h)};
        if (!state.ready || state.context != context || state.seen + 1 < focus_frame)
        {
            state.from = state.current = state.target = target;
            state.elapsed = 0.14f;
            state.ready = true;
        }
        if (state.target != target)
        {
            state.from = state.current;
            state.target = target;
            state.elapsed = 0;
        }
        state.context = context;
        // A fixed duration keeps long moves from trailing behind the selected control.
        state.elapsed = std::min(0.14f, state.elapsed + std::max(0.0f, focus_dt));
        const float amount = motion::ease_out_cubic(state.elapsed / 0.14f);
        for (std::size_t i = 0; i < target.size(); ++i)
            state.current[i] = state.from[i] + (state.target[i] - state.from[i]) * amount;
        state.seen = focus_frame;
        x = static_cast<int>(std::lround(state.current[0]));
        y = static_cast<int>(std::lround(state.current[1]));
        w = static_cast<int>(std::lround(state.current[2]));
        h = static_cast<int>(std::lround(state.current[3]));
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

gfx::Color mix(gfx::Color from, gfx::Color to, float amount) noexcept
{
    const float t = std::clamp(amount, 0.0f, 1.0f);
    const auto channel = [&](unsigned shift)
    {
        const float a = static_cast<float>((from >> shift) & 0xffu);
        const float b = static_cast<float>((to >> shift) & 0xffu);
        return static_cast<std::uint32_t>(std::lround(a + (b - a) * t)) << shift;
    };
    return channel(24) | channel(16) | channel(8) | channel(0);
}

Theme theme_at(float discover) noexcept
{
    return {mix(kHomeTheme.page_top, kDiscoverTheme.page_top, discover),
            mix(kHomeTheme.accent_from, kDiscoverTheme.accent_from, discover),
            mix(kHomeTheme.accent_to, kDiscoverTheme.accent_to, discover)};
}

gfx::Color page_at(const Theme &theme, int y) noexcept
{
    if (y >= kPageRamp)
        return kPageBottom;
    return mix(theme.page_top, kPageBottom,
               static_cast<float>(std::max(0, y)) / static_cast<float>(kPageRamp));
}

namespace
{
constexpr int kSegmentInset = 5;

int segment_width(const char *label, const Segmented &control) noexcept
{
    return text::measure(label, control.size, Weight::bold) + control.padding * 2;
}
} // namespace

int segmented_width(const Segmented &control) noexcept
{
    int width = kSegmentInset * 2;
    for (const char *label : control.labels)
        width += segment_width(label, control);
    return width;
}

int draw_segmented(int x, int y, const Segmented &control, float chosen, const Theme &theme,
                   bool focused) noexcept
{
    const int width = segmented_width(control);
    const int h = control.height;
    gfx::rounded_rect(x, y, width, h, h / 2, gfx::rgba(0xff, 0xff, 0xff, 0x12));
    gfx::stroke_rounded_rect(x, y, width, h, h / 2, focused ? 2 : 1,
                             focused ? gfx::palette::focus_border
                                     : gfx::rgba(0xff, 0xff, 0xff, 0x1c));

    /* Where each segment starts, so the pill can sit between two of them. */
    std::array<int, 8> starts{};
    std::array<int, 8> widths{};
    int at = x + kSegmentInset;
    int i = 0;
    for (const char *label : control.labels)
    {
        if (i >= static_cast<int>(starts.size()))
            break;
        starts[static_cast<std::size_t>(i)] = at;
        widths[static_cast<std::size_t>(i)] = segment_width(label, control);
        at += widths[static_cast<std::size_t>(i)];
        ++i;
    }
    const float place = std::clamp(chosen, 0.0f, static_cast<float>(std::max(0, i - 1)));
    const int low = static_cast<int>(std::floor(place));
    const int high = std::min(i - 1, low + 1);
    const float t = place - static_cast<float>(low);
    const auto lerp = [t](int a, int b) {
        return static_cast<int>(std::lround(static_cast<float>(a) + static_cast<float>(b - a) * t));
    };
    if (i > 0)
    {
        const int pill_x =
            lerp(starts[static_cast<std::size_t>(low)], starts[static_cast<std::size_t>(high)]);
        const int pill_w =
            lerp(widths[static_cast<std::size_t>(low)], widths[static_cast<std::size_t>(high)]);
        const int pill_h = h - kSegmentInset * 2;
        gfx::rounded_horizontal_gradient(pill_x, y + kSegmentInset, pill_w, pill_h, pill_h / 2,
                                         theme.accent_from, theme.accent_to);
    }

    i = 0;
    for (const char *label : control.labels)
    {
        if (i >= static_cast<int>(starts.size()))
            break;
        const float near = std::clamp(1.0f - std::fabs(place - static_cast<float>(i)), 0.0f, 1.0f);
        const auto alpha = static_cast<std::uint8_t>(0x8c + std::lround(near * (0xff - 0x8c)));
        const int label_w = text::measure(label, control.size, Weight::bold);
        text::draw(starts[static_cast<std::size_t>(i)] +
                       (widths[static_cast<std::size_t>(i)] - label_w) / 2,
                   text::centered_y(y, h, label, control.size, Weight::bold), label, control.size,
                   Weight::bold, gfx::rgba(0xff, 0xff, 0xff, alpha));
        ++i;
    }
    return width;
}

void panel(int x, int y, int w, int h) noexcept
{
    gfx::rounded_rect(x, y, w, h, 20, gfx::palette::surface);
}

void draw_search_field(int x, int y, int w, const SearchField &field) noexcept
{
    const int h = kSearchFieldH;
    const bool active = field.typing || field.voice;

    /* The field stays legible behind the native keyboard. */
    gfx::rounded_rect(x, y, w, h, h / 2, gfx::rgba(0xff, 0xff, 0xff, active ? 0x16 : 0x10));
    gfx::stroke_rounded_rect(x, y, w, h, h / 2, 2,
                             active ? gfx::rgba(0xff, 0xff, 0xff, 0x8c)
                                    : gfx::rgba(0xff, 0xff, 0xff, 0x24));

    icons::draw(field.voice ? icons::Icon::microphone : icons::Icon::search, x + 26,
                y + (h - 30) / 2, 30, gfx::rgba(0xff, 0xff, 0xff, active ? 0xdd : 0x88));

    const int text_x = x + 70;
    const int text_w = w - 70 - 210;
    control_label(text_x, y, text_w, h, field.text.empty() ? field.placeholder : field.text, 28,
                  text::Weight::medium,
                  field.text.empty() ? gfx::palette::text_dim : gfx::palette::text);

    if (!field.scope.empty())
    {
        const int scope_w = text::measure(field.scope, 20, text::Weight::medium) + 34;
        const int scope_x = x + w - scope_w - 18;
        gfx::rounded_rect(scope_x, y + 19, scope_w, 34, 17, gfx::rgba(0xff, 0xff, 0xff, 0x14));
        text::draw(scope_x + 17,
                   text::centered_y(y + 19, 34, field.scope, 20, text::Weight::medium), field.scope,
                   20, text::Weight::medium, gfx::rgba(0xff, 0xff, 0xff, 0xb4));
    }

    if (field.voice_available)
        draw_button_chip(x + w + 26, y + (h - 44) / 2,
                         field.listening ? icons::Icon::ps_cross : icons::Icon::r3,
                         field.listening ? "Finish" : "Speak", field.voice);
    if (!field.note.empty())
        text::draw(x + 28, y + h + 12, field.note, 20, text::Weight::regular,
                   gfx::palette::text_dim);
}

void apply_voice_search_state(SearchField &field, const voice_search::Snapshot &voice) noexcept
{
    field.voice = voice_search_active(voice.phase);
    field.listening = voice.phase == voice_search::Phase::listening;
    if (field.voice || voice.phase == voice_search::Phase::error)
        field.note.clear();
    if (voice.phase == voice_search::Phase::downloading)
        field.note =
            "One-time download · " +
            std::to_string(static_cast<int>(std::clamp(voice.progress, 0.0f, 1.0f) * 100)) + "%";
    else if (voice.phase == voice_search::Phase::listening ||
             voice.phase == voice_search::Phase::error)
        field.note = voice.message;
}

void draw_search_prompt(int x, int y, bool voice_available) noexcept
{
    constexpr const char *lead = "Press";
    text::draw(x, y, lead, 20, Weight::regular, gfx::palette::text_dim);
    x += text::measure(lead, 20, Weight::regular) + 9;
    icons::draw(icons::Icon::ps_cross, x, y - 1, 22, icons::button::cross);
    text::draw(x + 31, y, voice_available ? "to search or say a movie with R3" : "to search", 20,
               Weight::regular, gfx::palette::text_dim);
}

void draw_voice_search_status(int x, int y, const voice_search::Snapshot &voice) noexcept
{
    if (!voice_search_status_visible(voice.phase))
        return;
    constexpr gfx::Color ink = gfx::rgb(0x7c, 0xd7, 0xe8);
    const bool listening = voice.phase == voice_search::Phase::listening;
    icons::draw(icons::Icon::microphone, x + 14, y + 12, 38,
                voice.muted ? gfx::rgb(0xf0, 0x9b, 0x68) : ink);
    const char *title = listening ? (voice.muted ? "Microphone is muted" : "Say a title")
                        : voice.phase == voice_search::Phase::processing
                            ? "Recognising speech"
                            : "Downloading voice search";
    text::draw(x + 72, y + 10, title, 28, Weight::medium, gfx::palette::text);
    if (listening)
    {
        const float level = voice.muted ? 0.0f : std::clamp(voice.level, 0.0f, 1.0f);
        for (int i = 0; i < 15; ++i)
        {
            const int height = 5 + static_cast<int>(level * (18 - std::abs(7 - i)) * 3);
            gfx::rounded_rect(x + 610 + i * 10, y + 38 - height / 2, 4, height, 2,
                              i < static_cast<int>(level * 15) ? ink
                                                               : gfx::rgba(0xff, 0xff, 0xff, 0x30));
        }
    }
    else if (voice.phase == voice_search::Phase::downloading)
    {
        gfx::rounded_rect(x + 72, y + 91, 420, 4, 2, gfx::rgba(0xff, 0xff, 0xff, 0x30));
        const int progress = static_cast<int>(420 * std::clamp(voice.progress, 0.0f, 1.0f));
        if (progress > 0)
            gfx::rounded_rect(x + 72, y + 91, progress, 4, 2, ink);
    }
}

void draw_search_close_hint(const voice_search::Snapshot &voice) noexcept
{
    if (voice_search_active(voice.phase))
        return;
    const char *label = "Close";
    const int width = 22 + 9 + text::measure(label, 20, Weight::regular);
    draw_button_hints(gfx::kWidth - kSafeX - width, gfx::kHeight - 48,
                      {{icons::Icon::ps_circle, label}});
}

} // namespace slopfin::ui
