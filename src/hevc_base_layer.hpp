/* SlopFin HDR10-compatible Dolby Vision base-layer helpers.
 * Copyright (C) 2026 Brett. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_HEVC_BASE_LAYER_HPP
#define SLOPFIN_HEVC_BASE_LAYER_HPP
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace slopfin::video
{
inline bool hdr10_base_layer(std::string_view range, int profile, int compatibility,
                             std::string_view transfer, std::string_view primaries,
                             int depth) noexcept
{
    if (transfer != "smpte2084" || primaries != "bt2020" || depth != 10)
        return false;
    return (profile == 7 && compatibility == 6 && range == "DOVIWithEL") ||
           (profile == 8 && compatibility == 1 &&
            (range == "DOVIWithHDR10" || range == "DOVIWithHDR10Plus"));
}

/* In-place complete Annex-B access unit filtering. Keep layer-zero HEVC and
 * its static HDR SEI; discard DV RPU (62), encapsulated EL (63), and nonzero
 * HEVC layers. Never apply to Profile 5 or unknown compatibility metadata.
 * No allocation; false means malformed input and the decoder must not use it. */
inline bool strip_dolby_vision(std::uint8_t *data, std::size_t &bytes) noexcept
{
    const auto prefix = [data, bytes](std::size_t at) -> std::size_t
    {
        if (at + 3 <= bytes && data[at] == 0 && data[at + 1] == 0)
        {
            if (data[at + 2] == 1)
                return 3;
            if (at + 4 <= bytes && data[at + 2] == 0 && data[at + 3] == 1)
                return 4;
        }
        return 0;
    };
    std::size_t cursor = 0, written = 0;
    while (cursor < bytes)
    {
        const std::size_t start = prefix(cursor);
        if (!start || cursor + start + 2 > bytes)
            return false;
        const auto first = data[cursor + start];
        const auto second = data[cursor + start + 1];
        if ((first & 0x80) || !(second & 7))
            return false;
        const unsigned type = (first >> 1) & 63;
        const unsigned layer = ((first & 1) << 5) | (second >> 3);
        std::size_t end = cursor + start + 2;
        while (end < bytes && !prefix(end))
            ++end;
        if (layer == 0 && type != 62 && type != 63)
        {
            std::memmove(data + written, data + cursor, end - cursor);
            written += end - cursor;
        }
        cursor = end;
    }
    bytes = written;
    return true;
}
} // namespace slopfin::video
#endif
