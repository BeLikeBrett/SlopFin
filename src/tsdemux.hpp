/*
 * SlopFin - MPEG-TS demultiplexer.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Jellyfin can hand back a single continuous MPEG-TS stream, which avoids
 * playlist handling entirely: one socket, one demuxer. Bytes go in as they
 * arrive and complete access units come out.
 *
 * Deliberately free of PS5 headers so it can be tested on a workstation.
 */

#ifndef SLOPFIN_TSDEMUX_HPP
#define SLOPFIN_TSDEMUX_HPP

#include <cstdint>
#include <deque>
#include <vector>

namespace slopfin::ts
{

enum class Codec : std::uint8_t
{
    unknown = 0,
    h264,
    hevc,
    aac_adts,
    mp3,
    ac3,
    eac3,
    dts,
    truehd,
};

struct AccessUnit
{
    std::vector<std::uint8_t> data;
    std::int64_t pts = -1; /* 90 kHz units, -1 when absent */
    std::int64_t dts = -1;
    bool video = false;
};

class Demuxer final
{
  public:
    /* Accepts any number of bytes, aligned or not. */
    void feed(const std::uint8_t *bytes, std::size_t count) noexcept;

    /* Moves one complete access unit out, oldest first. */
    bool next(AccessUnit &out) noexcept;

    /* Flushes whatever is still buffered; call when the stream ends. */
    void finish() noexcept;

    void reset() noexcept;

    [[nodiscard]] Codec video_codec() const noexcept
    {
        return video_codec_;
    }
    [[nodiscard]] Codec audio_codec() const noexcept
    {
        return audio_codec_;
    }
    [[nodiscard]] std::size_t pending() const noexcept
    {
        return ready_.size();
    }

  private:
    struct Stream
    {
        std::vector<std::uint8_t> pes;
        std::int64_t pts = -1;
        std::int64_t dts = -1;
        bool collecting = false;
    };

    void consume_packet(const std::uint8_t *packet) noexcept;
    void parse_pat(const std::uint8_t *section, std::size_t length) noexcept;
    void parse_pmt(const std::uint8_t *section, std::size_t length) noexcept;
    void push_payload(Stream &stream, bool video, const std::uint8_t *payload, std::size_t length,
                      bool unit_start) noexcept;
    void emit(Stream &stream, bool video) noexcept;

    std::vector<std::uint8_t> spare_; /* bytes left over between feeds */
    std::deque<AccessUnit> ready_;

    int pmt_pid_ = -1;
    int video_pid_ = -1;
    int audio_pid_ = -1;
    Codec video_codec_ = Codec::unknown;
    Codec audio_codec_ = Codec::unknown;

    Stream video_;
    Stream audio_;
};

} // namespace slopfin::ts

#endif
