/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "video_scale.hpp"
#include "video_scale_simd.hpp"
#include <vector>
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
    for (bool hdr : {false, true})
        for (int width : {1, 2, 3, 4, 5, 7, 16, 1920})
        {
            std::vector<std::uint32_t> input(width), lower(width), doubled(width * 2),
                second(width * 2), a(width * 2), b(width * 2);
            for (int x = 0; x < width; ++x)
            {
                input[x] =
                    hdr ? slopfin::hdr::pack((x * 131) % 1024, (x * 73) % 1024, (x * 41) % 1024)
                        : 0xff000000u | ((x * 7919) & 0xffffff);
                lower[x] =
                    hdr ? slopfin::hdr::pack((x * 11) % 1024, (x * 913) % 1024, (x * 23) % 1024)
                        : 0xff000000u | ((x * 3319) & 0xffffff);
            }
            double_line(input.data(), width, doubled.data(), hdr);
            double_line(lower.data(), width, second.data(), hdr);
            double_rows(doubled.data(), second.data(), width * 2, a.data(), b.data(), hdr);
            for (int x = 0; x < width * 2; ++x)
            {
                auto p = axis(x, width, width * 2);
                assert(doubled[x] == mix(input[p.first], input[p.second], p.weight, hdr));
                assert(a[x] == mix(doubled[x], second[x], 64, hdr));
                assert(b[x] == mix(doubled[x], second[x], 192, hdr));
            }
        }
    for (bool hdr : {false, true})
        for (int source_width : {1, 3, 16, 1280})
            for (int output_width : {1, 7, 1920, 3840})
            {
                std::vector<std::uint32_t> source(source_width), out(output_width),
                    mixed(output_width);
                std::vector<Sample> columns(output_width);
                for (int i = 0; i < source_width; ++i)
                    source[i] = hdr ? slopfin::hdr::pack((i * 391) % 1024, (i * 199) % 1024,
                                                         (i * 79) % 1024)
                                    : 0xff000000u | ((i * 73291) & 0xffffff);
                for (int x = 0; x < output_width; ++x)
                    columns[x] = axis(x, source_width, output_width);
                horizontal_line(source.data(), columns.data(), output_width, out.data(), hdr);
                for (int x = 0; x < output_width; ++x)
                    assert(out[x] == mix(source[columns[x].first], source[columns[x].second],
                                         columns[x].weight, hdr));
                for (unsigned weight : {0, 1, 85, 128, 170, 255})
                {
                    std::vector<std::uint32_t> lower(
                        output_width, hdr ? slopfin::hdr::pack(1011, 59, 713) : 0xff1a64efu);
                    vertical_line(out.data(), lower.data(), output_width, weight, mixed.data(),
                                  hdr);
                    for (int x = 0; x < output_width; ++x)
                        assert(mixed[x] == mix(out[x], lower[x], weight, hdr));
                }
            }
    std::puts("Video scaling coordinates and packed interpolation tests passed");
}
