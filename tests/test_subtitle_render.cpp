/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "gfx.hpp"
#include "text.hpp"
#include "subtitle_style.hpp"
#include "bitmap_sample.hpp"
#include "video_scale.hpp"
#include <vector>
#include "../host/host_platform.hpp"
#include <cassert>
#include <cstdio>
#include <string_view>
#include <array>
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

    // Wrapping preserves explicit breaks and never splits a UTF-8 character.
    const auto word_width = text::measure("three", 42, text::Weight::medium);
    assert((text::wrap_lines("one two\n\nthree", word_width, 42, text::Weight::medium) ==
            std::vector<std::string>{"one", "two", "", "three"}));
    const auto accent_width = text::measure("éé", 42, text::Weight::medium);
    const auto accents = text::wrap_lines("ééééé", accent_width, 42, text::Weight::medium);
    assert((accents == std::vector<std::string>{"éé", "éé", "é"}));
    assert(text::wrap_lines("", 500, 42, text::Weight::medium).size() == 1);

    const auto bounds = [&]
    {
        std::array<int, 4> box = {10000, 10000, 0, 0};
        const auto *buffer = gfx::back_buffer();
        for (int y = 150; y < 450; ++y)
            for (int x = 150; x < 600; ++x)
                if ((buffer[y * 3840 + x] & 0xffffff) != 0)
                {
                    box[0] = std::min(box[0], x);
                    box[1] = std::min(box[1], y);
                    box[2] = std::max(box[2], x);
                    box[3] = std::max(box[3], y);
                }
        return box;
    };
    gfx::clear(gfx::rgb(0, 0, 0));
    text::draw(100, 100, "O", 42, text::Weight::medium, gfx::rgb(255, 255, 255));
    const auto fill_bounds = bounds();
    gfx::clear(gfx::rgb(0, 0, 0));
    text::draw_edge(100, 100, "O", 42, text::Weight::medium, gfx::rgb(255, 255, 255), 2.8f);
    const auto edge_bounds = bounds();
    assert(edge_bounds[0] <= fill_bounds[0] - 4 && edge_bounds[1] <= fill_bounds[1] - 4);
    assert(edge_bounds[2] >= fill_bounds[2] + 4 && edge_bounds[3] >= fill_bounds[3] + 4);
    int soft_pixels = 0;
    for (int y = edge_bounds[1]; y <= edge_bounds[3]; ++y)
        for (int x = edge_bounds[0]; x <= edge_bounds[2]; ++x)
        {
            const unsigned value = gfx::back_buffer()[y * 3840 + x] & 255;
            soft_pixels += value > 0 && value < 255;
        }
    assert(soft_pixels > 60); // A contour has antialiasing, not a binary enlarged silhouette.

    // Changing thickness/shadow and then returning must not retain a stale mask.
    const std::vector<std::uint32_t> first(gfx::back_buffer(), gfx::back_buffer() + 3840 * 2160);
    text::draw_edge(100, 100, "O", 42, text::Weight::medium, gfx::rgb(255, 0, 0), 1.3f, true);
    text::draw_edge(100, 100, "O", 42, text::Weight::medium, gfx::rgb(255, 0, 0), 1.3f);
    gfx::clear(gfx::rgb(0, 0, 0));
    text::draw_edge(100, 100, "O", 42, text::Weight::medium, gfx::rgb(255, 255, 255), 2.8f);
    assert(std::equal(first.begin(), first.end(), gfx::back_buffer()));

    // A translucent outline is applied once, even where neighbouring letters overlap.
    gfx::clear(gfx::rgb(255, 255, 255));
    text::draw_edge(100, 100, "jiffy WWW", 42, text::Weight::medium, gfx::rgba(0, 0, 0, 128), 5.0f);
    for (int y = 150; y < 450; ++y)
        for (int x = 150; x < 1600; ++x)
            assert((gfx::back_buffer()[y * 3840 + x] & 255) >= 127);

    // Review every edge style and size on both a bright and a dark scene.
    for (int size_index = 0; size_index < 5; ++size_index)
    {
        gfx::clear(gfx::rgb(20, 24, 32));
        gfx::fill_rect(960, 0, 960, 1080, gfx::rgb(205, 211, 220));
        for (int edge = 0; edge < 4; ++edge)
        {
            subtitle_style::Choices choices;
            choices.edge = edge;
            choices.size = size_index;
            const auto style = subtitle_style::resolve(choices);
            const int y = 90 + edge * 240;
            for (int x : {60, 1020})
            {
                text::draw(x, y - 34, subtitle_style::kEdgeNames[edge], 20, text::Weight::medium,
                           gfx::rgb(120, 140, 160));
                if (edge != 2)
                    text::draw_edge(x, y, "Every word. Åéü!", style.pixel_size,
                                    text::Weight::medium, gfx::rgba(0, 0, 0, style.edge_alpha),
                                    style.edge_px, edge == 1);
                text::draw(x, y, "Every word. Åéü!", style.pixel_size, text::Weight::medium,
                           style.colour);
            }
        }
        gfx::present();
        assert(host::write_png("build/tests/subtitle-edges-size-" + std::to_string(size_index) +
                               ".png"));
    }
    // Every packaged family/weight must produce distinct pixels, including
    // faces above index 7 (which used to alias a three-bit glyph cache key).
    constexpr text::Weight families[][2] = {{text::Weight::medium, text::Weight::bold},
                                            {text::Weight::readable, text::Weight::readable_bold},
                                            {text::Weight::roboto, text::Weight::roboto_bold},
                                            {text::Weight::lato, text::Weight::lato_bold},
                                            {text::Weight::serif, text::Weight::serif_bold},
                                            {text::Weight::mono, text::Weight::mono_bold}};
    std::vector<std::uint64_t> fingerprints;
    for (const auto &family : families)
        for (const auto weight : family)
        {
            gfx::clear(gfx::rgb(0, 0, 0));
            text::draw_edge(100, 100, "Every word. Åéü!", 42, weight, gfx::rgb(60, 90, 150), 2.0f);
            text::draw(100, 100, "Every word. Åéü!", 42, weight, gfx::rgb(255, 255, 255));
            std::uint64_t hash = 1469598103934665603ULL;
            for (int y = 160; y < 400; ++y)
                for (int x = 160; x < 1600; ++x)
                    hash = (hash ^ gfx::back_buffer()[y * 3840 + x]) * 1099511628211ULL;
            assert(std::find(fingerprints.begin(), fingerprints.end(), hash) == fingerprints.end());
            fingerprints.push_back(hash);
        }
    assert(text::measure("iiii", 42, text::Weight::mono) ==
           text::measure("WWWW", 42, text::Weight::mono));
    assert(text::measure("iiii", 42, text::Weight::medium) !=
           text::measure("WWWW", 42, text::Weight::medium));
    gfx::clear(gfx::rgb(20, 24, 32));
    for (int i = 0; i < subtitle_style::kFontCount; ++i)
    {
        const int y = 36 + i * 174;
        text::draw(60, y, subtitle_style::kFontNames[i], 22, text::Weight::medium,
                   gfx::rgb(140, 170, 200));
        for (int bold = 0; bold < 2; ++bold)
        {
            const int x = 60 + bold * 960;
            text::draw_edge(x, y + 46, "Every word. Every scene.", 42, families[i][bold],
                            gfx::rgb(45, 70, 110), 2.0f);
            text::draw(x, y + 46, "Every word. Every scene.", 42, families[i][bold],
                       gfx::rgb(255, 244, 220));
        }
    }
    gfx::present();
    assert(host::write_png("build/tests/subtitle-font-families.png"));

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
