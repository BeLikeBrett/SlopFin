/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
// SlopFin icon rendering. SPDX-License-Identifier: GPL-3.0-or-later
// Checks coverage at points whose answer is known from the geometry, and writes
// every icon to a PGM so the shapes can be looked at, not just asserted.
#include "icons.hpp"

#include <cassert>
#include <cstdio>
#include <string>

namespace slopfin::gfx
{
void blend_mask_physical(int, int, int, int, const unsigned char *, Color) noexcept
{
}
void blend_mask_gradient_physical(int, int, int, int, const unsigned char *, Color, Color) noexcept
{
}
int to_physical_x(int x) noexcept
{
    return x;
}
int to_physical_y(int y) noexcept
{
    return y;
}
int to_physical_size(int v) noexcept
{
    return v;
}
} // namespace slopfin::gfx

using slopfin::icons::Icon;

static int at(Icon icon, int size, float u, float v)
{
    const unsigned char *m = slopfin::icons::mask(icon, size);
    return m[static_cast<int>(v * size) * size + static_cast<int>(u * size)];
}

int main(int argc, char **argv)
{
    constexpr int kSize = 96;
    // Play: solid in the middle of the triangle, empty in the corners.
    assert(at(Icon::play, kSize, 0.45f, 0.5f) == 255);
    assert(at(Icon::play, kSize, 0.05f, 0.05f) == 0);
    assert(at(Icon::play, kSize, 0.9f, 0.1f) == 0);
    // Pause: two bars with a gap between them.
    assert(at(Icon::pause, kSize, 0.35f, 0.5f) == 255);
    assert(at(Icon::pause, kSize, 0.5f, 0.5f) == 0);
    // Info: hollow ring, so the area between ring and stem is empty.
    assert(at(Icon::info, kSize, 0.3f, 0.5f) == 0);
    assert(at(Icon::info, kSize, 0.5f, 0.6f) == 255);
    // Edges are anti-aliased: some pixels are neither empty nor full.
    const unsigned char *m = slopfin::icons::mask(Icon::subtitles, kSize);
    int partial = 0;
    for (int i = 0; i < kSize * kSize; ++i)
        partial += (m[i] > 0 && m[i] < 255) ? 1 : 0;
    assert(partial > 50);
    // Back is the mirror image of forward.
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x)
            assert(slopfin::icons::mask(Icon::skip_back, kSize)[y * kSize + x] ==
                   slopfin::icons::mask(Icon::skip_forward, kSize)[y * kSize + (kSize - 1 - x)]);

    if (argc > 1)
    {
        // One strip of every icon, for eyeballing.
        const int count = static_cast<int>(Icon::count);
        const std::string path = std::string{argv[1]} + "/icons.pgm";
        FILE *f = std::fopen(path.c_str(), "wb");
        std::fprintf(f, "P5 %d %d 255\n", kSize * count, kSize);
        for (int y = 0; y < kSize; ++y)
            for (int i = 0; i < count; ++i)
                std::fwrite(slopfin::icons::mask(static_cast<Icon>(i), kSize) + y * kSize, 1, kSize,
                            f);
        std::fclose(f);
        std::printf("wrote %s\n", path.c_str());
    }
    std::puts("Icon rendering regressions passed");
}
