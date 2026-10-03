/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
// Packetization must not change encoded AAC frames or their initial timestamps.
#include "adts_frames.hpp"
#include <cassert>
#include <vector>
#include <algorithm>
using slopfin::audio::AdtsFrames;
static std::vector<std::uint8_t> frame(unsigned length, bool crc = false)
{
    std::vector<std::uint8_t> out(length, 0x35);
    out[0] = 0xff;
    out[1] = crc ? 0xf0 : 0xf1;
    out[2] = 0x4c;
    out[3] = 0x80 | (length >> 11);
    out[4] = (length >> 3) & 255;
    out[5] = (length & 7) << 5;
    out[6] = 0;
    return out;
}
int main()
{
    const auto first = frame(211), second = frame(307, true), largest = frame(8191);
    for (const auto &original : {first, second, largest})
    {
        for (std::size_t split = 1; split < original.size(); ++split)
        {
            AdtsFrames parser;
            unsigned calls = 0;
            auto accept = [&](const std::uint8_t *p, std::size_t n, std::int64_t pts)
            {
                assert(++calls == 1 && n == original.size() && pts == 90000);
                assert(std::equal(p, p + n, original.begin()));
            };
            parser.feed(original.data(), split, 90000, accept);
            assert(calls == 0);
            parser.feed(original.data() + split, original.size() - split, 92000, accept);
            assert(calls == 1);
        }
    }
    std::vector<std::uint8_t> stream = first;
    stream.insert(stream.end(), second.begin(), second.end());
    for (const auto chunk : {1u, 2u, 7u, 188u, 4096u})
    {
        AdtsFrames parser;
        unsigned calls = 0;
        for (std::size_t at = 0; at < stream.size(); at += chunk)
            parser.feed(stream.data() + at, std::min<std::size_t>(chunk, stream.size() - at), 90000,
                        [&](const std::uint8_t *p, std::size_t n, std::int64_t)
                        {
                            const auto &expected = calls++ ? second : first;
                            assert(n == expected.size() && std::equal(p, p + n, expected.begin()));
                        });
        assert(calls == 2);
    }
    AdtsFrames parser;
    unsigned calls = 0;
    auto accept = [&](const std::uint8_t *p, std::size_t n, std::int64_t pts)
    {
        assert(n == first.size() && pts == 180000 && std::equal(p, p + n, first.begin()));
        ++calls;
    };
    parser.feed(second.data(), 11, 90000, accept);
    parser.reset();
    auto invalid = frame(7);
    invalid[2] = 0x7c; // reserved sampling frequency
    invalid.insert(invalid.end(), first.begin(), first.end());
    parser.feed(invalid.data(), invalid.size(), 180000, accept);
    assert(calls == 1);
}
