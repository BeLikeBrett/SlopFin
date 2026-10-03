/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "hdr.hpp"

#include <cassert>
#include <cstdio>

using namespace slopfin::hdr;

int main()
{
    // Compare the integer fast path with an independent floating-point matrix.
    for (int y = 0; y < 1024; y += 7)
        for (int u = 0; u < 1024; u += 17)
            for (int v = 0; v < 1024; v += 19)
            {
                const auto packed = slopfin::hdr::from_ycbcr10(y, u, v);
                const double l = (y - 64) * (1023.0 / 876.0);
                const double cb = (u - 512) * (1023.0 / 896.0);
                const double cr = (v - 512) * (1023.0 / 896.0);
                const double reference[] = {l + 1.4746 * cr, l - .164553 * cb - .571353 * cr,
                                            l + 1.8814 * cb};
                for (int channel = 0; channel < 3; ++channel)
                    assert(std::abs(static_cast<int>((packed >> (10 * channel)) & 1023) -
                                    static_cast<int>(std::clamp(reference[channel], 0.0, 1023.0) +
                                                     .5)) <= 1);
            }

    assert(pack(1023, 0, 0) == 0xc00003ffu);
    assert(pack(0, 1023, 0) == 0xc00ffc00u);
    assert(pack(0, 0, 1023) == 0xfff00000u);
    assert(pack(-1, 1024, 0) == pack(0, 1023, 0));
    assert(from_ycbcr10(64, 512, 512) == pack(0, 0, 0));
    assert(from_ycbcr10(940, 512, 512) == pack(1023, 1023, 1023));
    assert(from_ycbcr10(502, 512, 512) == pack(512, 512, 512));
    // Limited-range BT.2020 red, quantized to a 10-bit YCbCr sample.
    const auto red = from_ycbcr10(294, 387, 960);
    assert((red & 1023) >= 1022);
    assert(((red >> 10) & 1023) <= 1);
    assert(((red >> 20) & 1023) <= 1);
    assert(std::abs(eotf(0.5080784215) - 100.0) < 0.001);
    assert(std::abs(eotf(0.7518270962) - 1000.0) < 0.001);
    Compositor compositor;
    compositor.initialize();
    for (unsigned alpha = 1; alpha < 256; ++alpha)
        for (unsigned code = 0; code < 1024; ++code)
        {
            const auto expected = compositor.encode(compositor.pq[code] * (1.0f - alpha / 255.0f));
            assert(compositor.blend(pack(code, code, code), alpha << 24) ==
                   pack(expected, expected, expected));
        }
    for (int code = 0; code < 1024; ++code)
    {
        assert(std::abs(oetf(eotf(code / 1023.0)) * 1023.0 - code) < 0.001);
        assert(std::abs(compositor.encode(compositor.pq[code]) - code) <= 1);
    }
    const auto white = compositor.blend(pack(0, 0, 0), 0xffffffffu);
    const int white_code = white & 1023;
    assert(std::abs(eotf(white_code / 1023.0) - 203.0) < 1.0);
    assert(white == pack(white_code, white_code, white_code));
    assert(compositor.blend(white, 0x00ff00ffu) == white);
    assert(compositor.blend(white, 0xff000000u) == pack(0, 0, 0));
    const auto half = compositor.blend(white, 0x80000000u);
    assert(std::abs(eotf((half & 1023) / 1023.0) - 203.0 * 127 / 255) < 1.0);
    std::puts("HDR packing, PQ, BT.2020 matrix and linear-light UI tests passed");
}
