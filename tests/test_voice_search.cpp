/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "voice_search_policy.hpp"
#include <array>
#include <cassert>
#include <iostream>

using namespace slopfin::voice_search::detail;
int main()
{
    std::array<std::int16_t, kGrain> block{};
    CaptureGate gate;
    block.fill(12000);
    for (int i = 0; i < 20; ++i)
    {
        block.fill(12000);
        assert(gate.accept(block, true));
        for (auto sample : block)
            assert(sample == 0); // Hardware mute gates PCM, not just the meter.
    }
    assert(!gate.heard_speech() && gate.level == 0);

    gate = {};
    block.fill(1800);
    for (int i = 0; i < 20; ++i)
        assert(gate.accept(block, false));
    assert(gate.heard_speech());
    block.fill(0);
    for (int i = 0; i < 74; ++i)
        assert(gate.accept(block, false));
    assert(!gate.accept(block, false)); // 75 grains = 1.2 seconds quiet.

    gate = {};
    block.fill(0);
    int grains = 0;
    while (gate.accept(block, false))
        ++grains;
    assert(grains == 749); // No-speech capture remains bounded to 12 seconds.
    assert(!gate.heard_speech());

    gate = {};
    block.fill(3000);
    grains = 0;
    while (gate.accept(block, false))
        ++grains;
    assert(grains == 749); // Continuous sound cannot record indefinitely.

    assert(clean_query(" \n Dragon\t Ball Z. \r\n") == "Dragon Ball Z");
    assert(clean_query("千と千尋の神隠し!") == "千と千尋の神隠し");
    assert(clean_query("\n\r\t").empty());
    assert(clean_query(std::string(800, 'x')).size() == 240);
    std::string boundary(238, 'a');
    boundary += "映画";
    assert(clean_query(boundary) == std::string(238, 'a'));
    std::cout << "Voice capture mute, silence, bounds and search text tests passed\n";
}
