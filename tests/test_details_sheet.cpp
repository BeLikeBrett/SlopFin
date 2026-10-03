/*
 * SlopFin - host tests for the Details sheet's formatting.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The expected strings are what the source server reports for Backrooms (2026),
 * checked by hand against the API response and ffprobe, not produced by the
 * code under test.
 */

#include "details_sheet.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;

void same(const std::string &got, const std::string &want, const std::string &what)
{
    if (got == want)
        return;
    std::printf("FAIL: %s: got \"%s\", want \"%s\"\n", what.c_str(), got.c_str(), want.c_str());
    ++g_failures;
}

std::string value_of(const std::vector<slopfin::details::Line> &lines, const std::string &section,
                     const std::string &key)
{
    bool inside = false;
    for (const auto &line : lines)
    {
        if (line.kind == slopfin::details::Line::Kind::heading)
            inside = line.key.rfind(section, 0) == 0;
        else if (inside && line.key == key)
            return line.value;
    }
    return "<missing>";
}
} // namespace

int main()
{
    using namespace slopfin::details;

    same(grouped(77884008652LL), "77,884,008,652", "thousands separators");
    same(size_text(77884008652LL), "77.9 GB", "a 4K remux");
    same(size_text(2103952677LL), "2.1 GB", "a 1080p episode");
    same(size_text(645000000LL), "645 MB", "under a gigabyte");
    same(rate_text(103490076LL), "103.5 Mbps", "overall bitrate");
    same(rate_text(640000LL), "640 kbps", "an audio track");
    same(runtime_text(111), "1 h 51 min", "a film");
    same(runtime_text(45), "45 min", "an episode");
    same(frame_rate_text(23.976023864746094), "23.976 fps", "film rate");
    same(frame_rate_text(24.0), "24 fps", "whole rate");
    same(level_text("hevc", 153.0), "Level 5.1", "HEVC level is reported times thirty");
    same(level_text("h264", 41.0), "Level 4.1", "H.264 level is reported times ten");
    same(range_text("DOVIWithEL", "Dolby Vision Profile 7.6 (HDR10)"),
         "Dolby Vision Profile 7.6 (HDR10)", "the server's own DV title wins");
    same(range_text("HDR10Plus", ""), "HDR10+", "HDR10+");
    same(date_text("2026-09-04T18:29:42.0000000Z"), "Sep 4, 2026", "date added");

    slopfin::jellyfin::Item item;
    item.runtime_minutes = 111;
    item.date_created = "2026-09-04T18:29:42.0000000Z";
    slopfin::jellyfin::MediaSource source;
    source.path = "/hdd4tb/Media/Movies/Backrooms (2026)/Backrooms (2026) Remux-2160p.mkv";
    source.container = "mkv";
    source.size_bytes = 77884008652LL;
    source.bitrate = 103490076LL;
    source.video_codec = "hevc";
    source.video_profile = "Main 10";
    source.video_level = 153.0;
    source.width = 3840;
    source.height = 2160;
    source.aspect_ratio = "16:9";
    source.frame_rate = 23.976023864746094;
    source.video_bit_depth = 10;
    source.video_range = "DOVIWithEL";
    source.video_dovi = "Dolby Vision Profile 7.6 (HDR10)";
    slopfin::jellyfin::Track truehd;
    truehd.label = "English - DTS-HD MA - 5.1 - Default";
    source.audio_tracks.push_back(truehd);

    const auto lines = build(item, source);
    same(value_of(lines, "File", "Name"), "Backrooms (2026) Remux-2160p.mkv", "file name");
    same(value_of(lines, "File", "Folder"), "/hdd4tb/Media/Movies/Backrooms (2026)", "folder");
    same(value_of(lines, "File", "Size"), "77.9 GB   (77,884,008,652 bytes)", "size line");
    same(value_of(lines, "Video", "Codec"), "HEVC  \xc2\xb7  Main 10  \xc2\xb7  Level 5.1",
         "codec line");
    same(value_of(lines, "Video", "Resolution"), "3840 \xc3\x97 2160  \xc2\xb7  16:9",
         "resolution line");
    same(value_of(lines, "Video", "Colour"), "10-bit  \xc2\xb7  Dolby Vision Profile 7.6 (HDR10)",
         "colour line");
    same(value_of(lines, "Audio", "1"), "English - DTS-HD MA - 5.1 - Default", "audio track");
    same(value_of(lines, "Subtitles", " "), "None", "no subtitles says so");

    if (g_failures == 0)
        std::printf("details sheet: all checks passed\n");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
