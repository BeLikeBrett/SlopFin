/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
// SlopFin IEC 61937 packing. SPDX-License-Identifier: GPL-3.0-or-later
//
// Expected values come from IEC 61937 as FFmpeg's spdif muxer writes it, not
// from this packer: the packer's output for real AC-3, E-AC-3, DTS and a
// DTS-HD MA film track was compared byte for byte with `ffmpeg -c copy -f
// spdif` on 2026-09-15 and was identical. (Opus 5.)
#include "iec61937.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace slopfin::audio::iec61937;

namespace
{
struct Burst
{
    std::vector<std::uint8_t> bytes; /* little-endian, as the sink receives them */
    std::int64_t pts;
};

std::vector<Burst> pack(Format format, const std::vector<std::uint8_t> &stream, std::size_t piece)
{
    static Packer packer;
    packer.reset(format);
    std::vector<Burst> out;
    auto sink = [&](const std::uint16_t *words, std::size_t count, std::int64_t pts)
    {
        Burst burst{{}, pts};
        for (std::size_t i = 0; i < count; ++i)
        {
            burst.bytes.push_back(words[i] & 0xff);
            burst.bytes.push_back(words[i] >> 8);
        }
        out.push_back(burst);
    };
    for (std::size_t at = 0; at < stream.size(); at += piece)
        packer.feed(stream.data() + at, std::min(piece, stream.size() - at), at == 0 ? 9000 : -1,
                    sink);
    packer.flush(sink);
    return out;
}

std::vector<std::uint8_t> ac3_frame()
{
    std::vector<std::uint8_t> frame(1792, 0x5a); /* 448 kbit/s at 48 kHz */
    frame[0] = 0x0b, frame[1] = 0x77, frame[2] = 0x12, frame[3] = 0x34;
    frame[4] = 30, frame[5] = (8 << 3) | 2; /* bsid 8, bsmod 2 */
    return frame;
}

std::vector<std::uint8_t> eac3_frame(bool dependent, std::size_t bytes)
{
    std::vector<std::uint8_t> frame(bytes, 0x33);
    const std::size_t words = bytes / 2 - 1;
    frame[0] = 0x0b, frame[1] = 0x77;
    frame[2] = static_cast<std::uint8_t>((dependent ? 0x40 : 0) | (words >> 8));
    frame[3] = static_cast<std::uint8_t>(words);
    frame[4] = 0x30 | 0x0f; /* fscod 48 kHz, numblkscod 3 (six blocks), acmod 7, lfe */
    frame[5] = 16 << 3;
    return frame;
}
} // namespace

int main()
{
    /* AC-3: one frame per 6144-byte burst, length in bits, bytes swapped. */
    {
        auto one = ac3_frame();
        std::vector<std::uint8_t> stream(one);
        stream.insert(stream.end(), {0x00, 0x11, 0x22}); /* junk between frames */
        stream.insert(stream.end(), one.begin(), one.end());
        for (std::size_t piece : {1u, 7u, 188u, 5000u})
        {
            const auto bursts = pack(Format::ac3, stream, piece);
            assert(bursts.size() == 2);
            const auto &b = bursts[0].bytes;
            assert(b.size() == 6144);
            assert(b[0] == 0x72 && b[1] == 0xf8 && b[2] == 0x1f && b[3] == 0x4e);
            assert(b[4] == 0x01 && b[5] == 0x02); /* data type 1, bsmod 2 */
            assert(b[6] == 0x00 && b[7] == 0x38); /* 1792 * 8 = 0x3800 bits */
            assert(b[8] == 0x77 && b[9] == 0x0b); /* sync word swapped */
            assert(b[10] == 0x34 && b[11] == 0x12);
            assert(b[8 + 1792] == 0 && b.back() == 0); /* zero padding */
            assert(bursts[0].pts == 9000);
        }
    }

    /* E-AC-3: a 7.1 stream's dependent substream rides in its independent
       frame's burst; bursts are 24576 bytes with the length in bytes. */
    {
        std::vector<std::uint8_t> stream;
        for (int i = 0; i < 3; ++i)
        {
            auto independent = eac3_frame(false, 1200);
            auto dependent = eac3_frame(true, 800);
            stream.insert(stream.end(), independent.begin(), independent.end());
            stream.insert(stream.end(), dependent.begin(), dependent.end());
        }
        for (std::size_t piece : {1u, 188u, 4096u})
        {
            const auto bursts = pack(Format::eac3, stream, piece);
            assert(bursts.size() == 3);
            for (const auto &burst : bursts)
            {
                const auto &b = burst.bytes;
                assert(b.size() == 24576);
                assert(b[4] == 0x15 && b[5] == 0x00);
                assert(b[6] == (2000 & 0xff) && b[7] == 2000 >> 8);
                assert(b[8 + 1200] == 0x77 && b[9 + 1200] == 0x0b); /* dependent follows */
                assert(b[8 + 2000] == 0);
            }
        }
    }

    /* E-AC-3 with one block per frame gathers six frames per burst. */
    {
        auto frame = eac3_frame(false, 400);
        frame[4] = 0x00 | 0x0f; /* numblkscod 0: one block */
        std::vector<std::uint8_t> stream;
        for (int i = 0; i < 12; ++i)
            stream.insert(stream.end(), frame.begin(), frame.end());
        const auto bursts = pack(Format::eac3, stream, 333);
        assert(bursts.size() == 2);
        assert(bursts[0].bytes[6] == (2400 & 0xff) && bursts[0].bytes[7] == 2400 >> 8);
    }

    /* DTS: core only, the DTS-HD extension skipped, type by frame length. */
    {
        std::vector<std::uint8_t> core(1006, 0x44);
        core[0] = 0x7f, core[1] = 0xfe, core[2] = 0x80, core[3] = 0x01;
        const unsigned blocks = 15, size = 1006 - 1; /* 512 samples */
        /* frame type 1, deficit 31, no CRC, then 7 bits of blocks-1 and 14 of size-1 */
        core[4] = static_cast<std::uint8_t>(0x80 | 0x7c | ((blocks >> 6) & 1));
        core[5] = static_cast<std::uint8_t>(((blocks & 0x3f) << 2) | ((size >> 12) & 3));
        core[6] = static_cast<std::uint8_t>(size >> 4);
        core[7] = static_cast<std::uint8_t>((size << 4) & 0xf0);
        core[8] = 13 << 2;                        /* 48 kHz */
        std::vector<std::uint8_t> hd(3000, 0x7f); /* would false-sync if scanned */
        hd[0] = 0x64, hd[1] = 0x58, hd[2] = 0x20, hd[3] = 0x25;
        /* user 8 bits, index 2, size type 0, header 8 bits, frame 16 bits */
        const unsigned fsize = 3000 - 1;
        std::uint64_t bits = (std::uint64_t{0x20} << 48) | (std::uint64_t{0x60} << 37) |
                             (std::uint64_t{fsize} << 21);
        for (int i = 0; i < 7; ++i)
            hd[4 + i] = static_cast<std::uint8_t>(bits >> (48 - 8 * i));
        std::vector<std::uint8_t> stream;
        for (int i = 0; i < 4; ++i)
        {
            stream.insert(stream.end(), core.begin(), core.end());
            stream.insert(stream.end(), hd.begin(), hd.end());
        }
        assert(dts_substream_bytes(hd.data(), hd.size()) == 3000);
        for (std::size_t piece : {1u, 188u, 9000u})
        {
            const auto bursts = pack(Format::dts, stream, piece);
            assert(bursts.size() == 4);
            const auto &b = bursts[0].bytes;
            assert(b.size() == 2048);
            assert(b[4] == 0x0b && b[5] == 0x00);
            assert(b[6] == ((1006 * 8) & 0xff) && b[7] == (1006 * 8) >> 8);
            assert(b[8] == 0xfe && b[9] == 0x7f);
        }
    }
    std::puts("iec61937 tests passed");
}
