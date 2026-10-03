/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
// Real CPU decoders: fragmentation, timestamps, cancellation, EOF and reopen.
// Run via tools/test-software-audio.sh (requires host FFmpeg development libs).
#include "software_audio.hpp"
#include "bigalloc.hpp"
#include "tsdemux.hpp"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include <algorithm>
namespace slopfin::bigalloc
{
void *allocate(std::size_t n) noexcept
{
    return std::malloc(n);
}
void release(void *p) noexcept
{
    std::free(p);
}
} // namespace slopfin::bigalloc
using namespace slopfin::software_audio;
struct Capture
{
    std::vector<std::int16_t> pcm;
    std::int64_t first = -1, last = -1;
    unsigned channels = 0;
};
bool collect(void *opaque, const Pcm &pcm) noexcept
{
    auto &c = *static_cast<Capture *>(opaque);
    if (c.pcm.empty())
    {
        c.first = pcm.pts;
        c.channels = pcm.channels;
    }
    assert(c.channels == pcm.channels && pcm.frames > 0);
    assert(pcm.pts >= c.last);
    c.last = pcm.pts;
    for (int i : pcm.map)
        assert(i >= -1 && i < static_cast<int>(pcm.channels));
    c.pcm.insert(c.pcm.end(), pcm.samples, pcm.samples + pcm.frames * pcm.channels);
    return true;
}
bool cancel(void *, const Pcm &) noexcept
{
    return false;
}
int main(int argc, char **argv)
{
    assert(argc == 4 || argc == 5);
    const bool transport = argc == 5;
    const std::string name = argv[1];
    const Codec codec = name.starts_with("truehd") ? Codec::truehd
                        : name.starts_with("dts")  ? Codec::dts
                                                   : Codec::eac3;
    std::ifstream file(argv[2], std::ios::binary);
    std::vector<std::uint8_t> input((std::istreambuf_iterator<char>(file)), {});
    assert(!input.empty());
    Decoder decoder;
    Capture baseline;
    for (const std::size_t chunk : {65536u, 188u, 1u, 4093u})
    {
        assert(decoder.open(codec));
        Capture actual;
        if (transport)
        {
            slopfin::ts::Demuxer demux;
            const auto drain = [&]
            {
                slopfin::ts::AccessUnit unit;
                while (demux.next(unit))
                {
                    assert(!unit.video);
                    const auto expected = codec == Codec::truehd ? slopfin::ts::Codec::truehd
                                          : codec == Codec::eac3 ? slopfin::ts::Codec::eac3
                                                                 : slopfin::ts::Codec::dts;
                    assert(demux.audio_codec() == expected);
                    assert(decoder.feed(unit.data.data(), unit.data.size(), unit.pts, collect,
                                        &actual));
                }
            };
            for (std::size_t at = 0; at < input.size(); at += chunk)
            {
                demux.feed(input.data() + at, std::min(chunk, input.size() - at));
                drain();
            }
            demux.finish();
            drain();
        }
        else
        {
            for (std::size_t at = 0; at < input.size(); at += chunk)
                assert(decoder.feed(input.data() + at, std::min(chunk, input.size() - at),
                                    at == 0 ? 90000 : -1, collect, &actual));
        }
        assert(decoder.finish(collect, &actual));
        assert(!actual.pcm.empty());
        assert(decoder.finish(collect, &actual)); // idempotent drain
        assert(!decoder.feed(input.data(), 1, -1, collect, &actual));
        if (baseline.pcm.empty())
            baseline = actual;
        else
        {
            assert(actual.pcm == baseline.pcm);
            assert(actual.first == baseline.first);
        }
        assert(transport ? actual.first >= 0 : actual.first == 90000);
    }
    if (!transport)
    {
        assert(decoder.open(codec));
        assert(!decoder.feed(input.data(), input.size(), 0, cancel, nullptr));
        assert(!decoder.finish(collect, &baseline));
    }
    decoder.close();
    decoder.close();
    assert(!decoder.feed(input.data(), input.size(), 0, collect, &baseline));
    std::ofstream out(argv[3], std::ios::binary);
    out.write(reinterpret_cast<const char *>(baseline.pcm.data()), baseline.pcm.size() * 2);
    std::cout << name << ": fragmentation/PTS/EOF/cancel/reopen passed; "
              << baseline.pcm.size() / baseline.channels << " frames, " << baseline.channels
              << " channels\n";
}
