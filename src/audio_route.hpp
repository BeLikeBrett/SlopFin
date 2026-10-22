/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Which path a sound track takes, decided in one place.
 *
 * The negotiation with the server, the direct-play check and the audio output
 * all ask choose(), so they cannot disagree. The answer depends only on the
 * track, the settings, what already failed for this title, and what the TV or
 * receiver says it accepts. The full table is in docs/development/AUDIO_ROUTE.md
 * and every row is a test in tests/test_audio_route.cpp.
 *
 * The TV's list comes from sceAudioOutExGetMonitorInfo: the console's own
 * parse of the HDMI sink's audio descriptors (CTA-861 short audio
 * descriptors), eight bytes each. Without it the passthrough setting is
 * trusted, as before.
 *
 * Free of PS5 headers so it can be tested on a workstation.
 */
#ifndef SLOPFIN_AUDIO_ROUTE_HPP
#define SLOPFIN_AUDIO_ROUTE_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace slopfin::audio_route
{

/* CTA-861 audio format codes. */
enum class Format : std::uint8_t
{
    lpcm = 1,
    ac3 = 2,
    aac = 6,
    dts = 7,
    eac3 = 10,
    dts_hd = 11,
    mat = 12, /* Dolby TrueHD / MAT */
};

struct Descriptor
{
    std::uint8_t format = 0;
    std::uint8_t channels = 0;
    std::uint16_t extra = 0; /* the descriptor's third byte: bitrate/8k, depths, Atmos bit */
    std::uint32_t rates = 0; /* bit 0 32 kHz, 1 44.1, 2 48, 3 88.2, 4 96, 5 176.4, 6 192 */
};

/* What the HDMI sink accepts, as the console reported it. */
struct Sink
{
    bool known = false;
    std::size_t count = 0;
    std::array<Descriptor, 16> entries{};

    [[nodiscard]] const Descriptor *find(Format format) const noexcept
    {
        for (std::size_t i = 0; i < count; ++i)
            if (entries[i].format == static_cast<std::uint8_t>(format))
                return &entries[i];
        return nullptr;
    }
    [[nodiscard]] bool takes(Format format) const noexcept
    {
        const Descriptor *d = find(format);
        return d != nullptr && (d->rates & 4u) != 0; /* 48 kHz: the only rate sent */
    }
    /* E-AC-3 with joint object coding: Dolby Atmos reaches the speakers. */
    [[nodiscard]] bool atmos() const noexcept
    {
        const Descriptor *d = find(Format::eac3);
        return d != nullptr && (d->extra & 1u) != 0;
    }
};

/*
 * sceAudioOutExGetMonitorInfo(0, 0, buffer, 0x400). Layout measured on one
 * console (firmware 8.20, report 2026-10-22): a descriptor count at 0x98 and
 * eight-byte descriptors from 0xa0 -- format, channels, a 16-bit third byte,
 * a 32-bit rate mask. The bitrate limits it gave (AC-3 640 kbps, DTS 1536
 * kbps) are those formats' own maximums, which is what pinned the layout.
 * Anything out of range makes the whole list unknown rather than half-read.
 */
inline Sink parse_monitor_info(const std::uint8_t *data, std::size_t size) noexcept
{
    Sink sink;
    if (data == nullptr || size < 0xa0)
        return sink;
    const std::uint32_t count =
        static_cast<std::uint32_t>(data[0x98]) | static_cast<std::uint32_t>(data[0x99]) << 8 |
        static_cast<std::uint32_t>(data[0x9a]) << 16 | static_cast<std::uint32_t>(data[0x9b]) << 24;
    if (count == 0 || count > sink.entries.size() || 0xa0 + count * 8 > size)
        return sink;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const std::uint8_t *at = data + 0xa0 + i * 8;
        Descriptor d;
        d.format = at[0];
        d.channels = at[1];
        d.extra = static_cast<std::uint16_t>(at[2] | at[3] << 8);
        d.rates = static_cast<std::uint32_t>(at[4]) | static_cast<std::uint32_t>(at[5]) << 8 |
                  static_cast<std::uint32_t>(at[6]) << 16 | static_cast<std::uint32_t>(at[7]) << 24;
        if (d.format < 1 || d.format > 15 || d.channels < 1 || d.channels > 8 || d.rates == 0 ||
            d.rates > 0x7f)
            return Sink{};
        sink.entries[i] = d;
    }
    sink.count = count;
    sink.known = true;
    return sink;
}

/* The list as a short tag line for Settings: "AC-3 · E-AC-3 Atmos · TrueHD ·
   DTS · DTS-HD · PCM 7.1". Empty when the console gave none. */
inline std::string describe(const Sink &sink)
{
    if (!sink.known)
        return {};
    std::string out;
    const auto add = [&out](const std::string &name)
    {
        if (out.find(name) == std::string::npos)
            out += (out.empty() ? "" : " \xc2\xb7 ") + name;
    };
    for (std::size_t i = 0; i < sink.count; ++i)
    {
        const auto f = static_cast<Format>(sink.entries[i].format);
        if (f == Format::ac3)
            add("AC-3");
        else if (f == Format::eac3)
            add(sink.atmos() ? "E-AC-3 Atmos" : "E-AC-3");
        else if (f == Format::mat)
            add("TrueHD");
        else if (f == Format::dts)
            add("DTS");
        else if (f == Format::dts_hd)
            add("DTS-HD");
    }
    if (const Descriptor *pcm = sink.find(Format::lpcm); pcm != nullptr)
        add(pcm->channels >= 8 ? "PCM 7.1" : pcm->channels >= 6 ? "PCM 5.1" : "Stereo PCM");
    return out;
}

/* A track as Jellyfin describes it: codec "aac", "ac3", "eac3", "dts",
   "truehd", "flac"...; for DTS, the profile names the core or an extension. */
struct Track
{
    std::string codec;
    std::string profile;
    int channels = 2;
    int sample_rate = 48000;
};

struct Options
{
    bool passthrough = true;         /* Settings: HDMI audio passthrough */
    bool software = false;           /* Settings: decode on PS5, and the build has it */
    bool passthrough_failed = false; /* this title: the HDMI port refused it */
    bool software_failed = false;    /* this title: the CPU decoder failed */
    int channel_limit = 0;           /* 0: none */
    std::string conversion;          /* a format chosen in the Audio panel, or empty */
    Sink sink;
};

enum class Path : std::uint8_t
{
    console,     /* the console's own decoder: AAC, MP3, AC-3 */
    passthrough, /* compressed to the TV or receiver, which decodes it */
    software,    /* the PS5 CPU decodes it (FFmpeg) */
    server,      /* Jellyfin converts it to `target` first */
};

struct Route
{
    Path path = Path::server;
    std::string target;   /* server: what to convert to */
    int channels = 0;     /* server: the most channels to ask for */
    const char *why = ""; /* one line for the playback information panel */
};

inline const char *path_name(Path path) noexcept
{
    return path == Path::console       ? "PS5 decoder"
           : path == Path::passthrough ? "HDMI passthrough"
           : path == Path::software    ? "PS5 CPU decoder"
                                       : "server conversion";
}

namespace detail
{
inline Format sink_format(const std::string &codec) noexcept
{
    return codec == "ac3" ? Format::ac3 : codec == "eac3" ? Format::eac3 : Format::dts;
}

inline bool sink_takes(const Options &o, const std::string &codec) noexcept
{
    if (!o.sink.known)
        return true; /* no list: trust the setting, as before the list was read */
    if (codec == "dts")
        return o.sink.takes(Format::dts) || o.sink.takes(Format::dts_hd);
    return o.sink.takes(sink_format(codec));
}
} // namespace detail

/*
 * The best path, in this order:
 *   1. passthrough, for AC-3, E-AC-3 and DTS the sink lists (DTS: its core);
 *   2. the console's decoder, for AAC, MP3 (stereo) and AC-3 (5.1);
 *   3. the CPU decoder, for E-AC-3, TrueHD and DTS core, up to 7.1 (16-bit PCM);
 *   4. server conversion, to E-AC-3 when the sink takes it, else AC-3 for
 *      surround or AAC for stereo -- all of which rows 1 or 2 can play.
 * A failure for this title removes its row; a conversion chosen by hand, a
 * channel limit below the track, or a rate other than 48 kHz goes to row 4.
 */
inline Route choose(const Track &track, const Options &o) noexcept
{
    const std::string &codec = track.codec;
    const int limit = o.channel_limit > 0 ? o.channel_limit : 8;
    const bool rate_ok = track.sample_rate == 48000;
    const bool chosen = !o.conversion.empty() && o.conversion != codec;
    const bool within = track.channels >= 1 && track.channels <= limit;

    if (rate_ok && within && !chosen)
    {
        if ((codec == "ac3" || codec == "eac3" || codec == "dts") && o.passthrough &&
            !o.passthrough_failed && detail::sink_takes(o, codec))
            return {Path::passthrough,
                    {},
                    0,
                    o.sink.known ? "the TV or receiver lists this format"
                                 : "passthrough on; the TV's formats are unknown"};
        if (codec == "aac" || (codec == "mp3" && track.channels <= 2) ||
            (codec == "ac3" && track.channels <= 6))
            return {Path::console, {}, 0, "decoded by the PS5"};
        const bool dts_core = codec == "dts" && track.profile == "DTS";
        if ((codec == "eac3" || codec == "truehd" || dts_core) && o.software &&
            !o.software_failed && track.channels <= 8)
            return {Path::software, {}, 0, "decoded by the PS5 CPU"};
    }

    /* The server converts to something one of the rows above plays. */
    Route route;
    route.path = Path::server;
    const bool eac3_out = o.passthrough && !o.passthrough_failed && detail::sink_takes(o, "eac3");
    if (chosen)
        route.target = o.conversion;
    else if (track.channels > 2 && limit > 2)
        route.target = eac3_out ? "eac3" : "ac3";
    else
        route.target = "aac";
    const bool dolby = route.target == "ac3" || route.target == "eac3";
    route.channels = std::min({std::max(1, track.channels), limit, dolby ? 6 : 8});
    route.why = chosen     ? "the format chosen in the Audio panel"
                : !rate_ok ? "the PS5 plays sound at 48 kHz"
                : !within  ? "more channels than the limit"
                           : "no path on this PS5 plays this format";
    return route;
}

} // namespace slopfin::audio_route

#endif
