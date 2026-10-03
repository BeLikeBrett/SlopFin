/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/telemetry.cpp"
#include <cassert>
#include <cstdio>
int main()
{
    using namespace slopfin::telemetry;
    enable(true);
    Sample old{};
    old.at_us = 100;
    record(old);
    enable(false);
    enable(true);
    assert(g_count == 0 && g_next == 0);
    Sample current{};
    current.at_us = 100000000;
    record(current);
    assert(g_count == 1 && g_ring[0].at_us == current.at_us);
    enable(false);
    record(old);
    assert(g_count == 1);
    std::puts("Frame trace recordings are isolated");
}
