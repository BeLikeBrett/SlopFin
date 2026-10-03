/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/* SlopFin software audio feasibility probe. SPDX-License-Identifier: GPL-3.0-or-later */
#include "software_audio.hpp"
#ifndef SLOPFIN_SOFTWARE_AUDIO
namespace slopfin::software_audio
{
bool probe(const char *, std::FILE *report) noexcept
{
    std::fprintf(report, "Software audio probe is not included in this build.\n");
    return false;
}
} // namespace slopfin::software_audio
#else
#include "bigalloc.hpp"
#include <algorithm>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

    // FFmpeg's allocator prefix keeps decoder-owned buffers out of the small heap.
    void *slopfin_av_malloc(std::size_t size)
    {
        if (size > 256u * 1024u * 1024u)
            return nullptr;
        return slopfin::bigalloc::allocate(size);
    }
    void *slopfin_av_realloc(void *p, std::size_t size)
    {
        if (size > 256u * 1024u * 1024u)
            return nullptr;
        if (!size)
        {
            slopfin::bigalloc::release(p);
            return nullptr;
        }
        return slopfin::bigalloc::reallocate(p, size);
    }
    void slopfin_av_free(void *p)
    {
        slopfin::bigalloc::release(p);
    }
}

namespace slopfin::software_audio
{
bool probe(const char *name, std::FILE *report) noexcept
{
    const bool truehd = std::strcmp(name, "truehd_51") == 0;
    const bool dts = std::strcmp(name, "dts_51") == 0;
    const bool eac3 = std::strcmp(name, "eac3_51") == 0 || std::strcmp(name, "eac3_20") == 0;
    if (!truehd && !dts && !eac3)
        return false;
    constexpr std::size_t input_cap = 2u * 1024u * 1024u, pcm_cap = 8u * 1024u * 1024u;
    auto *input = static_cast<std::uint8_t *>(bigalloc::allocate(input_cap));
    struct Capture
    {
        std::uint8_t *pcm;
        std::size_t used = 0;
        unsigned channels = 0, frames = 0;
    };
    Capture capture{static_cast<std::uint8_t *>(bigalloc::allocate(pcm_cap))};
    const auto collect = [](void *opaque, const Pcm &pcm) noexcept -> bool
    {
        auto &c = *static_cast<Capture *>(opaque);
        const auto bytes = pcm.frames * pcm.channels * sizeof(std::int16_t);
        if ((c.channels && c.channels != pcm.channels) || bytes > pcm_cap - c.used)
            return false;
        c.channels = pcm.channels;
        std::memcpy(c.pcm + c.used, pcm.samples, bytes);
        c.used += bytes;
        ++c.frames;
        return true;
    };
    char path[160]{};
    std::snprintf(path, sizeof(path), "/data/slopfin-codec/%s.bin", name);
    const int fd = ::open(path, O_RDONLY);
    std::size_t bytes = 0;
    bool good = input && capture.pcm && fd >= 0;
    if (good)
    {
        while (bytes < input_cap)
        {
            const auto n = ::read(fd, input + bytes, input_cap - bytes);
            if (n <= 0)
                break;
            bytes += static_cast<std::size_t>(n);
        }
        good = bytes > 0 && bytes < input_cap;
    }
    if (fd >= 0)
        ::close(fd);
    Decoder decoder;
    const auto started = std::chrono::steady_clock::now();
    good = good && decoder.open(truehd ? Codec::truehd : dts ? Codec::dts : Codec::eac3);
    for (std::size_t at = 0; good && at < bytes; at += 188)
        good = decoder.feed(input + at, std::min<std::size_t>(188, bytes - at), at == 0 ? 0 : -1,
                            collect, &capture);
    if (good)
        good = decoder.finish(collect, &capture);
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    good = good && capture.used > 0;
    std::fprintf(report,
                 "Streaming software %s: %s, frames=%u channels=%u rate=48000 PCM=%zu "
                 "decode_elapsed=%.6fs\n",
                 name, good ? "decoded" : "FAILED", capture.frames, capture.channels, capture.used,
                 elapsed);
    if (good)
    {
        if (auto *out = std::fopen("/data/slopfin-software-audio.raw", "wb"))
        {
            good = std::fwrite(capture.pcm, 1, capture.used, out) == capture.used;
            std::fclose(out);
        }
        else
            good = false;
        if (auto *out = std::fopen("/data/slopfin-software-audio.txt", "wb"))
        {
            std::fprintf(out, "channels %u\nrate 48000\nframes %zu\n", capture.channels,
                         capture.used / (capture.channels * 2));
            std::fclose(out);
        }
        else
            good = false;
    }
    bigalloc::release(capture.pcm);
    bigalloc::release(input);
    return good;
}
} // namespace slopfin::software_audio
#endif
