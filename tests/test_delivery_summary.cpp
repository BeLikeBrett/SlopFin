/*
 * SlopFin - host tests for "On this PS5".
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Each case is a delivery observed on the TV on 2026-09-14, with the server's
 * reasons and what the live session then reported (Jellyfin's own
 * TranscodingInfo). The summary must agree with what actually happened.
 */

#include "delivery_summary.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;

std::string value(const std::vector<slopfin::details::Line> &lines, const std::string &key)
{
    for (const auto &line : lines)
        if (line.key == key)
            return line.value;
    return "<missing>";
}

void contains(const std::string &got, const std::string &want, const std::string &what)
{
    if (got.find(want) != std::string::npos)
        return;
    std::printf("FAIL: %s: \"%s\" does not contain \"%s\"\n", what.c_str(), got.c_str(),
                want.c_str());
    ++g_failures;
}

slopfin::jellyfin::PlaybackPlan plan(const std::string &query)
{
    slopfin::jellyfin::PlaybackPlan out;
    out.valid = true;
    out.path = "/videos/x/stream.ts?" + query;
    return out;
}
} // namespace

int main()
{
    using slopfin::delivery::describe;

    /* Backrooms, default track: live session said video hevc copy, dts -> aac. */
    {
        auto p = plan("VideoCodec=hevc&AudioCodec=aac&TranscodeReasons=ContainerNotSupported,"
                      "AudioCodecNotSupported");
        p.video_codec = "hevc";
        p.audio_codec = "dts";
        p.audio_profile = "DTS-HD MA";
        p.dv_hdr10_base = true;
        const auto lines = describe(p, -1);
        contains(value(lines, "Plays as"), "audio converted", "Backrooms: how");
        contains(value(lines, "Video"), "Copied (HEVC)", "Backrooms: video copied");
        contains(value(lines, "Video"), "HDR10 base", "Backrooms: DV base layer");
        contains(value(lines, "Audio"), "DTS-HD MA converted to AAC", "Backrooms: audio");
        contains(value(lines, "Why"), "repackaged", "Backrooms: first reason");
    }

    /* Backrooms at a 40 Mbps cap: live session said hevc_nvenc, reason
     * ContainerBitrateExceedsLimit. */
    {
        auto p =
            plan("VideoCodec=hevc&AudioCodec=aac&TranscodeReasons=ContainerBitrateExceedsLimit");
        p.video_codec = "hevc";
        p.audio_codec = "dts";
        const auto lines = describe(p, -1);
        contains(value(lines, "Plays as"), "encoded again", "40 Mbps cap: how");
        contains(value(lines, "Video"), "Encoded again", "40 Mbps cap: video");
        contains(value(lines, "Why"), "above the quality setting", "40 Mbps cap: reason");
    }

    /* Avatar on the SOFTWARE_AUDIO build: live session said hevc copy, truehd copy. */
    {
        auto p = plan("VideoCodec=hevc&AudioCodec=truehd&TranscodeReasons=ContainerNotSupported");
        p.video_codec = "hevc";
        p.audio_codec = "truehd";
        p.subtitle_url = "/Videos/a/b/Subtitles/1/0/Stream.srt";
        const auto lines = describe(p, 1);
        contains(value(lines, "Plays as"), "nothing is re-encoded", "Avatar: how");
        contains(value(lines, "Audio"), "Copied (TrueHD)", "Avatar: audio copied");
        contains(value(lines, "Audio"), "decoded on this PS5", "Avatar: CPU decode");
        contains(value(lines, "Subtitles"), "drawn by SlopFin", "Avatar: text subtitles");
    }

    /* A picture subtitle chosen: the server burns it in, which encodes the video. */
    {
        auto p = plan("VideoCodec=hevc&AudioCodec=aac&TranscodeReasons=ContainerNotSupported,"
                      "SubtitleCodecNotSupported");
        p.video_codec = "hevc";
        p.audio_codec = "ac3";
        p.subtitle_burned = true;
        const auto lines = describe(p, 3);
        contains(value(lines, "Plays as"), "encoded again", "PGS: how");
        contains(value(lines, "Subtitles"), "into the picture", "PGS: subtitles");
    }

    {
        slopfin::jellyfin::PlaybackPlan failed;
        contains(value(describe(failed, -1), "Server"), "Could not",
                 "a failed negotiation says so");
    }

    if (g_failures == 0)
        std::printf("delivery summary: all checks passed\n");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
