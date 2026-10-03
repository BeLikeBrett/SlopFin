/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "playback_url.hpp"
#include <cassert>
#include <cstdio>

int main()
{
    using slopfin::jellyfin::with_subtitle_index;
    assert(with_subtitle_index("/stream.ts", -1) == "/stream.ts?SubtitleStreamIndex=-1");
    assert(with_subtitle_index("/stream.ts?a=1&SubtitleMethod=Encode", -1) ==
           "/stream.ts?a=1&SubtitleMethod=Encode&SubtitleStreamIndex=-1");
    assert(with_subtitle_index("/stream.ts?SubtitleStreamIndex=3&a=1&subtitlestreamindex=5#x",
                               -1) == "/stream.ts?a=1&SubtitleStreamIndex=-1#x");
    assert(with_subtitle_index("/stream.ts?token=ab%26cd&SUBTITLESTREAMINDEX=-1", 7) ==
           "/stream.ts?token=ab%26cd&SubtitleStreamIndex=7");
    std::puts("Playback subtitle selection URL tests passed");
}
