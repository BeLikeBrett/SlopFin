/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
// Complete AAC ADTS frames across transport/PES boundaries.
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SLOPFIN_ADTS_FRAMES_HPP
#define SLOPFIN_ADTS_FRAMES_HPP
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace slopfin::audio
{
class AdtsFrames
{
  public:
    void reset() noexcept
    {
        used_ = expected_ = 0;
        pts_ = -1;
    }
    template <class Consumer>
    void feed(const std::uint8_t *data, std::size_t bytes, std::int64_t pts, Consumer consume)
    {
        while (bytes)
        {
            if (!used_)
                pts_ = pts;
            const auto target = expected_ ? expected_ : 7u;
            const auto take = bytes < target - used_ ? bytes : target - used_;
            std::memcpy(buffer_.data() + used_, data, take);
            used_ += take;
            data += take;
            bytes -= take;
            if (!expected_ && used_ == 7)
            {
                const auto *h = buffer_.data();
                const unsigned frequency = (h[2] >> 2) & 15;
                const unsigned length = ((h[3] & 3u) << 11) | (h[4] << 3) | (h[5] >> 5);
                const unsigned header = (h[1] & 1) ? 7 : 9;
                if (h[0] != 0xff || (h[1] & 0xf6) != 0xf0 || frequency >= 13 || length < header)
                {
                    std::memmove(buffer_.data(), buffer_.data() + 1, --used_);
                    continue;
                }
                expected_ = length;
            }
            if (expected_ && used_ == expected_)
            {
                constexpr unsigned rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                              22050, 16000, 12000, 11025, 8000,  7350};
                const auto duration =
                    90000LL * 1024 * (1 + (buffer_[6] & 3)) / rates[(buffer_[2] >> 2) & 15];
                consume(buffer_.data(), used_, pts_);
                pts = pts_ < 0 ? -1 : pts_ + duration;
                used_ = expected_ = 0;
            }
        }
    }

  private:
    std::array<std::uint8_t, 8191> buffer_{};
    std::size_t used_ = 0, expected_ = 0;
    std::int64_t pts_ = -1;
};
} // namespace slopfin::audio
#endif
