/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "gfx.hpp"
#include "gpu_menu.hpp"
#include "text.hpp"
#include "../host/host_platform.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>
namespace slopfin::trace
{
void mark(std::string_view s) noexcept
{
    std::fprintf(stderr, "%.*s\n", static_cast<int>(s.size()), s.data());
}
} // namespace slopfin::trace
int main(int argc, char **argv)
{
    using namespace slopfin;
    host::Options o;
    o.headless = true;
    o.allow_arbitrary_surface = true;
    o.output_width = argc > 1 ? std::atoi(argv[1]) : 1920;
    o.output_height = o.output_width * 9 / 16;
    assert(host::open_display(o) && gfx::initialize() && text::initialize());
    assert(gpu_menu::active());
    std::vector<std::uint32_t> artwork(240 * 360);
    for (std::size_t i = 0; i < artwork.size(); i++)
        artwork[i] = 0xff000000u | ((i * 7919) & 0xffffffu);
    gfx::Bitmap art{artwork.data(), 240, 360};
    const auto scene = [&]
    {
        gfx::clear(gfx::rgb(13, 15, 22));
        gfx::vertical_gradient(0, 0, 1920, 500, gfx::rgb(30, 44, 60), gfx::rgb(13, 15, 22));
        gfx::horizontal_gradient(300, 0, 1620, 1080, gfx::rgba(0, 0, 0, 168),
                                 gfx::rgba(0, 0, 0, 64));
        gfx::media_scrim(720, gfx::rgb(13, 13, 18), 0x8c, 240, 1420, 0xf0);
        gfx::push_clip(900, 450, 800, 390);
        gfx::media_scrim(1080, gfx::rgb(19, 24, 30), 0x99, 360, 1780, 0xf0);
        gfx::pop_clip();
        // Degenerate ramps still darken safely without divisions by zero.
        gfx::push_clip(0, 0, 120, 80);
        gfx::media_scrim(1080, gfx::rgb(8, 16, 24), 0, 1080, 0, 255);
        gfx::pop_clip();
        gfx::fill_rect(13, 21, 101, 61, gfx::rgba(195, 71, 140, 116));
        gfx::rounded_rect(190, 100, 300, 80, 24, gfx::rgba(30, 200, 220, 180));
        gfx::stroke_rounded_rect(510, 100, 300, 80, 24, 3, gfx::rgba(150, 71, 220, 215));
        gfx::rounded_horizontal_gradient(190, 200, 600, 80, 25, gfx::rgba(10, 80, 150, 100),
                                         gfx::rgba(130, 220, 60, 240));
        gfx::rounded_vertical_gradient(820, 200, 160, 56, 19, gfx::rgb(255, 255, 255),
                                       gfx::rgb(216, 229, 239));
        gfx::drop_shadow(190, 350, 240, 360, 18, 24, 140);
        gfx::blit_cover(art, 190, 350, 240, 360, 18, 255);
        gfx::blit_cover(art, 470, 350, 280, 360, 18, 130);
        gfx::push_clip(805, 400, 110, 220);
        gfx::blit_cover(art, 780, 350, 240, 360, 18, 255);
        gfx::pop_clip();
        text::draw(1000, 200, "Native 4K subtitles", 42, text::Weight::medium,
                   gfx::rgb(255, 255, 255));
        text::draw_edge(1000, 260, "Smooth outlines", 42, text::Weight::medium,
                        gfx::rgba(0, 0, 0, 204), 2.8f);
        text::draw(1000, 260, "Smooth outlines", 42, text::Weight::medium, gfx::rgb(255, 255, 255));
        text::draw_edge(1000, 320, "Soft shadow", 42, text::Weight::readable,
                        gfx::rgba(0, 0, 0, 153), 1.3f, true);
        text::draw(1000, 320, "Soft shadow", 42, text::Weight::readable, gfx::rgb(255, 228, 92));
        // New subtitle families occupy indices above the old 3-bit face key.
        constexpr text::Weight extra_faces[] = {text::Weight::roboto_bold, text::Weight::lato_bold,
                                                text::Weight::serif_bold, text::Weight::mono_bold};
        for (int i = 0; i < 4; ++i)
        {
            const int y = 560 + i * 100;
            text::draw_edge(1000, y, "Family Åéü!", 42, extra_faces[i], gfx::rgba(90, 50, 160, 204),
                            2.0f);
            text::draw(1000, y, "Family Åéü!", 42, extra_faces[i], gfx::rgb(255, 235, 200));
        }
        gfx::arc(1100, 450, 50, 7, .2f, .8f, gfx::rgba(220, 120, 80, 200));
        gfx::arc(1250, 450, 17, 3, .54f, .54f, gfx::rgba(88, 212, 233, 255), false);
        gfx::present();
        gpu_menu::finish(gfx::back_buffer());
    };
    gfx::set_menu_acceleration(false);
    scene();
    assert(host::write_png("build/tests/cpu-menu-reference.png"));
    const auto words = gfx::surface_words();
    std::vector<std::uint32_t> reference(gfx::back_buffer(), gfx::back_buffer() + words);
    gfx::set_menu_acceleration(true);
    (void)gpu_menu::take_stats();
    scene();
    assert(gpu_menu::take_stats().cpu_syncs == 0);
    const auto *actual = gfx::back_buffer();
    unsigned max_delta = 0;
    std::size_t different = 0, bad = 0;
    for (std::size_t i = 0; i < words; i++)
    {
        unsigned delta = 0;
        for (int c = 0; c < 24; c += 8)
            delta =
                std::max(delta, static_cast<unsigned>(std::abs(int((actual[i] >> c) & 255) -
                                                               int((reference[i] >> c) & 255))));
        if (delta > 2 && bad < 12)
            std::fprintf(stderr, "diff x=%zu y=%zu cpu=%08x gpu=%08x\n", i % gfx::physical_width(),
                         i / gfx::physical_width(), reference[i], actual[i]);
        max_delta = std::max(max_delta, delta);
        different += delta != 0;
        bad += delta > 2;
    }
    std::printf("%dx%d: GPU/CPU differing=%zu, delta>2=%zu, max=%u\n", o.output_width,
                o.output_height, different, bad, max_delta);
    assert(host::write_png("build/tests/gpu-menu-" + std::to_string(o.output_width) + ".png"));
    assert(bad <
           words / 10000); // At most rounding at analytic arc boundaries, never stale geometry.
    // A reused source address with new contents must not return a stale GPU texture.
    std::fill(artwork.begin(), artwork.end(), gfx::rgb(8, 231, 64));
    int scan_w = 0, scan_h = 0;
    const auto *scan = host::scanout_pixels(scan_w, scan_h);
    assert(scan_w == gfx::physical_width() && scan_h == gfx::physical_height());
    for (std::size_t i = 0; i < words; i++)
        assert(scan[i] == gfx::scanout_pixel(actual[i]));
    gfx::blit(art, 0, 0, 255);
    gfx::present();
    gpu_menu::finish(gfx::back_buffer());
    assert(gfx::back_buffer()[0] == gfx::rgb(8, 231, 64));

    // Wrap the three submission/display slots repeatedly without reading back
    // between frames. Switching to playback's CPU path must drain every flip.
    for (int frame = 0; frame < 9; ++frame)
    {
        gfx::clear(gfx::rgb(20 + frame, 30, 40));
        gfx::fill_rect(100 + frame * 30, 150, 240, 120, gfx::rgb(240, 100, 60));
        text::draw(100, 400, "Queued menu frame", 36, text::Weight::medium,
                   gfx::rgb(255, 255, 255));
        gfx::present();
    }
    gfx::set_menu_acceleration(false);
    assert(!gpu_menu::active());
    scan = host::scanout_pixels(scan_w, scan_h);
    actual = gfx::back_buffer();
    for (std::size_t i = 0; i < words; ++i)
        assert(scan[i] == gfx::scanout_pixel(actual[i]));
    assert(actual[0] == gfx::rgb(28, 30, 40));
    // CPU playback can replace the surface before menus resume.
    gfx::clear(gfx::rgb(17, 121, 199));
    gfx::present();
    gfx::set_menu_acceleration(true);
    gfx::fill_rect(100, 100, 50, 50, gfx::rgb(255, 80, 40));
    gfx::present();
    gpu_menu::finish(gfx::back_buffer());
    assert(gfx::back_buffer()[0] == gfx::rgb(17, 121, 199));
    // Shutdown must also drain a queued frame, without an explicit readback.
    gfx::clear(gfx::rgb(12, 23, 34));
    gfx::present();
    gfx::shutdown();
    host::close_display();
}
