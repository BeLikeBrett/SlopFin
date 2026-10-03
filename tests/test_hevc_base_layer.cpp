/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "hevc_base_layer.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
int main()
{
    using namespace slopfin::video;
    assert(hdr10_base_layer("DOVIWithEL", 7, 6, "smpte2084", "bt2020", 10));
    assert(hdr10_base_layer("DOVIWithHDR10", 8, 1, "smpte2084", "bt2020", 10));
    assert(!hdr10_base_layer("DOVI", 5, 0, "smpte2084", "bt2020", 10));
    assert(!hdr10_base_layer("DOVIWithHLG", 8, 4, "arib-std-b67", "bt2020", 10));
    assert(!hdr10_base_layer("DOVIWithEL", 7, 0, "smpte2084", "bt2020", 10));
    assert(!hdr10_base_layer("DOVIWithHDR10", 8, 1, "bt709", "bt2020", 10));
    std::vector<std::uint8_t> data, expected;
    const auto nal = [&](int type, int layer, int start, bool keep)
    {
        std::vector<std::uint8_t> n(start == 4 ? 3 : 2, 0);
        n.push_back(1);
        n.push_back(static_cast<std::uint8_t>((type << 1) | (layer >> 5)));
        n.push_back(static_cast<std::uint8_t>((layer << 3) | 1));
        n.insert(n.end(), {0xaa, 0, 0, 3, 1, 0xbb}); // Emulation prevention is not a start code.
        data.insert(data.end(), n.begin(), n.end());
        if (keep)
            expected.insert(expected.end(), n.begin(), n.end());
    };
    nal(32, 0, 4, true);
    nal(33, 0, 3, true);
    nal(39, 0, 4, true);
    nal(19, 0, 3, true);
    nal(62, 0, 4, false);
    nal(63, 0, 3, false);
    nal(19, 1, 4, false);
    nal(40, 0, 3, true);
    auto size = data.size();
    assert(strip_dolby_vision(data.data(), size));
    data.resize(size);
    assert(data == expected);
    std::uint8_t bad[] = {0, 0, 1, 0x26};
    size = sizeof(bad);
    assert(!strip_dolby_vision(bad, size));
    std::uint8_t temporal[] = {0, 0, 1, 0x26, 0};
    size = sizeof(temporal);
    assert(!strip_dolby_vision(temporal, size));
    std::puts("DV base compatibility and Annex-B filtering tests passed");
}
