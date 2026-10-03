/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
// SlopFin subtitle parsing. SPDX-License-Identifier: GPL-3.0-or-later
#include "subtitles.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

int main()
{
    using namespace slopfin::subtitles;
    // BOM, CRLF, an index line, italics, an ASS override, an entity, a two-line
    // cue, a position hint after the end time, and out-of-order input.
    const char *srt =
        "\xEF\xBB\xBF"
        "2\r\n00:00:05,000 --> 00:00:07,500 X1:10\r\n{\\an8}<i>Second</i> &amp; last\r\n\r\n"
        "1\r\n00:00:01,000 --> 00:00:03,000\r\nFirst line\r\nsecond line\r\n\r\n"
        "3\r\n00:01:00.250 --> 00:01:02.000\r\nDot separator\r\n\r\n"
        "4\r\nnot a time --> nonsense\r\nDropped\r\n";
    const auto cues = parse_srt(srt);
    assert(cues.size() == 3);
    assert(std::fabs(cues[0].start - 1.0) < 1e-9 && cues[0].text == "First line\nsecond line");
    assert(cues[1].text == "Second & last");
    assert(std::fabs(cues[1].end - 7.5) < 1e-9);
    assert(std::fabs(cues[2].start - 60.25) < 1e-9);

    assert(active(cues, 0.5).empty());
    assert(active(cues, 1.0) == "First line\nsecond line");
    assert(active(cues, 3.0).empty()); // end is exclusive
    assert(active(cues, 6.0) == "Second & last");
    assert(active(cues, 61.0) == "Dot separator");
    assert(active(cues, 1e9).empty());

    // Overlapping cues are both shown, earliest first.
    const auto overlap = parse_srt("1\n00:00:01,000 --> 00:00:10,000\nA\n\n"
                                   "2\n00:00:02,000 --> 00:00:03,000\nB\n");
    assert(active(overlap, 2.5) == "A\nB");

    // A file with no trailing newline still yields its last cue.
    assert(parse_srt("1\n00:00:01,000 --> 00:00:02,000\nTail").size() == 1);
    assert(parse_srt("").empty());
    puts("Subtitle parsing regressions passed");
}
