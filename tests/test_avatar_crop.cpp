/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "avatar_crop.hpp"
#include <cassert>
#include <limits>
int main()
{
    using slopfin::avatar::Crop;
    Crop c;
    auto r = c.rect(1920, 1080);
    assert(r.x == 420 && r.y == 0 && r.side == 1080);
    c.move(-100, 100, 1920, 1080);
    r = c.rect(1920, 1080);
    assert(r.x == 0 && r.y == 0);
    c.zoom = 2;
    c.constrain(1920, 1080);
    c.move(100, 100, 1920, 1080);
    r = c.rect(1920, 1080);
    assert(r.x == 1380 && r.y == 540 && r.side == 540);
    c = {};
    r = c.rect(600, 1200);
    assert(r.x == 0 && r.y == 300 && r.side == 600);
    c.zoom = 0;
    c.x = std::numeric_limits<double>::quiet_NaN();
    c.y = std::numeric_limits<double>::infinity();
    c.move(0, 0, 600, 1200);
    r = c.rect(600, 1200);
    assert(r.x == 0 && r.y == 300 && r.side == 600);
    c.zoom = 100;
    c.constrain(600, 1200);
    assert(c.zoom == 6);
    for (int w : {1, 2, 511, 1920, 3840})
        for (int h : {1, 2, 701, 1080, 2160})
            for (int i = 0; i < 100; ++i)
            {
                c.zoom = i * .08;
                c.move(i % 2 ? 1 : -1, i % 3 ? -.2 : .2, w, h);
                r = c.rect(w, h);
                assert(r.side >= 1 && r.x >= 0 && r.y >= 0 && r.x + r.side <= w &&
                       r.y + r.side <= h);
            }
    assert(c.rect(0, 20).side == 0);
}
