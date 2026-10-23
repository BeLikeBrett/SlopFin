/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "gfx.hpp"
#include "../host/host_platform.hpp"
#include <cassert>
#include <string_view>
#include <vector>
#include <algorithm>
namespace slopfin::trace
{
void mark(std::string_view) noexcept
{
}
} // namespace slopfin::trace
int main()
{
    using namespace slopfin;
    host::Options options;
    options.headless = true;
    assert(host::open_display(options));
    assert(gfx::initialize());
    const auto background = gfx::rgb(8, 9, 10);
    gfx::clear(background);
    gfx::rounded_horizontal_gradient(100, 100, 400, 120, 24, gfx::rgba(0, 164, 220, 180),
                                     gfx::rgba(0, 164, 220, 180));
    auto *p = gfx::back_buffer();
    const int width = gfx::physical_width();
    auto pixel = [&](int x, int y)
    { return p[gfx::to_physical_y(y) * width + gfx::to_physical_x(x)]; };
    const auto outside = pixel(99, 99);
    assert(pixel(100, 100) == outside && pixel(499, 100) == outside && pixel(100, 219) == outside &&
           pixel(499, 219) == outside);
    assert(pixel(124, 100) != outside && pixel(100, 124) != outside && pixel(300, 160) != outside);
    // Uniform colour must have identical coverage at mirrored physical corners.
    const int x = gfx::to_physical_x(100), y = gfx::to_physical_y(100);
    const int w = gfx::to_physical_x(500) - x, h = gfx::to_physical_y(220) - y;
    bool partial = false;
    for (int dy = 0; dy < gfx::to_physical_size(24); ++dy)
        for (int dx = 0; dx < gfx::to_physical_size(24); ++dx)
        {
            const auto c = p[(y + dy) * width + x + dx];
            assert(c == p[(y + dy) * width + x + w - 1 - dx]);
            assert(c == p[(y + h - 1 - dy) * width + x + dx]);
            partial |= c != outside && c != pixel(300, 160);
        }
    assert(partial);
    const std::vector<std::uint32_t> reference(p, p + gfx::surface_words());
    gfx::clear(background);
    gfx::rounded_rect(100, 100, 400, 120, 24, gfx::rgba(0, 164, 220, 180));
    assert(std::equal(reference.begin(), reference.end(), gfx::back_buffer()));
    gfx::clear(background);
    gfx::rounded_vertical_gradient(100, 100, 400, 120, 24, gfx::rgba(0, 164, 220, 180),
                                   gfx::rgba(0, 164, 220, 180));
    assert(std::equal(reference.begin(), reference.end(), gfx::back_buffer()));
    gfx::clear(background);
    gfx::vertical_gradient(100, 100, 400, 120, gfx::rgb(255, 255, 255), gfx::rgb(216, 229, 239));
    const std::vector<std::uint32_t> ramp(p, p + gfx::surface_words());
    gfx::clear(background);
    gfx::rounded_vertical_gradient(100, 100, 400, 120, 24, gfx::rgb(255, 255, 255),
                                   gfx::rgb(216, 229, 239));
    assert(pixel(100, 100) == outside && pixel(499, 219) == outside);
    for (int row = y; row < y + h; ++row)
        assert(p[row * width + x + w / 2] == ramp[row * width + x + w / 2]);
    gfx::clear(background);
    gfx::push_clip(150, 110, 200, 80);
    gfx::rounded_rect(100, 100, 400, 120, 24, gfx::rgba(0, 164, 220, 180));
    gfx::pop_clip();
    assert(pixel(149, 120) == outside && pixel(200, 109) == outside);
    assert(pixel(200, 120) != outside && pixel(350, 120) == outside);
    gfx::rounded_horizontal_gradient(10, 10, -1, 0, 24, background, background);
    gfx::present();
    assert(host::write_png("/tmp/slopfin-rounded-render-test.png"));
    gfx::shutdown();
    host::close_display();
}
