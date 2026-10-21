/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Direct play of the fixtures in tests/media: 96x64, 24 fps, 4 seconds,
 * keyframes every second, B-frames. Their demuxed output was compared with
 * FFmpeg (h264/hevc_mp4toannexb, raw ADTS/AC-3, ffprobe timestamps) when they
 * were added; this checks the same properties without needing FFmpeg.
 */
#include "direct_source.hpp"
#include "seek_skip.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

/* The console allocator is flexible memory; the heap stands in for it here. */
namespace slopfin::bigalloc
{
void *allocate(std::size_t bytes) noexcept
{
    return std::malloc(bytes);
}
void release(void *pointer) noexcept
{
    std::free(pointer);
}
} // namespace slopfin::bigalloc

using namespace slopfin;

namespace
{
class FileRanges final : public direct::Ranges
{
  public:
    explicit FileRanges(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), {});
        assert(!bytes.empty());
    }
    bool read(std::uint64_t offset, std::size_t size, std::uint8_t *out,
              std::size_t &got) noexcept override
    {
        ++reads;
        got = 0;
        if (offset > bytes.size())
            return false;
        got = std::min<std::size_t>(size, bytes.size() - offset);
        std::memcpy(out, bytes.data() + offset, got);
        return true;
    }
    std::vector<std::uint8_t> bytes;
    int reads = 0;
};

struct Result
{
    std::vector<ts::AccessUnit> video, audio;
};

/* Feeds the file from the chosen offset in uneven pieces, as a socket would. */
Result play(FileRanges &file, direct::Opened &opened)
{
    Result result;
    std::size_t at = opened.offset;
    unsigned seed = 11;
    while (at < file.bytes.size())
    {
        seed = seed * 1103515245u + 12345u;
        const std::size_t piece = std::min<std::size_t>(1 + seed % 9000, file.bytes.size() - at);
        opened.demuxer->feed(file.bytes.data() + at, piece);
        at += piece;
        ts::AccessUnit unit;
        while (opened.demuxer->next(unit))
            (unit.video ? result.video : result.audio).push_back(std::move(unit));
    }
    opened.demuxer->finish();
    assert(!opened.demuxer->broken());
    return result;
}

bool starts_with_delimiter(const ts::AccessUnit &unit, bool hevc)
{
    static const std::uint8_t h264[] = {0, 0, 0, 1, 0x09};
    static const std::uint8_t h265[] = {0, 0, 0, 1, 0x46, 0x01};
    return hevc ? unit.data.size() > 6 && std::memcmp(unit.data.data(), h265, 6) == 0
                : unit.data.size() > 5 && std::memcmp(unit.data.data(), h264, 5) == 0;
}

/* A parameter set follows the delimiter: what makes a unit decodable alone. */
bool carries_parameters(const ts::AccessUnit &unit, bool hevc)
{
    const std::size_t at = hevc ? 7 : 6;
    if (unit.data.size() < at + 5 || unit.data[at] != 0 || unit.data[at + 3] != 1)
        return false;
    const std::uint8_t nal = unit.data[at + 4];
    return hevc ? ((nal >> 1) & 0x3f) == 32 : (nal & 0x1f) == 7;
}

void check(const char *name, const char *container, int ordinal, const char *codec, bool hevc,
           std::size_t audio_units)
{
    FileRanges file(std::string("tests/media/") + name);
    direct::Source source;
    source.key = name;
    source.container = container;
    source.size = file.bytes.size();
    source.audio_ordinal = ordinal;
    source.audio_codec = codec;

    /* From the start: every picture, in decode order, each beginning cleanly. */
    direct::Opened opened;
    assert(direct::open(source, 0.0, file, opened));
    const int index_reads = file.reads;
    Result all = play(file, opened);
    assert(all.video.size() == 96);
    assert(carries_parameters(all.video.front(), hevc));
    std::vector<std::int64_t> shown;
    for (const ts::AccessUnit &unit : all.video)
    {
        assert(starts_with_delimiter(unit, hevc));
        shown.push_back(unit.pts);
    }
    std::sort(shown.begin(), shown.end());
    /* Matroska keeps whole milliseconds: half of one is 45 ticks. */
    for (std::size_t i = 0; i < shown.size(); ++i)
        assert(std::llabs(shown[i] - static_cast<std::int64_t>(i) * 3750) <= 45);
    if (all.audio.size() != audio_units)
        std::fprintf(stderr, "%s audio %d: %zu audio units\n", name, ordinal, all.audio.size());
    assert(all.audio.size() == audio_units);
    /* Unreferenced pictures: never the keyframe, never a stream's majority of
       references; FFmpeg's encoders leave some B-frames unreferenced. */
    video::SkipState state;
    std::size_t disposable = 0;
    for (std::size_t i = 0; i < all.video.size(); ++i)
    {
        const bool spare =
            video::disposable(all.video[i].data.data(), all.video[i].data.size(), hevc, state);
        assert(!(spare && carries_parameters(all.video[i], hevc)));
        disposable += spare ? 1 : 0;
    }
    assert(disposable > 0 && disposable < all.video.size() / 2 + all.video.size() / 4);
    for (std::size_t i = 1; i < all.audio.size(); ++i)
        assert(all.audio[i].pts > all.audio[i - 1].pts);
    if (std::strcmp(codec, "aac") == 0)
        for (const ts::AccessUnit &unit : all.audio)
            assert(unit.data[0] == 0xff && (unit.data[1] & 0xf6) == 0xf0);
    else
        for (const ts::AccessUnit &unit : all.audio)
            assert(unit.data[0] == 0x0b && unit.data[1] == 0x77); /* AC-3 sync */

    /* A seek: the keyframe before, audio from the target, no index re-read. */
    assert(direct::open(source, 2.3, file, opened));
    assert(file.reads == index_reads);
    Result seek = play(file, opened);
    assert(carries_parameters(seek.video.front(), hevc));
    assert(seek.video.front().pts <= 207000 && seek.video.front().pts >= 90000);
    assert(!seek.audio.empty() && seek.audio.front().pts >= 207000 &&
           seek.audio.front().pts < 207000 + 3000);
    std::printf("%s audio %d (%s): %zu video (%zu unreferenced), %zu audio; seek to 2.3 s "
                "starts at %.3f s\n",
                name, ordinal, codec, all.video.size(), disposable, all.audio.size(),
                static_cast<double>(seek.video.front().pts) / 90000.0);
}
} // namespace

int main()
{
    /* FFmpeg's packet counts less the one priming frame before zero. */
    check("h264_aac_ac3_srt.mkv", "mkv", 0, "aac", false, 188);
    check("h264_aac_ac3_srt.mkv", "mkv", 1, "ac3", false, 124);
    check("hevc_ac3.mp4", "mp4", 0, "ac3", true, 124);
    check("h264_aac_faststart.mp4", "mp4", 0, "aac", false, 188);

    /* Audio laced several frames to a block, as mkvmerge writes it, gives the
       same frames as the unlaced original. These copies have no Cues. */
    for (int ordinal = 0; ordinal < 2; ++ordinal)
    {
        std::vector<std::uint8_t> expected;
        for (const char *name : {"h264_aac_ac3_srt.mkv", "h264_aac_ac3_srt_xiph_laced.mkv",
                                 "h264_aac_ac3_srt_ebml_laced.mkv"})
        {
            FileRanges laced(std::string("tests/media/") + name);
            direct::Source source;
            source.key = std::string(name) + std::to_string(ordinal);
            source.container = "mkv";
            source.size = laced.bytes.size();
            source.audio_ordinal = ordinal;
            source.audio_codec = ordinal == 0 ? "aac" : "ac3";
            direct::Opened opened;
            assert(direct::open(source, 0.0, laced, opened));
            std::vector<std::uint8_t> audio;
            for (const ts::AccessUnit &unit : play(laced, opened).audio)
                audio.insert(audio.end(), unit.data.begin(), unit.data.end());
            if (expected.empty())
                expected = audio;
            assert(!audio.empty() && audio == expected);
            /* Without an index, starting later is refused rather than guessed. */
            if (std::strstr(name, "laced") != nullptr)
                assert(!direct::open(source, 2.0, laced, opened) && !opened.why.empty());
        }
    }

    /* Refusals, each with a reason: wrong track, wrong codec, not a container. */
    FileRanges file("tests/media/h264_aac_ac3_srt.mkv");
    direct::Source source;
    source.key = "refusals";
    source.container = "mkv";
    source.size = file.bytes.size();
    source.audio_ordinal = 2;
    source.audio_codec = "aac";
    direct::Opened opened;
    assert(!direct::open(source, 0.0, file, opened) && !opened.why.empty());
    source.audio_ordinal = 0;
    source.audio_codec = "eac3";
    assert(!direct::open(source, 0.0, file, opened) && !opened.why.empty());
    source.key = "as mp4";
    source.container = "mp4";
    source.audio_codec = "aac";
    assert(!direct::open(source, 0.0, file, opened) && !opened.why.empty());
    /* No audio: video alone plays. */
    source.key = "silent";
    source.container = "mkv";
    source.audio_ordinal = -1;
    source.audio_codec.clear();
    assert(direct::open(source, 0.0, file, opened));
    Result silent = play(file, opened);
    assert(silent.video.size() == 96 && silent.audio.empty());
    direct::forget();
    std::puts("direct play sources passed");
    return 0;
}
