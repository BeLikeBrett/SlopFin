/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <algorithm>
#include <cmath>

namespace slopfin::avatar
{
struct Rect
{
    int x, y, side;
};

struct Crop
{
    double x = 0.5;
    double y = 0.5;
    double zoom = 1.0;

    void constrain(int width, int height) noexcept
    {
        zoom = std::clamp(std::isfinite(zoom) ? zoom : 1.0, 1.0, 6.0);
        if (width <= 0 || height <= 0)
            return;
        const double side = std::min(width, height) / zoom;
        const double half_x = side / (2.0 * width);
        const double half_y = side / (2.0 * height);
        x = std::clamp(std::isfinite(x) ? x : 0.5, half_x, 1.0 - half_x);
        y = std::clamp(std::isfinite(y) ? y : 0.5, half_y, 1.0 - half_y);
    }

    Rect rect(int width, int height) const noexcept
    {
        if (width <= 0 || height <= 0)
            return {0, 0, 0};
        Crop bounded = *this;
        bounded.constrain(width, height);
        const int side =
            std::max(1, static_cast<int>(std::lround(std::min(width, height) / bounded.zoom)));
        return {std::clamp(static_cast<int>(std::lround(bounded.x * width - side * 0.5)), 0,
                           width - side),
                std::clamp(static_cast<int>(std::lround(bounded.y * height - side * 0.5)), 0,
                           height - side),
                side};
    }

    void move(double dx, double dy, int width, int height) noexcept
    {
        if (width <= 0 || height <= 0)
            return;
        constrain(width, height);
        const double side = std::min(width, height) / zoom;
        x += dx * side / width;
        y += dy * side / height;
        constrain(width, height);
    }
};
} // namespace slopfin::avatar
