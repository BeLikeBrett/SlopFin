/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
// SlopFin MPEG-TS demux regressions. SPDX-License-Identifier: GPL-3.0-or-later
#include "tsdemux.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>

namespace
{
constexpr int kPmtPid = 0x100;
constexpr int kVideoPid = 0x101;
constexpr int kAudioPid = 0x102;

std::array<std::uint8_t, 188> packet(int pid, bool unit_start) noexcept
{
    std::array<std::uint8_t, 188> out{};
    out.fill(0xff);
    out[0] = 0x47;
    out[1] = static_cast<std::uint8_t>((unit_start ? 0x40 : 0x00) | ((pid >> 8) & 0x1f));
    out[2] = static_cast<std::uint8_t>(pid);
    out[3] = 0x10;
    return out;
}

void put_section_length(std::uint8_t *section, int length) noexcept
{
    section[1] = static_cast<std::uint8_t>(0xb0 | ((length >> 8) & 0x0f));
    section[2] = static_cast<std::uint8_t>(length);
}

std::array<std::uint8_t, 188> pat() noexcept
{
    auto out = packet(0, true);
    out[4] = 0;
    std::uint8_t *section = out.data() + 5;
    section[0] = 0x00;
    put_section_length(section, 13);
    section[3] = 0;
    section[4] = 1;
    section[5] = 0xc1;
    section[6] = 0;
    section[7] = 0;
    section[8] = 0;
    section[9] = 1;
    section[10] = static_cast<std::uint8_t>(0xe0 | (kPmtPid >> 8));
    section[11] = static_cast<std::uint8_t>(kPmtPid);
    return out;
}

std::array<std::uint8_t, 188> pmt(std::uint8_t audio_type) noexcept
{
    auto out = packet(kPmtPid, true);
    out[4] = 0;
    std::uint8_t *section = out.data() + 5;
    section[0] = 0x02;
    put_section_length(section, 23);
    section[3] = 0;
    section[4] = 1;
    section[5] = 0xc1;
    section[6] = 0;
    section[7] = 0;
    section[8] = static_cast<std::uint8_t>(0xe0 | (kVideoPid >> 8));
    section[9] = static_cast<std::uint8_t>(kVideoPid);
    section[10] = 0xf0;
    section[11] = 0;
    section[12] = 0x1b;
    section[13] = static_cast<std::uint8_t>(0xe0 | (kVideoPid >> 8));
    section[14] = static_cast<std::uint8_t>(kVideoPid);
    section[15] = 0xf0;
    section[16] = 0;
    section[17] = audio_type;
    section[18] = static_cast<std::uint8_t>(0xe0 | (kAudioPid >> 8));
    section[19] = static_cast<std::uint8_t>(kAudioPid);
    section[20] = 0xf0;
    section[21] = 0;
    return out;
}

void feed_tables(slopfin::ts::Demuxer &demuxer, std::uint8_t audio_type) noexcept
{
    const auto pat_packet = pat();
    const auto pmt_packet = pmt(audio_type);
    demuxer.feed(pat_packet.data(), pat_packet.size());
    demuxer.feed(pmt_packet.data(), pmt_packet.size());
}
} // namespace

int main()
{
    using slopfin::ts::Codec;

    slopfin::ts::Demuxer demuxer;
    feed_tables(demuxer, 0x0f);
    assert(demuxer.video_codec() == Codec::h264);
    assert(demuxer.audio_codec() == Codec::aac_adts);

    demuxer.reset();
    feed_tables(demuxer, 0x03);
    assert(demuxer.video_codec() == Codec::h264);
    assert(demuxer.audio_codec() == Codec::mp3);

    demuxer.reset();
    feed_tables(demuxer, 0x04);
    assert(demuxer.video_codec() == Codec::h264);
    assert(demuxer.audio_codec() == Codec::mp3);

    demuxer.reset();
    feed_tables(demuxer, 0x81);
    assert(demuxer.audio_codec() == Codec::ac3);

    for (const bool registration : {false, true})
    {
        demuxer.reset();
        auto p = pmt(0x06);
        auto *s = p.data() + 5;
        s[21] = registration ? 6 : 3;
        put_section_length(s, 23 + s[21]);
        s[22] = registration ? 0x05 : 0x6a;
        s[23] = registration ? 4 : 1;
        s[24] = registration ? 'A' : 0;
        if (registration)
        {
            s[25] = 'C';
            s[26] = '-';
            s[27] = '3';
        }
        const auto table = pat();
        demuxer.feed(table.data(), table.size());
        demuxer.feed(p.data(), p.size());
        assert(demuxer.audio_codec() == Codec::ac3);
        demuxer.reset();
        s[21] = 100;
        demuxer.feed(table.data(), table.size());
        demuxer.feed(p.data(), p.size());
        assert(demuxer.audio_codec() == Codec::unknown);
    }
    const std::pair<std::uint8_t, Codec> software_types[] = {
        {0x87, Codec::eac3}, {0x8a, Codec::dts}, {0x82, Codec::dts}, {0x83, Codec::truehd}};
    for (const auto &[type, expected] : software_types)
    {
        demuxer.reset();
        feed_tables(demuxer, type);
        assert(demuxer.audio_codec() == expected);
    }
    for (const auto &[tag, expected] : {std::pair{0x7a, Codec::eac3}, std::pair{0x7b, Codec::dts}})
    {
        demuxer.reset();
        auto p = pmt(0x06);
        auto *s = p.data() + 5;
        s[21] = 3;
        put_section_length(s, 26);
        s[22] = tag;
        s[23] = 1;
        s[24] = 0;
        const auto table = pat();
        demuxer.feed(table.data(), table.size());
        demuxer.feed(p.data(), p.size());
        assert(demuxer.audio_codec() == expected);
    }
    puts("MPEG-TS stream-type regressions passed");
}
