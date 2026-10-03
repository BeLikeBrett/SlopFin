/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
// SlopFin AC-3 framing regressions. SPDX-License-Identifier: GPL-3.0-or-later
#include "ac3.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

int main()
{
    using namespace slopfin::audio;
    std::vector<std::uint8_t> frame(1792);
    frame[0] = 0x0b;
    frame[1] = 0x77;
    frame[4] = 30;
    frame[5] = 8 << 3;
    assert(ac3_frame_bytes(frame.data(), 6) == 0);
    assert(ac3_frame_bytes(frame.data(), frame.size()) == 1792);
    frame[5] = 16 << 3;
    assert(ac3_frame_bytes(frame.data(), frame.size()) == 0); // E-AC-3 is a different decoder.
    frame[5] = 8 << 3;
    frame[4] |= 0x40;
    assert(ac3_frame_bytes(frame.data(), frame.size()) == 0); // 44.1 kHz needs resampling.
    frame[4] = 30;
    for (std::size_t split = 1; split < frame.size(); ++split)
    {
        Ac3Frames stream;
        int count = 0;
        auto consume = [&](const std::uint8_t *data, std::size_t bytes, std::int64_t pts)
        {
            assert(bytes == frame.size());
            assert(std::memcmp(data, frame.data(), bytes) == 0);
            assert(pts == 90000);
            ++count;
        };
        stream.feed(frame.data(), split, 90000, consume);
        assert(count == 0);
        stream.feed(frame.data() + split, frame.size() - split, -1, consume);
        assert(count == 1);
    }
    Ac3Frames stream;
    std::vector<std::uint8_t> joined{1, 2, 3, 0x0b};
    joined.insert(joined.end(), frame.begin(), frame.end());
    joined.insert(joined.end(), frame.begin(), frame.end());
    int count = 0;
    stream.feed(joined.data(), joined.size(), 90000,
                [&](const std::uint8_t *, std::size_t bytes, std::int64_t pts)
                {
                    assert(bytes == 1792);
                    assert(pts == 90000 + 2880 * count++);
                });
    assert(count == 2);
    stream.feed(frame.data(), 100, 0, [](auto, auto, auto) { assert(false); });
    stream.reset();
    stream.feed(frame.data(), frame.size(), 180000,
                [](auto, auto, auto pts) { assert(pts == 180000); });
    puts("AC-3 framing regressions passed");
}
