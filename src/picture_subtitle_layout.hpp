/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_PICTURE_SUBTITLE_LAYOUT_HPP
#define SLOPFIN_PICTURE_SUBTITLE_LAYOUT_HPP
#include "bitmap_subtitles.hpp"
#include <algorithm>
#include <cmath>
namespace slopfin::picture_subtitle_layout
{
struct Box
{
    int x = 0, y = 0, width = 0, height = 0;
};
struct Transform
{
    double scale = 0, x = 0, y = 0;
    Box apply(const bitmap_subtitles::Rect &r) const noexcept
    {
        const int left = static_cast<int>(std::lround(x + r.x * scale));
        const int top = static_cast<int>(std::lround(y + r.y * scale));
        return {left, top, static_cast<int>(std::lround(x + (r.x + r.width) * scale)) - left,
                static_cast<int>(std::lround(y + (r.y + r.height) * scale)) - top};
    }
};
/* Original, Bottom, Higher, Top. Resize the complete composition together,
   preserving spacing, aspect and source placement unless a position is chosen. */
inline Transform place(const bitmap_subtitles::Frame &frame, int percent, int position,
                       Box view = {0, 0, 1920, 1080}) noexcept
{
    if (frame.width <= 0 || frame.height <= 0 || view.width <= 0 || view.height <= 0)
        return {};
    const double original =
        std::min(double(view.width) / frame.width, double(view.height) / frame.height);
    Transform out{original, view.x + (view.width - frame.width * original) / 2,
                  view.y + (view.height - frame.height * original) / 2};
    if (frame.rects.empty() || (percent == 100 && position == 0))
        return out;
    int left = frame.width, top = frame.height, right = 0, bottom = 0;
    for (const auto &r : frame.rects)
    {
        left = std::min(left, r.x);
        top = std::min(top, r.y);
        right = std::max(right, r.x + r.width);
        bottom = std::max(bottom, r.y + r.height);
    }
    if (right <= left || bottom <= top)
        return out;
    const double margin = std::min(view.width, view.height) * 0.025;
    const double scale = std::min({original * std::clamp(percent, 50, 200) / 100.,
                                   (view.width - 2 * margin) / (right - left),
                                   (view.height - 2 * margin) / (bottom - top)});
    out.x += (left + right) * original / 2. - (left + right) * scale / 2.;
    out.y += bottom * original - bottom * scale;
    if (position > 0)
    {
        out.x = view.x + view.width / 2. - (left + right) * scale / 2.;
        if (position == 3)
            out.y = view.y + view.height * 0.07 - top * scale;
        else
            out.y = view.y + view.height * (position == 2 ? 0.78 : 0.93) - bottom * scale;
    }
    const double box_w = (right - left) * scale, box_h = (bottom - top) * scale;
    out.x =
        std::clamp(out.x + left * scale, view.x + margin, view.x + view.width - margin - box_w) -
        left * scale;
    out.y =
        std::clamp(out.y + top * scale, view.y + margin, view.y + view.height - margin - box_h) -
        top * scale;
    out.scale = scale;
    return out;
}
} // namespace slopfin::picture_subtitle_layout
#endif
