/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_buffer_gate.hpp"
#include <cassert>
int main()
{
    using slopfin::audio::BufferGate;
    BufferGate gate;
    gate.reset(true);
    assert(!gate.ready(100000, 512, false, false)); // video must be ready too
    assert(!gate.ready(71999, 512, true, false));
    assert(gate.ready(72000, 512, true, false) && !gate.waiting());
    assert(!gate.ready(0, 512, true, false) && gate.events() == 1);
    assert(!gate.ready(512, 512, true, false)); // no repeated fragment/silence chatter
    assert(gate.ready(96000, 512, true, false));
    assert(!gate.ready(0, 512, true, false));
    assert(!gate.ready(96000, 512, true, false));
    assert(gate.ready(144000, 512, true, false));
    for (int i = 0; i < 10; ++i)
    {
        assert(!gate.ready(0, 512, true, false));
        assert(gate.ready(192000, 512, true, false)); // bounded adaptation
    }
    gate.reset(true);
    assert(gate.ready(512, 512, true, true)); // short clip/EOF below target
    assert(!gate.ready(0, 512, true, true) && !gate.waiting());
    gate.reset(true);
    for (int i = 0; i < 940; ++i)
        (void)gate.ready(0, 512, true, false);
    assert(gate.failed());
    gate.reset(true);
    for (int i = 0; i < 938; ++i)
        (void)gate.ready(1024, 512, true, false);
    assert(gate.ready(1024, 512, true, false)); // bounded wait if target cannot fit
    gate.reset(false);
    assert(gate.ready(512, 512, false, false)); // existing unbuffered fixtures
}
