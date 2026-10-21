/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * What the playback reader needs from a container: bytes in, in file order,
 * and whole access units out with 90 kHz timestamps. MPEG-TS from the
 * server, and MKV or MP4 read straight from the library file, all look the
 * same past this point, so the decoder and audio paths never see a container.
 */
#ifndef SLOPFIN_MEDIA_DEMUX_HPP
#define SLOPFIN_MEDIA_DEMUX_HPP

#include "tsdemux.hpp"

namespace slopfin::media
{

class Demuxer
{
  public:
    virtual ~Demuxer() = default;
    /* Accepts any number of bytes; the stream continues from the last call. */
    virtual void feed(const std::uint8_t *bytes, std::size_t count) noexcept = 0;
    /* Moves one complete access unit out, in file order. */
    virtual bool next(ts::AccessUnit &out) noexcept = 0;
    /* The stream has ended: release anything still held. */
    virtual void finish() noexcept = 0;
    [[nodiscard]] virtual ts::Codec video_codec() const noexcept = 0;
    [[nodiscard]] virtual ts::Codec audio_codec() const noexcept = 0;
    /* The bytes could not be understood; nothing more will come out. */
    [[nodiscard]] virtual bool broken() const noexcept
    {
        return false;
    }
};

class TsDemuxer final : public Demuxer
{
    ts::Demuxer inner_;

  public:
    void feed(const std::uint8_t *bytes, std::size_t count) noexcept override
    {
        inner_.feed(bytes, count);
    }
    bool next(ts::AccessUnit &out) noexcept override
    {
        return inner_.next(out);
    }
    void finish() noexcept override
    {
        inner_.finish();
    }
    [[nodiscard]] ts::Codec video_codec() const noexcept override
    {
        return inner_.video_codec();
    }
    [[nodiscard]] ts::Codec audio_codec() const noexcept override
    {
        return inner_.audio_codec();
    }
};

} // namespace slopfin::media

#endif
