/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "media_bitrate.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

int main()
{
    slopfin::player::MediaBitrate meter;
    assert(meter.add(100, true, 900000) == 0);
    // 1 MB video and 0.1 MB audio per media second, independent of wall time.
    for (int i = 1; i <= 3; ++i)
    {
        meter.add(100000, false, -1);
        const double rate = meter.add(1000000, true, 900000 + i * 90000);
        assert(rate == (i == 3 ? 8800000 : 0));
    }
    assert(meter.add(100, true, 1169000) == 8800000); // PTS reordering is not a seek.
    // A seek/discontinuity starts a fresh window, never a huge bitrate spike.
    assert(meter.add(100, true, 0) == 0);
    assert(meter.add(100, true, 100 * 90000) == 0);
    std::puts("media bitrate tests passed");
}
