/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "video_scale.hpp"
#include "hdr.hpp"
#include <cassert>
#include <cstdio>

int main()
{
    using namespace slopfin::video_scale;
    for (int size : {1, 2, 1080, 1920, 3840})
        for (int i = 0; i < size; ++i)
        {
            const auto s = axis(i, size, size);
            assert(s.first == i && s.weight == 0);
            assert(s.second < size);
        }
    assert(axis(0, 2, 4).first == 0);
    assert(axis(1, 2, 4).weight == 64);
    assert(axis(2, 2, 4).weight == 192);
    assert(axis(3, 2, 4).first == 1);
    assert(mix(0xff000000u, 0xffffffffu, 128, false) == 0xff7f7f7fu);
    for (unsigned w = 0; w < 256; ++w)
    {
        assert(mix(0xffab34efu, 0xffab34efu, w, false) == 0xffab34efu);
        const auto c = slopfin::hdr::pack(234, 876, 1023);
        assert(mix(c, c, w, true) == c);
    }
    std::puts("Video scaling coordinates and packed interpolation tests passed");
}
