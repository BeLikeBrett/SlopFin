/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_HDR_HPP
#define SLOPFIN_HDR_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace slopfin::hdr
{
inline constexpr std::uint64_t kPixelFormat = 0x8100070422000000ULL;
inline constexpr float kPaperWhiteNits = 203.0f;

inline double eotf(double code) noexcept
{
    const double p = std::pow(std::clamp(code, 0.0, 1.0), 32.0 / 2523.0);
    return 10000.0 *
           std::pow(std::max(p - 3424.0 / 4096.0, 0.0) / (2413.0 / 128.0 - 2392.0 / 128.0 * p),
                    16384.0 / 2610.0);
}

inline double oetf(double nits) noexcept
{
    const double p = std::pow(std::clamp(nits / 10000.0, 0.0, 1.0), 2610.0 / 16384.0);
    return std::pow((3424.0 / 4096.0 + 2413.0 / 128.0 * p) / (1.0 + 2392.0 / 128.0 * p),
                    2523.0 / 32.0);
}

constexpr std::uint32_t pack(int r, int g, int b) noexcept
{
    return 0xc0000000u | static_cast<std::uint32_t>(std::clamp(r, 0, 1023)) |
           (static_cast<std::uint32_t>(std::clamp(g, 0, 1023)) << 10) |
           (static_cast<std::uint32_t>(std::clamp(b, 0, 1023)) << 20);
}

/* PS5 Main10 samples are low-aligned. BT.2020 NCL matrixing preserves PQ. */
inline std::uint32_t from_ycbcr10(int y, int cb, int cr) noexcept
{
    // Q14 coefficients: less than one code of error against the floating
    // BT.2020 NCL reference, with integer clamps suitable for vectorization.
    const int luma = (y - 64) * 19134;
    const int u = cb - 512;
    const int v = cr - 512;
    return pack((luma + 27584 * v + 8192) >> 14, (luma - 3078 * u - 10688 * v + 8192) >> 14,
                (luma + 35194 * u + 8192) >> 14);
}

/* Immutable after display initialization; shared by the rendering workers.
 * A square-root-spaced LUT preserves dark detail without per-pixel powers. */
struct Compositor
{
    std::array<float, 256> srgb{};
    std::array<float, 1024> pq{};
    std::array<std::uint16_t, 16385> encoded{};
    std::array<std::array<std::uint16_t, 1024>, 256> black_overlay{};

    void initialize() noexcept
    {
        for (unsigned i = 0; i < srgb.size(); ++i)
        {
            const double c = i / 255.0;
            srgb[i] = static_cast<float>(
                (c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4)) * kPaperWhiteNits);
        }
        for (unsigned i = 0; i < pq.size(); ++i)
            pq[i] = static_cast<float>(eotf(i / 1023.0));
        for (unsigned i = 0; i < encoded.size(); ++i)
        {
            const double s = i / 16384.0;
            encoded[i] = static_cast<std::uint16_t>(oetf(s * s * 10000.0) * 1023.0 + 0.5);
        }
        for (unsigned alpha = 0; alpha < black_overlay.size(); ++alpha)
            for (unsigned code = 0; code < pq.size(); ++code)
                black_overlay[alpha][code] =
                    static_cast<std::uint16_t>(encode(pq[code] * (1.0f - alpha / 255.0f)));
    }

    int encode(float nits) const noexcept
    {
        const auto index = static_cast<unsigned>(
            std::sqrt(std::clamp(nits, 0.0f, 10000.0f) / 10000.0f) * 16384.0f + 0.5f);
        return encoded[index];
    }

    std::uint32_t blend(std::uint32_t dst, std::uint32_t argb) const noexcept
    {
        const unsigned alpha = argb >> 24;
        if (alpha == 0)
            return dst;
        // Playback scrims cover millions of pixels; precompute their linear-light blend.
        if ((argb & 0x00ffffffu) == 0)
        {
            const auto &table = black_overlay[alpha];
            return pack(table[dst & 1023], table[(dst >> 10) & 1023], table[(dst >> 20) & 1023]);
        }
        const float r = srgb[(argb >> 16) & 255];
        const float g = srgb[(argb >> 8) & 255];
        const float b = srgb[argb & 255];
        const float a = alpha / 255.0f;
        const float inv = 1.0f - a;
        return pack(
            encode((0.627404f * r + 0.329283f * g + 0.043313f * b) * a + pq[dst & 1023] * inv),
            encode((0.069097f * r + 0.919540f * g + 0.011363f * b) * a +
                   pq[(dst >> 10) & 1023] * inv),
            encode((0.016391f * r + 0.088013f * g + 0.895596f * b) * a +
                   pq[(dst >> 20) & 1023] * inv));
    }
};
} // namespace slopfin::hdr

#endif
