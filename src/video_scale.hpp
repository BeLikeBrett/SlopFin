/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_VIDEO_SCALE_HPP
#define SLOPFIN_VIDEO_SCALE_HPP

#include <algorithm>
#include <cstdint>

namespace slopfin::video_scale
{
struct Sample
{
    int first;
    int second;
    unsigned weight;
};

inline Sample axis(int destination, int source_size, int destination_size) noexcept
{
    const auto position = std::clamp<std::int64_t>(
        ((2LL * destination + 1) * source_size * 128) / destination_size - 128, 0,
        (source_size - 1LL) * 256);
    const int first = static_cast<int>(position >> 8);
    return {first, std::min(first + 1, source_size - 1), static_cast<unsigned>(position & 255)};
}

inline std::uint32_t mix(std::uint32_t a, std::uint32_t b, unsigned weight, bool hdr10) noexcept
{
    const unsigned inv = 256 - weight;
    if (!hdr10)
    {
        const auto rb = (((a & 0x00ff00ffu) * inv + (b & 0x00ff00ffu) * weight) >> 8) & 0x00ff00ffu;
        const auto g = (((a & 0x0000ff00u) * inv + (b & 0x0000ff00u) * weight) >> 8) & 0x0000ff00u;
        return 0xff000000u | rb | g;
    }
    std::uint32_t result = 0xc0000000u;
    for (unsigned shift = 0; shift <= 20; shift += 10)
        result |= (((((a >> shift) & 1023) * inv + ((b >> shift) & 1023) * weight) >> 8) << shift);
    return result;
}
} // namespace slopfin::video_scale

#endif
