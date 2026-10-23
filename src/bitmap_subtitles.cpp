/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "bitmap_subtitles.hpp"
#include "bigalloc.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#ifdef SLOPFIN_BITMAP_SUBTITLES
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
    int stbi_zlib_decode_buffer(char *, int, const char *, int);
}
#endif
namespace slopfin::bitmap_subtitles
{
namespace
{
std::mutex mutex;
std::deque<std::shared_ptr<Frame>> frames;
std::size_t held = 0;
bool enabled = false;
/* An unexpectedly dense or corrupt track must not exhaust console memory. */
constexpr std::size_t kMaxBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaxFrames = 512;
} // namespace
bool available() noexcept
{
#ifdef SLOPFIN_BITMAP_SUBTITLES
    return true;
#else
    return false;
#endif
}
void reset() noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    frames.clear();
    held = 0;
    enabled = false;
}
void enable(bool value) noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    enabled = value;
}
bool append(std::shared_ptr<Frame> frame) noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!enabled)
        return true; /* switched off or to text while the reader was ahead */
    if (!frame || !std::isfinite(frame->start) || frame->end <= frame->start ||
        frame->bytes > kMaxBytes - held || frames.size() >= kMaxFrames)
        return false;
    /* PGS display sets replace the previous composition, including explicit clears. */
    if (!frames.empty())
    {
        if (frame->start < frames.back()->start)
            return false;
        frames.back()->end = std::min(frames.back()->end, frame->start);
    }
    held += frame->bytes;
    frames.push_back(std::move(frame));
    return true;
}
std::shared_ptr<const Frame> current(double seconds) noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!enabled || !std::isfinite(seconds))
        return {};
    /* Retain recent history for timing adjustments. Future frames remain queued. */
    while (frames.size() > 1 && frames[1]->start <= seconds - 60.0)
    {
        held -= frames.front()->bytes;
        frames.pop_front();
    }
    for (auto it = frames.rbegin(); it != frames.rend(); ++it)
        if ((*it)->start <= seconds)
            return seconds < (*it)->end && !(*it)->rects.empty() ? *it : nullptr;
    return {};
}
Decoder::~Decoder()
{
    bigalloc::release(unpacked_);
#ifdef SLOPFIN_BITMAP_SUBTITLES
    auto *ctx = static_cast<AVCodecContext *>(context_);
    avcodec_free_context(&ctx);
#endif
}
bool Decoder::open(const std::string &codec, const std::vector<std::uint8_t> &extra, int width,
                   int height, int compression, const std::vector<std::uint8_t> &settings) noexcept
{
#ifdef SLOPFIN_BITMAP_SUBTITLES
    if (context_ || extra.size() > 1024u * 1024u || settings.size() > 1024u * 1024u ||
        (compression != -1 && compression != 0 && compression != 3))
        return false;
    const AVCodecID id = codec == "pgssub"   ? AV_CODEC_ID_HDMV_PGS_SUBTITLE
                         : codec == "dvdsub" ? AV_CODEC_ID_DVD_SUBTITLE
                                             : AV_CODEC_ID_NONE;
    const AVCodec *decoder = avcodec_find_decoder(id);
    auto *ctx = decoder ? avcodec_alloc_context3(decoder) : nullptr;
    if (!ctx)
        return false;
    ctx->pkt_timebase = AVRational{1, 90000};
    ctx->width = width;
    ctx->height = height;
    if (!extra.empty())
    {
        ctx->extradata =
            static_cast<std::uint8_t *>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!ctx->extradata)
        {
            avcodec_free_context(&ctx);
            return false;
        }
        std::memcpy(ctx->extradata, extra.data(), extra.size());
        ctx->extradata_size = static_cast<int>(extra.size());
    }
    if (avcodec_open2(ctx, decoder, nullptr) < 0)
    {
        avcodec_free_context(&ctx);
        return false;
    }
    compression_ = compression;
    if (compression == 3)
        prefix_ = settings;
    context_ = ctx;
    return true;
#else
    (void)codec;
    (void)extra;
    (void)width;
    (void)height;
    (void)compression;
    (void)settings;
    return false;
#endif
}
bool Decoder::decode(const std::uint8_t *data, std::size_t size, std::int64_t pts,
                     std::int64_t duration) noexcept
{
#ifdef SLOPFIN_BITMAP_SUBTITLES
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!enabled)
            return true;
    }
    auto *ctx = static_cast<AVCodecContext *>(context_);
    if (!ctx || !data || size == 0 || size > 4u * 1024u * 1024u || pts < 0)
        return false;
    constexpr std::size_t kMaxPacket = 4u * 1024u * 1024u;
    if (compression_ == 0)
    {
        if (!unpacked_)
            unpacked_ = bigalloc::allocate(kMaxPacket);
        if (!unpacked_)
            return false;
        const int n =
            stbi_zlib_decode_buffer(static_cast<char *>(unpacked_), kMaxPacket,
                                    reinterpret_cast<const char *>(data), static_cast<int>(size));
        if (n <= 0)
            return false;
        data = static_cast<const std::uint8_t *>(unpacked_);
        size = static_cast<std::size_t>(n);
    }
    if (prefix_.size() > kMaxPacket - size)
        return false;
    AVPacket *packet = av_packet_alloc();
    if (!packet || av_new_packet(packet, static_cast<int>(size + prefix_.size())) < 0)
    {
        av_packet_free(&packet);
        return false;
    }
    if (!prefix_.empty())
        std::memcpy(packet->data, prefix_.data(), prefix_.size());
    std::memcpy(packet->data + prefix_.size(), data, size);
    packet->pts = pts;
    packet->duration = duration;
    AVSubtitle sub{};
    int got = 0;
    const int result = avcodec_decode_subtitle2(ctx, &sub, &got, packet);
    av_packet_free(&packet);
    bool ok = result >= 0;
    if (ok && got)
    {
        auto frame = std::make_shared<Frame>();
        const double base = sub.pts != AV_NOPTS_VALUE ? sub.pts / 1000000.0 : pts / 90000.0;
        frame->start = base + sub.start_display_time / 1000.0;
        frame->end = sub.end_display_time == UINT32_MAX ? std::numeric_limits<double>::infinity()
                                                        : base + sub.end_display_time / 1000.0;
        /* Matroska BlockDuration also bounds PGS, whose decoder awaits a clear event. */
        if (duration > 0)
            frame->end = std::min(frame->end, pts / 90000.0 + duration / 90000.0);
        frame->width = ctx->width;
        frame->height = ctx->height;
        ok = frame->width > 0 && frame->height > 0 && frame->width <= 8192 && frame->height <= 4320;
        for (unsigned i = 0; ok && i < sub.num_rects; ++i)
        {
            const AVSubtitleRect &r = *sub.rects[i];
            if (r.type != SUBTITLE_BITMAP || r.w <= 0 || r.h <= 0 || r.x < 0 || r.y < 0 ||
                r.x > frame->width - r.w || r.y > frame->height - r.h || r.linesize[0] < r.w ||
                !r.data[0] || !r.data[1] || r.nb_colors <= 0 || r.nb_colors > 256)
            {
                ok = false;
                break;
            }
            const auto bytes = static_cast<std::size_t>(r.w) * r.h * sizeof(std::uint32_t);
            if (bytes > kMaxBytes - frame->bytes)
            {
                ok = false;
                break;
            }
            auto *pixels = static_cast<std::uint32_t *>(bigalloc::allocate(bytes));
            if (!pixels)
            {
                ok = false;
                break;
            }
            Rect rect{r.x, r.y, r.w, r.h, {pixels, bigalloc::release}};
            for (int y = 0; y < r.h; ++y)
                for (int x = 0; x < r.w; ++x)
                {
                    const unsigned index = r.data[0][y * r.linesize[0] + x];
                    std::uint32_t color = 0;
                    if (index < static_cast<unsigned>(r.nb_colors))
                        std::memcpy(&color, r.data[1] + index * 4, 4);
                    pixels[y * r.w + x] = color;
                }
            frame->bytes += bytes;
            frame->rects.push_back(std::move(rect));
        }
        if (ok)
            ok = append(std::move(frame));
    }
    avsubtitle_free(&sub);
    return ok;
#else
    (void)data;
    (void)size;
    (void)pts;
    (void)duration;
    return false;
#endif
}
} // namespace slopfin::bitmap_subtitles
