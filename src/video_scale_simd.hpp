/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_VIDEO_SCALE_SIMD_HPP
#define SLOPFIN_VIDEO_SCALE_SIMD_HPP
#include "video_scale.hpp"
#include <emmintrin.h>
namespace slopfin::video_scale
{
template <int Shift, int Mask>
inline __m128i channel_mix(__m128i a, __m128i b, __m128i weights) noexcept
{
    const auto mask = _mm_set1_epi32(Mask);
    const auto x = _mm_and_si128(_mm_srli_epi32(a, Shift), mask);
    const auto y = _mm_and_si128(_mm_srli_epi32(b, Shift), mask);
    const auto pair = _mm_or_si128(x, _mm_slli_epi32(y, 16));
    return _mm_slli_epi32(_mm_srli_epi32(_mm_madd_epi16(pair, weights), 8), Shift);
}
inline __m128i weighted(__m128i a, __m128i b, __m128i weights, bool hdr) noexcept
{
    if (hdr)
        return _mm_or_si128(_mm_set1_epi32(0xc0000000u),
                            _mm_or_si128(channel_mix<0, 1023>(a, b, weights),
                                         _mm_or_si128(channel_mix<10, 1023>(a, b, weights),
                                                      channel_mix<20, 1023>(a, b, weights))));
    return _mm_or_si128(_mm_set1_epi32(0xff000000u),
                        _mm_or_si128(channel_mix<0, 255>(a, b, weights),
                                     _mm_or_si128(channel_mix<8, 255>(a, b, weights),
                                                  channel_mix<16, 255>(a, b, weights))));
}
inline unsigned packed_weight(unsigned w) noexcept
{
    return (w << 16) | (256 - w);
}
inline void horizontal_line(const std::uint32_t *source, const Sample *columns, int width,
                            std::uint32_t *out, bool hdr) noexcept
{
    int x = 0;
    for (; x + 4 <= width; x += 4)
    {
        const auto *c = columns + x;
        const auto a = _mm_set_epi32(source[c[3].first], source[c[2].first], source[c[1].first],
                                     source[c[0].first]);
        const auto b = _mm_set_epi32(source[c[3].second], source[c[2].second], source[c[1].second],
                                     source[c[0].second]);
        const auto weights = _mm_set_epi32(packed_weight(c[3].weight), packed_weight(c[2].weight),
                                           packed_weight(c[1].weight), packed_weight(c[0].weight));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(out + x), weighted(a, b, weights, hdr));
    }
    for (; x < width; ++x)
        out[x] = mix(source[columns[x].first], source[columns[x].second], columns[x].weight, hdr);
}
inline void vertical_line(const std::uint32_t *a, const std::uint32_t *b, int width,
                          unsigned weight, std::uint32_t *out, bool hdr) noexcept
{
    const auto weights = _mm_set1_epi32(packed_weight(weight));
    int x = 0;
    for (; x + 4 <= width; x += 4)
        _mm_storeu_si128(reinterpret_cast<__m128i *>(out + x),
                         weighted(_mm_loadu_si128(reinterpret_cast<const __m128i *>(a + x)),
                                  _mm_loadu_si128(reinterpret_cast<const __m128i *>(b + x)),
                                  weights, hdr));
    for (; x < width; ++x)
        out[x] = mix(a[x], b[x], weight, hdr);
}
/* Exact 3:1 interpolation at the quarter-pixel positions of a 2x resize. */
template <int Shift> inline __m128i hdr_quarter_channel(__m128i a, __m128i b) noexcept
{
    const auto mask = _mm_set1_epi32(1023);
    const auto x = _mm_and_si128(_mm_srli_epi32(a, Shift), mask);
    const auto y = _mm_and_si128(_mm_srli_epi32(b, Shift), mask);
    return _mm_slli_epi32(_mm_add_epi32(x, _mm_srai_epi32(_mm_sub_epi32(y, x), 2)), Shift);
}
inline __m128i quarter(__m128i a, __m128i b, bool hdr) noexcept
{
    if (hdr)
        return _mm_or_si128(_mm_set1_epi32(0xc0000000u),
                            _mm_or_si128(hdr_quarter_channel<0>(a, b),
                                         _mm_or_si128(hdr_quarter_channel<10>(a, b),
                                                      hdr_quarter_channel<20>(a, b))));
    const auto zero = _mm_setzero_si128();
    const auto al = _mm_unpacklo_epi8(a, zero), ah = _mm_unpackhi_epi8(a, zero);
    const auto bl = _mm_unpacklo_epi8(b, zero), bh = _mm_unpackhi_epi8(b, zero);
    return _mm_packus_epi16(_mm_add_epi16(al, _mm_srai_epi16(_mm_sub_epi16(bl, al), 2)),
                            _mm_add_epi16(ah, _mm_srai_epi16(_mm_sub_epi16(bh, ah), 2)));
}
inline void double_line(const std::uint32_t *source, int width, std::uint32_t *out,
                        bool hdr) noexcept
{
    out[0] = source[0];
    int x = 0;
    for (; x + 4 < width; x += 4)
    {
        const auto a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(source + x));
        const auto b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(source + x + 1));
        const auto low = quarter(a, b, hdr), high = quarter(b, a, hdr);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(out + 2 * x + 1),
                         _mm_unpacklo_epi32(low, high));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(out + 2 * x + 5),
                         _mm_unpackhi_epi32(low, high));
    }
    for (; x + 1 < width; ++x)
    {
        out[2 * x + 1] = mix(source[x], source[x + 1], 64, hdr);
        out[2 * x + 2] = mix(source[x], source[x + 1], 192, hdr);
    }
    out[2 * width - 1] = source[width - 1];
}
inline void double_rows(const std::uint32_t *a, const std::uint32_t *b, int width,
                        std::uint32_t *first, std::uint32_t *second, bool hdr) noexcept
{
    int x = 0;
    for (; x + 4 <= width; x += 4)
    {
        const auto top = _mm_loadu_si128(reinterpret_cast<const __m128i *>(a + x));
        const auto bottom = _mm_loadu_si128(reinterpret_cast<const __m128i *>(b + x));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(first + x), quarter(top, bottom, hdr));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(second + x), quarter(bottom, top, hdr));
    }
    for (; x < width; ++x)
    {
        first[x] = mix(a[x], b[x], 64, hdr);
        second[x] = mix(a[x], b[x], 192, hdr);
    }
}
} // namespace slopfin::video_scale
#endif
