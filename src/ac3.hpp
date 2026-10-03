/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
// SlopFin native AC-3 contract. SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SLOPFIN_AC3_HPP
#define SLOPFIN_AC3_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace slopfin::audio
{
struct Ac3Param
{
    std::uint32_t size = sizeof(Ac3Param);
    std::uint32_t word_size = 1;
    std::uint32_t output_options[10] = {3, 2, 1, 7, 6, 0x7fffff, 0, 0, 0x7fffff, 0x7fffff};
    std::int32_t channel_map[6] = {0, 1, 2, 3, 4, 5};
    std::uint32_t timestamps[4] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
};

struct Ac3Info
{
    std::uint32_t size = sizeof(Ac3Info);
    std::uint32_t sample_rate = 0;
    std::uint32_t bitrate = 0;
    std::uint32_t acmod = 0;
    std::uint32_t lfe = 0;
    std::int32_t result = 0;
    std::uint32_t timestamps[6] = {};
};
static_assert(sizeof(Ac3Param) == 88);
static_assert(sizeof(Ac3Info) == 48);

// Measured with six independent tones: L, C, R, Ls, Rs, LFE.
inline constexpr int kAc3ToStandard[8] = {0, 2, 1, 5, 3, 4, -1, -1};

inline std::size_t ac3_frame_bytes(const std::uint8_t *data, std::size_t bytes) noexcept
{
    if (bytes < 7 || data[0] != 0x0b || data[1] != 0x77 || (data[4] >> 6) != 0 ||
        (data[5] >> 3) > 8)
        return 0;
    // The sink runs at 48 kHz; negotiation excludes half-rate and other rates.
    constexpr unsigned bitrates[] = {32,  40,  48,  56,  64,  80,  96,  112, 128, 160,
                                     192, 224, 256, 320, 384, 448, 512, 576, 640};
    const unsigned code = data[4] & 63;
    return code < 38 ? bitrates[code / 2] * 4 : 0;
}

class Ac3Frames
{
  public:
    void reset() noexcept
    {
        used_ = expected_ = 0;
        pts_ = -1;
    }

    template <typename Consumer>
    void feed(const std::uint8_t *data, std::size_t bytes, std::int64_t pts, Consumer consume)
    {
        while (bytes > 0)
        {
            if (used_ == 0)
                pts_ = pts;
            const std::size_t target = expected_ ? expected_ : 7;
            const std::size_t take = bytes < target - used_ ? bytes : target - used_;
            std::memcpy(buffer_.data() + used_, data, take);
            used_ += take;
            data += take;
            bytes -= take;
            if (!expected_ && used_ == 7)
            {
                expected_ = ac3_frame_bytes(buffer_.data(), used_);
                if (!expected_)
                {
                    std::memmove(buffer_.data(), buffer_.data() + 1, --used_);
                    continue;
                }
            }
            if (expected_ && used_ == expected_)
            {
                consume(buffer_.data(), used_, pts_);
                pts = pts_ < 0 ? -1 : pts_ + 2880; // 1536 samples at 48 kHz.
                used_ = expected_ = 0;
            }
        }
    }

  private:
    std::array<std::uint8_t, 2560> buffer_{};
    std::size_t used_ = 0;
    std::size_t expected_ = 0;
    std::int64_t pts_ = -1;
};
} // namespace slopfin::audio
#endif
