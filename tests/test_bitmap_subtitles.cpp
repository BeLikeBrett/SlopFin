/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "bitmap_subtitles.hpp"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_ZLIB
#define STBI_SUPPORT_ZLIB
#define STBI_NO_STDIO
#include "../vendor/stb_image.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <zlib.h>
namespace slopfin::bigalloc
{
void *allocate(std::size_t bytes) noexcept
{
    return std::malloc(bytes);
}
void release(void *p) noexcept
{
    std::free(p);
}
} // namespace slopfin::bigalloc
using namespace slopfin::bitmap_subtitles;
namespace
{
using Bytes = std::vector<std::uint8_t>;
void segment(Bytes &out, unsigned type, const Bytes &body)
{
    out.push_back(type);
    out.push_back(body.size() >> 8);
    out.push_back(body.size());
    out.insert(out.end(), body.begin(), body.end());
}
Bytes pgs(bool clear = false)
{
    Bytes out;
    // 1920x1080, epoch start; one 4x2 object at (100,900).
    Bytes pcs{7, 128, 4, 56, 0x10, 0, 0, 0x80, 0, 0, static_cast<std::uint8_t>(clear ? 0 : 1)};
    if (!clear)
        pcs.insert(pcs.end(), {0, 0, 0, 0, 0, 100, 3, 132});
    segment(out, 0x16, pcs);
    if (!clear)
    {
        segment(out, 0x14, {0, 0, 0, 16, 128, 128, 0, 1, 235, 128, 128, 255});
        segment(out, 0x15, {0, 0, 0, 0xc0, 0, 0, 14, 0, 4, 0, 2, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1});
    }
    segment(out, 0x80, {});
    return out;
}
} // namespace
int main()
{
    assert(available());
    reset();
    enable(true);
    Decoder decoder;
    assert(!decoder.open("dvbsub", {}, 1920, 1080));
    assert(decoder.open("pgssub", {}, 1920, 1080));
    auto data = pgs();
    assert(decoder.decode(data.data(), data.size(), 90000, 0));
    assert(!current(0.9));
    auto cue = current(1.0);
    assert(cue && cue->width == 1920 && cue->height == 1080 && cue->rects.size() == 1);
    const auto &r = cue->rects[0];
    assert(r.x == 100 && r.y == 900 && r.width == 4 && r.height == 2);
    for (int i = 0; i < 8; ++i)
        assert(r.pixels.get()[i] == 0xffffffff);
    data = pgs(true);
    assert(decoder.decode(data.data(), data.size(), 180000, 0));
    assert(current(1.99) && !current(2.0));
    // Decode-ahead may queue the next subtitle before the renderer consumes this one.
    data = pgs();
    assert(decoder.decode(data.data(), data.size(), 270000, 45000));
    assert(!current(2.5) && current(3.0) && !current(3.5));
    enable(false);
    assert(!current(3.1));
    reset();
    enable(true);
    assert(!current(3.1));
    // No stale captions after a seek/reset; an already-acquired image stays alive.
    assert(cue->rects[0].pixels.get()[0] == 0xffffffff);
    assert(!decoder.decode(data.data(), data.size(), -1, 0));
    assert(!decoder.decode(data.data(), 5u * 1024u * 1024u, 0, 0));
    auto oversized = std::make_shared<Frame>();
    oversized->start = 1;
    oversized->end = 2;
    oversized->bytes = 65u * 1024u * 1024u;
    assert(!append(oversized));
    reset();
    enable(true);
    data = pgs();
    Bytes compressed(compressBound(data.size()));
    uLongf compressed_size = compressed.size();
    assert(compress2(compressed.data(), &compressed_size, data.data(), data.size(), 9) == Z_OK);
    compressed.resize(compressed_size);
    Decoder zipped;
    assert(zipped.open("pgssub", {}, 1920, 1080, 0));
    assert(zipped.decode(compressed.data(), compressed.size(), 90000, 0));
    assert(current(1) && current(1)->rects[0].pixels.get()[0] == 0xffffffff);
    const Bytes corrupt_zlib{1, 2, 3};
    assert(!zipped.decode(corrupt_zlib.data(), corrupt_zlib.size(), 180000, 0));
    reset();
    enable(true);
    Decoder stripped;
    assert(stripped.open("pgssub", {}, 1920, 1080, 3, {data.begin(), data.begin() + 3}));
    assert(stripped.decode(data.data() + 3, data.size() - 3, 90000, 0));
    assert(current(1) && current(1)->rects[0].pixels.get()[0] == 0xffffffff);
    reset();
    enable(true);
    // Malformed packets must be safely rejected, without leaving a partial frame.
    const Bytes bad{0x16, 0xff, 0xff, 0};
    assert(decoder.decode(bad.data(), bad.size(), 0, 0));
    assert(!current(0));
    reset();
    enable(true);
    // Self-authored DVD subpicture: two interlaced RLE rows, a palette and start/stop commands.
    Bytes dvd{0, 0,    0, 6, 0x11, 0x11, 0,    0,    0,    0, 1, 3, 0, 0x10, 4,
              0, 0xf0, 5, 6, 0x40, 0x67, 0x19, 0x01, 0x91, 6, 0, 4, 0, 5,    0xff};
    const auto stop = dvd.size();
    dvd[8] = stop >> 8;
    dvd[9] = stop;
    dvd.insert(dvd.end(), {0, 90, static_cast<std::uint8_t>(stop >> 8),
                           static_cast<std::uint8_t>(stop), 2, 0xff});
    dvd[0] = dvd.size() >> 8;
    dvd[1] = dvd.size();
    const std::string palette =
        "size: 720x480\npalette: 000000, ffffff, 000000, 000000, 000000, 000000, 000000, 000000, "
        "000000, 000000, 000000, 000000, 000000, 000000, 000000, 000000\n";
    Decoder dvd_decoder;
    assert(dvd_decoder.open("dvdsub", {palette.begin(), palette.end()}, 0, 0));
    assert(dvd_decoder.decode(dvd.data(), dvd.size(), 90000, 45000));
    auto dvd_cue = current(1.0);
    assert(dvd_cue && dvd_cue->width == 720 && dvd_cue->height == 480 &&
           dvd_cue->rects.size() == 1);
    const auto &dr = dvd_cue->rects[0];
    assert(dr.x == 100 && dr.y == 400 && dr.width == 4 && dr.height == 2);
    for (int i = 0; i < 8; ++i)
        assert(dr.pixels.get()[i] == 0xffffffff);
    assert(current(1.49) && !current(1.5));
    reset();
    std::puts("PGS pixels, timing, clear events, decode-ahead, disable/reset and bounds passed");
}
