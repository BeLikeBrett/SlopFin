/*
 * SlopFin - text subtitles.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The server converts every text subtitle format to SubRip on request, so this
 * is the only format parsed here. Image formats (PGS, VobSub) are burned into
 * the picture by the server instead, and never reach this code.
 */

#ifndef SLOPFIN_SUBTITLES_HPP
#define SLOPFIN_SUBTITLES_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace slopfin::subtitles
{

struct Cue
{
    double start = 0.0; /* seconds from the start of the title */
    double end = 0.0;
    std::string text; /* styling removed; lines separated by '\n' */
};

/* Parses SubRip. Malformed blocks are skipped rather than failing the file. */
std::vector<Cue> parse_srt(std::string_view text);

/* Every cue showing at `seconds`, joined by newlines; empty when none are. */
std::string active(const std::vector<Cue> &cues, double seconds);

/* Thread-safe store the playback thread fills and the renderer reads. */
void set(std::vector<Cue> cues);
void clear();
std::string current(double seconds);
bool loaded();
/* How many cues are held, for the debug overlay. */
std::size_t count();

} // namespace slopfin::subtitles

#endif
