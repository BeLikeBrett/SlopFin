/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "gfx.hpp"
#include "text.hpp"
#include "bitmap_sample.hpp"
#include "video_scale.hpp"
#include <vector>
#include "../host/host_platform.hpp"
#include <cassert>
#include <cstdio>
#include <string_view>
namespace slopfin::trace
{
void mark(std::string_view) noexcept
{
}
} // namespace slopfin::trace
int main()
{
    using namespace slopfin;
    const std::uint32_t edge[] = {0xffffffff, 0, 0xffffffff, 0};
    assert(bitmap_sample::alpha_bilinear(edge, 2, 2, 32768, 0) == 0x80ffffff);
    assert(bitmap_sample::alpha_bilinear(edge, 2, 2, -32768, -32768) == 0xffffffff);
    assert(bitmap_sample::alpha_bilinear(edge, 2, 2, 65536, 65536) == 0);
    host::Options options;
    options.headless = true;
    options.output_width = 3840;
    options.output_height = 2160;
    assert(host::open_display(options) && gfx::initialize() && text::initialize());
    assert(gfx::physical_width() == 3840 && gfx::physical_height() == 2160);
    gfx::clear(gfx::rgb(0, 0, 0));
    text::draw(100, 100, "Subtitle sharpness", 42, text::Weight::medium, gfx::rgb(255, 255, 255));
    const auto *pixels = gfx::back_buffer();
    bool native_detail = false;
    int top = 1000, bottom = 0;
    for (int y = 200; y < 400; ++y)
        for (int x = 200; x < 1800; ++x)
            if ((pixels[y * 3840 + x] & 0xffffff) != 0)
            {
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
    // A real 84-pixel glyph has independent pixels inside each 2x2 block.
    for (int y = 200; y < 400; y += 2)
        for (int x = 200; x < 1800; x += 2)
        {
            auto a = pixels[y * 3840 + x];
            native_detail |= a != pixels[y * 3840 + x + 1] || a != pixels[(y + 1) * 3840 + x] ||
                             a != pixels[(y + 1) * 3840 + x + 1];
        }
    assert(native_detail && bottom - top > 45);
    gfx::present();
    assert(host::write_png("build/tests/subtitles-native-4k.png"));
    // Exercise the actual separable 720p -> 4K compositor, beyond helper tests.
    std::vector<std::uint32_t> video(1280 * 720);
    for (std::size_t i = 0; i < video.size(); ++i)
        video[i] = 0xff000000u | ((static_cast<unsigned>(i) * 7919u) & 0xffffffu);
    gfx::Bitmap video_image{video.data(), 1280, 720};
    gfx::blit_video(video_image, 0, 0, 1920, 1080);
    const auto *scaled = gfx::back_buffer();
    for (int y = 0; y < 2160; y += 7)
        for (int x = 0; x < 3840; x += 7)
        {
            const auto xs = video_scale::axis(x, 1280, 3840), ys = video_scale::axis(y, 720, 2160);
            const auto a = video_scale::mix(video[ys.first * 1280 + xs.first],
                                            video[ys.first * 1280 + xs.second], xs.weight, false);
            const auto b = video_scale::mix(video[ys.second * 1280 + xs.first],
                                            video[ys.second * 1280 + xs.second], xs.weight, false);
            assert(scaled[y * 3840 + x] == video_scale::mix(a, b, ys.weight, false));
        }
    // Independently un-tile the presented image and check every pixel, including
    // partial tile rows at 2160 pixels and the SIMD RGBA/ABGR conversion.
    auto *draw = gfx::back_buffer();
    for (int y = 0; y < 2160; ++y)
        for (int x = 0; x < 3840; ++x)
            draw[y * 3840 + x] =
                0xff000000u |
                ((static_cast<unsigned>(y) * 17393u + static_cast<unsigned>(x) * 3319u) &
                 0xffffffu);
    gfx::present();
    int scan_w = 0, scan_h = 0;
    const auto *scan = host::scanout_pixels(scan_w, scan_h);
    assert(scan && scan_w == 3840 && scan_h == 2160);
    for (int i = 0; i < 3840 * 2160; ++i)
        assert(scan[i] == gfx::scanout_pixel(draw[i]));
    gfx::shutdown();
    host::close_display();
    std::puts("Native 4K text and alpha-correct picture subtitle edges passed");
}
