/*
 * SlopFin - the Details sheet: what the file on the server actually is.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Everything here comes from the MediaSources the detail request already
 * fetches, so opening the sheet costs no network. Formatting lives in a header
 * so the numbers a viewer reads -- sizes, rates, levels -- are tested against
 * real files rather than eyeballed.
 */

#ifndef SLOPFIN_DETAILS_SHEET_HPP
#define SLOPFIN_DETAILS_SHEET_HPP

#include "jellyfin.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace slopfin::details
{

struct Line
{
    enum class Kind : unsigned char
    {
        heading,
        pair,
    };
    Kind kind = Kind::pair;
    std::string key;
    std::string value;
};

/* 77884008652 -> "77,884,008,652". */
inline std::string grouped(long long value)
{
    std::string digits = std::to_string(value < 0 ? -value : value);
    for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3)
        digits.insert(static_cast<std::size_t>(at), ",");
    return value < 0 ? "-" + digits : digits;
}

/* Decimal units, as the server and most file managers count them. */
inline std::string size_text(long long bytes)
{
    char text[64];
    if (bytes >= 1000000000LL)
        std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / 1e9);
    else if (bytes >= 1000000LL)
        std::snprintf(text, sizeof(text), "%.0f MB", static_cast<double>(bytes) / 1e6);
    else
        std::snprintf(text, sizeof(text), "%lld KB", bytes / 1000LL);
    return text;
}

inline std::string rate_text(long long bits_per_second)
{
    char text[32];
    if (bits_per_second >= 1000000LL)
        std::snprintf(text, sizeof(text), "%.1f Mbps", static_cast<double>(bits_per_second) / 1e6);
    else
        std::snprintf(text, sizeof(text), "%lld kbps", bits_per_second / 1000LL);
    return text;
}

inline std::string runtime_text(int minutes)
{
    if (minutes <= 0)
        return {};
    if (minutes < 60)
        return std::to_string(minutes) + " min";
    return std::to_string(minutes / 60) + " h " + std::to_string(minutes % 60) + " min";
}

/* 23.976 stays 23.976; 24.0 reads 24. */
inline std::string frame_rate_text(double fps)
{
    if (fps <= 0.0)
        return {};
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", fps);
    std::string out = text;
    while (!out.empty() && out.back() == '0')
        out.pop_back();
    if (!out.empty() && out.back() == '.')
        out.pop_back();
    return out + " fps";
}

inline std::string codec_name(const std::string &codec)
{
    if (codec == "hevc")
        return "HEVC";
    if (codec == "h264")
        return "H.264";
    if (codec == "av1")
        return "AV1";
    if (codec == "vp9")
        return "VP9";
    if (codec == "mpeg2video")
        return "MPEG-2";
    if (codec == "vc1")
        return "VC-1";
    if (codec == "truehd")
        return "TrueHD";
    if (codec == "eac3")
        return "E-AC-3";
    if (codec == "ac3")
        return "AC-3";
    if (codec == "aac")
        return "AAC";
    std::string upper = codec;
    for (char &c : upper)
        c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
    return upper;
}

/* The server reports HEVC levels times thirty (153 is 5.1) and H.264 levels
   times ten (41 is 4.1). */
inline std::string level_text(const std::string &codec, double level)
{
    if (level <= 0.0)
        return {};
    const double scaled = codec == "hevc" ? level / 30.0 : codec == "h264" ? level / 10.0 : level;
    char text[32];
    std::snprintf(text, sizeof(text), "Level %.1f", scaled);
    return text;
}

inline std::string range_text(const std::string &range, const std::string &dovi)
{
    if (!dovi.empty())
        return dovi;
    if (range.rfind("DOVI", 0) == 0)
        return "Dolby Vision";
    if (range == "HDR10Plus")
        return "HDR10+";
    return range; /* SDR, HDR10, HLG as the server names them */
}

/* "2026-09-04T18:29:42.0000000Z" -> "Sep 4, 2026". */
inline std::string date_text(const std::string &iso)
{
    static constexpr const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int year = 0, month = 0, day = 0;
    if (std::sscanf(iso.c_str(), "%d-%d-%d", &year, &month, &day) != 3 || month < 1 || month > 12)
        return {};
    return std::string{kMonths[month - 1]} + " " + std::to_string(day) + ", " +
           std::to_string(year);
}

inline void join(std::string &into, const std::string &part)
{
    if (part.empty())
        return;
    if (!into.empty())
        into += "  \xc2\xb7  ";
    into += part;
}

inline std::vector<Line> build(const jellyfin::Item &item, const jellyfin::MediaSource &source)
{
    std::vector<Line> out;
    const auto heading = [&out](std::string title)
    { out.push_back({Line::Kind::heading, std::move(title), {}}); };
    const auto pair = [&out](std::string key, std::string value)
    {
        if (!value.empty())
            out.push_back({Line::Kind::pair, std::move(key), std::move(value)});
    };

    heading("File");
    const std::size_t slash = source.path.find_last_of('/');
    pair("Name", slash == std::string::npos ? source.path : source.path.substr(slash + 1));
    pair("Folder", slash == std::string::npos ? std::string{} : source.path.substr(0, slash));
    pair("Format", codec_name(source.container));
    if (source.size_bytes > 0)
        pair("Size",
             size_text(source.size_bytes) + "   (" + grouped(source.size_bytes) + " bytes)");
    pair("Length", runtime_text(item.runtime_minutes));
    if (source.bitrate > 0)
        pair("Bitrate", rate_text(source.bitrate));
    pair("Added", date_text(item.date_created));

    heading("Video");
    std::string codec = codec_name(source.video_codec);
    join(codec, source.video_profile);
    join(codec, level_text(source.video_codec, source.video_level));
    pair("Codec", codec);
    std::string frame;
    if (source.width > 0)
        frame = std::to_string(source.width) + " \xc3\x97 " + std::to_string(source.height);
    join(frame, source.aspect_ratio);
    pair("Resolution", frame);
    pair("Frame rate", frame_rate_text(source.frame_rate));
    std::string colour =
        source.video_bit_depth > 0 ? std::to_string(source.video_bit_depth) + "-bit" : "";
    join(colour, range_text(source.video_range, source.video_dovi));
    pair("Colour", colour);
    if (source.video_bitrate > 0)
        pair("Bitrate", rate_text(source.video_bitrate));

    const auto tracks = [&](const char *title, const std::vector<jellyfin::Track> &list)
    {
        heading(std::string{title} + " (" + std::to_string(list.size()) + ")");
        if (list.empty())
            pair(" ", "None");
        for (std::size_t i = 0; i < list.size(); ++i)
        {
            const jellyfin::Track &track = list[i];
            std::string value = track.label.empty() ? track.language : track.label;
            if (track.bitrate > 0)
                join(value, rate_text(track.bitrate));
            pair(std::to_string(i + 1), value);
        }
    };
    tracks("Audio", source.audio_tracks);
    tracks("Subtitles", source.subtitle_tracks);
    return out;
}

} // namespace slopfin::details

#endif
