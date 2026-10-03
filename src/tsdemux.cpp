/*
 * SlopFin - MPEG-TS demultiplexer.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tsdemux.hpp"

#include <cstring>

namespace
{
constexpr std::size_t kPacketBytes = 188;
constexpr std::uint8_t kSyncByte = 0x47;

/* Stream types from the PMT that this client knows how to decode. */
constexpr std::uint8_t kStreamTypeH264 = 0x1b;
constexpr std::uint8_t kStreamTypeHevc = 0x24;
constexpr std::uint8_t kStreamTypeAacAdts = 0x0f;
constexpr std::uint8_t kStreamTypeMpeg1Audio = 0x03;
constexpr std::uint8_t kStreamTypeMpeg2Audio = 0x04;

/* A 33-bit timestamp spread across five bytes with marker bits between. */
std::int64_t read_timestamp(const std::uint8_t *bytes) noexcept
{
    return (static_cast<std::int64_t>(bytes[0] & 0x0e) << 29) |
           (static_cast<std::int64_t>(bytes[1]) << 22) |
           (static_cast<std::int64_t>(bytes[2] & 0xfe) << 14) |
           (static_cast<std::int64_t>(bytes[3]) << 7) | (static_cast<std::int64_t>(bytes[4]) >> 1);
}
} // namespace

namespace slopfin::ts
{

void Demuxer::reset() noexcept
{
    spare_.clear();
    ready_.clear();
    pmt_pid_ = -1;
    video_pid_ = -1;
    audio_pid_ = -1;
    video_codec_ = Codec::unknown;
    audio_codec_ = Codec::unknown;
    video_ = Stream{};
    audio_ = Stream{};
}

void Demuxer::feed(const std::uint8_t *bytes, std::size_t count) noexcept
{
    if (bytes == nullptr || count == 0)
        return;

    /* Work from the leftovers plus the new bytes as one run. */
    spare_.insert(spare_.end(), bytes, bytes + count);

    std::size_t cursor = 0;
    while (cursor + kPacketBytes <= spare_.size())
    {
        if (spare_[cursor] != kSyncByte)
        {
            /* Resynchronise: step to the next plausible sync byte. */
            ++cursor;
            continue;
        }
        consume_packet(spare_.data() + cursor);
        cursor += kPacketBytes;
    }

    if (cursor > 0)
        spare_.erase(spare_.begin(), spare_.begin() + static_cast<std::ptrdiff_t>(cursor));

    /* Never let an unsynchronised run grow without bound. */
    if (spare_.size() > kPacketBytes * 64)
        spare_.erase(spare_.begin(), spare_.end() - static_cast<std::ptrdiff_t>(kPacketBytes * 2));
}

void Demuxer::consume_packet(const std::uint8_t *packet) noexcept
{
    const bool unit_start = (packet[1] & 0x40) != 0;
    const int pid = ((packet[1] & 0x1f) << 8) | packet[2];
    const std::uint8_t adaptation = (packet[3] >> 4) & 0x03;
    if (adaptation == 0 || adaptation == 2)
        return; /* no payload */

    std::size_t offset = 4;
    if (adaptation == 3)
    {
        const std::size_t adaptation_length = packet[4];
        offset += 1 + adaptation_length;
        if (offset >= kPacketBytes)
            return;
    }

    const std::uint8_t *payload = packet + offset;
    const std::size_t length = kPacketBytes - offset;

    if (pid == 0)
    {
        if (unit_start && length > 1 && payload[0] < length - 1)
            parse_pat(payload + 1 + payload[0], length - 1 - payload[0]);
        return;
    }
    if (pid == pmt_pid_)
    {
        if (unit_start && length > 1 && payload[0] < length - 1)
            parse_pmt(payload + 1 + payload[0], length - 1 - payload[0]);
        return;
    }
    if (pid == video_pid_)
    {
        push_payload(video_, true, payload, length, unit_start);
        return;
    }
    if (pid == audio_pid_)
        push_payload(audio_, false, payload, length, unit_start);
}

void Demuxer::parse_pat(const std::uint8_t *section, std::size_t length) noexcept
{
    if (length < 12 || section[0] != 0x00)
        return;
    const std::size_t section_length = ((section[1] & 0x0f) << 8) | section[2];
    if (section_length + 3 > length)
        return;
    /* Entries start after the 8-byte header and stop before the 4-byte CRC. */
    for (std::size_t at = 8; at + 4 <= section_length + 3 - 4; at += 4)
    {
        const int program = (section[at] << 8) | section[at + 1];
        const int pid = ((section[at + 2] & 0x1f) << 8) | section[at + 3];
        if (program != 0)
        {
            pmt_pid_ = pid;
            return;
        }
    }
}

void Demuxer::parse_pmt(const std::uint8_t *section, std::size_t length) noexcept
{
    if (length < 16 || section[0] != 0x02)
        return;
    const std::size_t section_length = ((section[1] & 0x0f) << 8) | section[2];
    if (section_length + 3 > length)
        return;
    const std::size_t program_info_length = ((section[10] & 0x0f) << 8) | section[11];
    std::size_t at = 12 + program_info_length;
    const std::size_t end = section_length + 3 - 4;

    while (at + 5 <= end)
    {
        const std::uint8_t stream_type = section[at];
        const int pid = ((section[at + 1] & 0x1f) << 8) | section[at + 2];
        const std::size_t info_length = ((section[at + 3] & 0x0f) << 8) | section[at + 4];
        if (info_length > end - at - 5)
            return;
        bool ac3 = stream_type == 0x81;
        bool eac3 = stream_type == 0x87;
        bool dts = stream_type == 0x8a || stream_type == 0x82;
        const bool truehd = stream_type == 0x83;

        if (stream_type == 0x06)
        {
            for (std::size_t d = at + 5; d + 2 <= at + 5 + info_length;)
            {
                const std::size_t size = section[d + 1];
                if (size > at + 5 + info_length - d - 2)
                    return;
                ac3 |= section[d] == 0x6a || (section[d] == 0x05 && size >= 4 &&
                                              std::memcmp(section + d + 2, "AC-3", 4) == 0);
                eac3 |= section[d] == 0x7a || (section[d] == 0x05 && size >= 4 &&
                                               std::memcmp(section + d + 2, "EAC3", 4) == 0);
                dts |= section[d] == 0x7b || (section[d] == 0x05 && size >= 4 &&
                                              (std::memcmp(section + d + 2, "DTS1", 4) == 0 ||
                                               std::memcmp(section + d + 2, "DTS2", 4) == 0 ||
                                               std::memcmp(section + d + 2, "DTS3", 4) == 0));
                d += 2 + size;
            }
        }

        if (stream_type == kStreamTypeH264 || stream_type == kStreamTypeHevc)
        {
            video_pid_ = pid;
            video_codec_ = (stream_type == kStreamTypeH264) ? Codec::h264 : Codec::hevc;
        }
        else if (stream_type == kStreamTypeAacAdts)
        {
            audio_pid_ = pid;
            audio_codec_ = Codec::aac_adts;
        }
        else if (stream_type == kStreamTypeMpeg1Audio || stream_type == kStreamTypeMpeg2Audio)
        {
            audio_pid_ = pid;
            audio_codec_ = Codec::mp3;
        }
        else if (eac3 || dts || truehd)
        {
            audio_pid_ = pid;
            audio_codec_ = truehd ? Codec::truehd : eac3 ? Codec::eac3 : Codec::dts;
        }
        else if (ac3)
        {
            audio_pid_ = pid;
            audio_codec_ = Codec::ac3;
        }
        at += 5 + info_length;
    }
}

void Demuxer::push_payload(Stream &stream, bool video, const std::uint8_t *payload,
                           std::size_t length, bool unit_start) noexcept
{
    if (unit_start)
    {
        /* A new PES starts here, so whatever was collecting is complete. */
        emit(stream, video);

        if (length < 9 || payload[0] != 0x00 || payload[1] != 0x00 || payload[2] != 0x01)
            return;
        const std::uint8_t flags = payload[7];
        const std::size_t header_length = payload[8];
        std::size_t at = 9;
        stream.pts = -1;
        stream.dts = -1;
        if ((flags & 0x80) != 0 && at + 5 <= length)
        {
            stream.pts = read_timestamp(payload + at);
            at += 5;
            if ((flags & 0x40) != 0 && at + 5 <= length)
                stream.dts = read_timestamp(payload + at);
        }
        const std::size_t data_at = 9 + header_length;
        if (data_at >= length)
        {
            stream.collecting = true;
            return;
        }
        stream.pes.assign(payload + data_at, payload + length);
        stream.collecting = true;
        return;
    }

    if (!stream.collecting)
        return;
    stream.pes.insert(stream.pes.end(), payload, payload + length);
}

void Demuxer::emit(Stream &stream, bool video) noexcept
{
    if (!stream.collecting || stream.pes.empty())
    {
        stream.pes.clear();
        return;
    }
    AccessUnit unit;
    unit.data = std::move(stream.pes);
    unit.pts = stream.pts;
    unit.dts = stream.dts;
    unit.video = video;
    ready_.push_back(std::move(unit));
    stream.pes.clear();
    stream.collecting = false;
}

void Demuxer::finish() noexcept
{
    emit(video_, true);
    emit(audio_, false);
}

bool Demuxer::next(AccessUnit &out) noexcept
{
    if (ready_.empty())
        return false;
    out = std::move(ready_.front());
    ready_.pop_front();
    return true;
}

} // namespace slopfin::ts
