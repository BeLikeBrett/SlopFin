/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "picture_subtitle_layout.hpp"
#include <cassert>
#include <cstdio>
int main()
{
    using namespace slopfin;
    bitmap_subtitles::Frame frame;
    frame.width = 1920;
    frame.height = 1080;
    frame.rects = {{500, 900, 920, 90, {}}};
    auto original = picture_subtitle_layout::place(frame, 100, 0).apply(frame.rects[0]);
    assert(original.x == 500 && original.y == 900 && original.width == 920 &&
           original.height == 90);
    auto large = picture_subtitle_layout::place(frame, 150, 0).apply(frame.rects[0]);
    assert(large.width == 1380 && large.height == 135 && large.y + large.height == 990);
    assert(large.x + large.width / 2 == 960);
    auto lower = picture_subtitle_layout::place(frame, 100, 1).apply(frame.rects[0]);
    auto higher = picture_subtitle_layout::place(frame, 100, 2).apply(frame.rects[0]);
    auto top = picture_subtitle_layout::place(frame, 100, 3).apply(frame.rects[0]);
    assert(top.y < higher.y && higher.y < lower.y);
    frame.rects.push_back({10, 10, 1900, 30, {}});
    auto huge = picture_subtitle_layout::place(frame, 200, 3);
    for (auto &r : frame.rects)
    {
        const auto box = huge.apply(r);
        assert(box.x >= 0 && box.y >= 0 && box.x + box.width <= 1920 && box.y + box.height <= 1080);
    }
    frame.width = 720;
    frame.height = 480;
    frame.rects = {{100, 400, 520, 50, {}}};
    auto dvd = picture_subtitle_layout::place(frame, 125, 2).apply(frame.rects[0]);
    assert(dvd.width > 0 && dvd.height > 0 && dvd.x >= 0 && dvd.x + dvd.width <= 1920);
    std::puts("Picture subtitle original placement, sizing, positions and safe bounds passed");
}
