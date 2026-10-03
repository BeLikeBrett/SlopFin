/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_MEDIA_BITRATE_HPP
#define SLOPFIN_MEDIA_BITRATE_HPP

#include <cstddef>
#include <cstdint>

namespace slopfin::player
{
/* Compressed audio/video payload per media second, excluding TS overhead.
 * Decode timestamps avoid B-frame presentation-order jitter. No wall clock:
 * buffering, slow rendering and pause must not alter a file's bitrate. */
class MediaBitrate
{
  public:
    double add(std::size_t bytes, bool video, std::int64_t timestamp) noexcept
    {
        bytes_ += bytes;
        if (!video || timestamp < 0)
            return value_;
        if (start_ < 0 || timestamp < last_ - 90000 || timestamp - last_ > 30 * 90000)
        {
            start_ = last_ = timestamp;
            bytes_ = 0;
            value_ = 0;
            return value_;
        }
        if (timestamp < last_)
            return value_; // PTS-only B-frame reorder.
        last_ = timestamp;
        const auto elapsed = timestamp - start_;
        if (elapsed >= 3 * 90000)
        {
            value_ = static_cast<double>(bytes_) * 8.0 * 90000.0 / elapsed;
            start_ = timestamp;
            bytes_ = 0;
        }
        return value_;
    }

  private:
    std::int64_t start_ = -1;
    std::int64_t last_ = -1;
    std::size_t bytes_ = 0;
    double value_ = 0;
};
} // namespace slopfin::player
#endif
