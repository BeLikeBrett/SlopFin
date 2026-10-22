/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Every row of the audio path table (docs/development/AUDIO_ROUTE.md), against
 * a real sink's capability list and synthetic ones built from it.
 */
#include "audio_route.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "audio_route_beyond_tv.inc"

using namespace slopfin::audio_route;

namespace
{
Sink beyond_tv()
{
    return parse_monitor_info(kBeyondTv, sizeof(kBeyondTv));
}

/* The same list with formats removed, as a simpler TV would report it. */
Sink without(Sink sink, std::initializer_list<Format> gone)
{
    Sink out = sink;
    out.count = 0;
    for (std::size_t i = 0; i < sink.count; ++i)
    {
        bool drop = false;
        for (Format f : gone)
            drop |= sink.entries[i].format == static_cast<std::uint8_t>(f);
        if (!drop)
            out.entries[out.count++] = sink.entries[i];
    }
    return out;
}

struct Row
{
    const char *codec;
    const char *profile;
    int channels;
    int rate;
    Path path;
    const char *target; /* server rows */
    int target_channels;
};

void check(const char *name, const Options &options, const std::vector<Row> &rows)
{
    for (const Row &row : rows)
    {
        const Route route = choose({row.codec, row.profile, row.channels, row.rate}, options);
        const bool ok = route.path == row.path &&
                        (row.path != Path::server ||
                         (route.target == row.target && route.channels == row.target_channels));
        if (!ok)
            std::fprintf(stderr, "%s: %s %s %dch %d Hz -> %s %s %d, expected %s %s %d\n", name,
                         row.codec, row.profile, row.channels, row.rate, path_name(route.path),
                         route.target.c_str(), route.channels, path_name(row.path), row.target,
                         row.target_channels);
        assert(ok);
    }
}
} // namespace

int main()
{
    /* The real list: eight descriptors, read exactly. */
    const Sink tv = beyond_tv();
    assert(tv.known && tv.count == 8);
    const Descriptor *ac3 = tv.find(Format::ac3);
    assert(ac3 && ac3->channels == 6 && ac3->extra == 80 && ac3->rates == 0x07); /* 640 kbps */
    const Descriptor *dts = tv.find(Format::dts);
    assert(dts && dts->channels == 6 && dts->extra == 192); /* 1536 kbps */
    assert(tv.find(Format::lpcm)->channels == 2);
    assert(tv.find(Format::eac3)->channels == 8 && tv.atmos());
    assert(tv.find(Format::mat)->rates == 0x04); /* TrueHD at 48 kHz only */
    assert(tv.find(Format::dts_hd)->rates == 0x7e);
    assert(tv.find(Format::aac)->extra == 40); /* 320 kbps */

    /* Damaged or foreign buffers are unknown, never half-read. */
    std::vector<unsigned char> broken(kBeyondTv, kBeyondTv + sizeof(kBeyondTv));
    broken[0xa0 + 8 * 3 + 1] = 9; /* nine channels */
    assert(!parse_monitor_info(broken.data(), broken.size()).known);
    broken.assign(kBeyondTv, kBeyondTv + sizeof(kBeyondTv));
    broken[0x98] = 0;
    assert(!parse_monitor_info(broken.data(), broken.size()).known);
    assert(!parse_monitor_info(kBeyondTv, 0x90).known);
    assert(!parse_monitor_info(nullptr, 0).known);

    Options receiver;
    receiver.software = true;
    receiver.sink = tv;
    check("receiver, CPU decoding on", receiver,
          {
              {"aac", "", 2, 48000, Path::console, "", 0},
              {"aac", "", 6, 48000, Path::console, "", 0},
              {"mp3", "", 2, 48000, Path::console, "", 0},
              {"ac3", "", 6, 48000, Path::passthrough, "", 0},
              {"eac3", "", 8, 48000, Path::passthrough, "", 0},
              {"dts", "DTS", 6, 48000, Path::passthrough, "", 0},
              {"dts", "DTS-HD MA", 8, 48000, Path::passthrough, "", 0},
              {"truehd", "", 8, 48000, Path::software, "", 0},
              {"flac", "", 6, 48000, Path::server, "eac3", 6},
              {"flac", "", 2, 48000, Path::server, "aac", 2},
              {"opus", "", 2, 48000, Path::server, "aac", 2},
              {"aac", "", 2, 44100, Path::server, "aac", 2},
              {"ac3", "", 6, 44100, Path::server, "eac3", 6},
          });

    /* A TV on its own: stereo PCM and nothing else. */
    Options tv_only = receiver;
    tv_only.sink = without(
        tv, {Format::ac3, Format::eac3, Format::dts, Format::dts_hd, Format::mat, Format::aac});
    check("stereo TV, CPU decoding on", tv_only,
          {
              {"ac3", "", 6, 48000, Path::console, "", 0},
              {"eac3", "", 8, 48000, Path::software, "", 0},
              {"dts", "DTS", 6, 48000, Path::software, "", 0},
              {"dts", "DTS-HD MA", 8, 48000, Path::server, "ac3", 6},
              {"truehd", "", 8, 48000, Path::software, "", 0},
              {"flac", "", 6, 48000, Path::server, "ac3", 6},
          });

    /* A TV with Dolby but no DTS: DTS never goes to HDMI. */
    Options no_dts = receiver;
    no_dts.software = false;
    no_dts.sink = without(tv, {Format::dts, Format::dts_hd});
    check("Dolby-only TV, CPU decoding off", no_dts,
          {
              {"dts", "DTS", 6, 48000, Path::server, "eac3", 6},
              {"dts", "DTS-HD MA", 8, 48000, Path::server, "eac3", 6},
              {"eac3", "", 6, 48000, Path::passthrough, "", 0},
              {"truehd", "", 8, 48000, Path::server, "eac3", 6},
          });

    /* No list from the console: the passthrough setting is trusted. */
    Options unknown;
    unknown.software = true;
    check("unknown sink", unknown,
          {
              {"dts", "DTS", 6, 48000, Path::passthrough, "", 0},
              {"truehd", "", 8, 48000, Path::software, "", 0},
              {"flac", "", 6, 48000, Path::server, "eac3", 6},
          });

    /* Passthrough off: decode everything here or convert. */
    Options decode = receiver;
    decode.passthrough = false;
    check("passthrough off", decode,
          {
              {"ac3", "", 6, 48000, Path::console, "", 0},
              {"eac3", "", 8, 48000, Path::software, "", 0},
              {"dts", "DTS", 6, 48000, Path::software, "", 0},
              {"truehd", "", 8, 48000, Path::software, "", 0},
              {"flac", "", 6, 48000, Path::server, "ac3", 6},
          });

    /* What failed for this title comes off the ladder. */
    Options failed = receiver;
    failed.passthrough_failed = true;
    check("passthrough failed", failed,
          {
              {"eac3", "", 8, 48000, Path::software, "", 0},
              {"ac3", "", 6, 48000, Path::console, "", 0},
              {"truehd", "", 8, 48000, Path::software, "", 0},
          });
    failed.software_failed = true;
    check("passthrough and CPU decoding failed", failed,
          {
              {"eac3", "", 8, 48000, Path::server, "ac3", 6},
              {"truehd", "", 8, 48000, Path::server, "ac3", 6},
              {"dts", "DTS", 6, 48000, Path::server, "ac3", 6},
          });

    /* A conversion chosen by hand, and a channel limit, are kept. */
    Options chosen = receiver;
    chosen.conversion = "ac3";
    check("AC-3 chosen", chosen,
          {
              {"eac3", "", 8, 48000, Path::server, "ac3", 6},
              {"truehd", "", 8, 48000, Path::server, "ac3", 6},
              {"ac3", "", 6, 48000, Path::passthrough, "", 0},
          });
    Options stereo = receiver;
    stereo.channel_limit = 2;
    check("stereo limit", stereo,
          {
              {"aac", "", 2, 48000, Path::console, "", 0},
              {"ac3", "", 6, 48000, Path::server, "aac", 2},
              {"truehd", "", 8, 48000, Path::server, "aac", 2},
          });

    std::puts("audio route table passed");
    return 0;
}
