/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_BITMAP_SAMPLE_HPP
#define SLOPFIN_BITMAP_SAMPLE_HPP
#include <algorithm>
#include <cstdint>
namespace slopfin::bitmap_sample
{
/* Filter coverage and premultiplied colour together, then return straight ARGB.
   Filtering straight RGB separately darkens white lettering at transparent edges. */
inline std::uint32_t alpha_bilinear(const std::uint32_t *pixels, int width, int height,
                                    std::int64_t u, std::int64_t v) noexcept
{
    u = std::clamp<std::int64_t>(u, 0, (width - 1LL) << 16);
    v = std::clamp<std::int64_t>(v, 0, (height - 1LL) << 16);
    const int x = static_cast<int>(u >> 16), y = static_cast<int>(v >> 16);
    const unsigned fx = static_cast<unsigned>((u >> 8) & 255);
    const unsigned fy = static_cast<unsigned>((v >> 8) & 255);
    const std::uint32_t colors[] = {
        pixels[y * width + x], pixels[y * width + std::min(x + 1, width - 1)],
        pixels[std::min(y + 1, height - 1) * width + x],
        pixels[std::min(y + 1, height - 1) * width + std::min(x + 1, width - 1)]};
    const unsigned weights[] = {(256 - fx) * (256 - fy), fx * (256 - fy), (256 - fx) * fy, fx * fy};
    std::uint32_t alpha = 0, sums[3] = {};
    for (unsigned i = 0; i < 4; ++i)
    {
        const unsigned covered = (colors[i] >> 24) * weights[i];
        alpha += covered;
        for (unsigned c = 0; c < 3; ++c)
            sums[c] += ((colors[i] >> (c * 8)) & 255) * covered;
    }
    if (!alpha)
        return 0;
    std::uint32_t result = ((alpha + 32768) >> 16) << 24;
    for (unsigned c = 0; c < 3; ++c)
        result |= ((sums[c] + alpha / 2) / alpha) << (c * 8);
    return result;
}
} // namespace slopfin::bitmap_sample
#endif
