/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
// SlopFin playback regressions. SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx.hpp"
#include "presentation_queue.hpp"
#include <cassert>
#include <cstdio>

int main()
{
    using namespace slopfin;
    // Inspect actual output bytes as the display interprets them, independent
    // of the staging screenshot reader. Red must occupy byte zero of RGBA.
    const auto red = gfx::scanout_pixel(gfx::rgb(255, 0, 0));
    const auto *bytes = reinterpret_cast<const unsigned char *>(&red);
    assert(bytes[0] == 255 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 255);
    assert(gfx::scanout_pixel(gfx::rgb(0, 0, 255)) == 0xffff0000u);
    assert(gfx::scanout_pixel(gfx::rgb(128, 128, 128)) == 0xff808080u);

    player::PresentationQueue pending;
    std::int64_t pts = -2;
    assert(!pending.take(pts));
    // I/P/B/B decode order. The first calls buffer pictures. VideoDec2's
    // output is already reordered, so pair it with the earliest pending PTS.
    for (const auto input : {0, 120000, 40000})
        assert(pending.push(input));
    assert(pending.take(pts) && pts == 0);
    assert(pending.push(80000));
    assert(pending.take(pts) && pts == 40000);
    assert(pending.take(pts) && pts == 80000);
    assert(pending.take(pts) && pts == 120000);
    assert(!pending.take(pts));
    // Unknown timestamps remain consumable, and a stalled decoder cannot
    // grow the queue indefinitely.
    assert(pending.push(-1));
    assert(pending.take(pts) && pts == -1);
    for (int i = 0; i < 64; ++i)
        assert(pending.push(63 - i));
    assert(!pending.push(65));
    for (int i = 0; i < 64; ++i)
        assert(pending.take(pts) && pts == i);
    puts("Playback timestamp and scan-out regressions passed");
}
