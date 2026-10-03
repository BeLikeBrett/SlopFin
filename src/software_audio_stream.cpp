/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/* SlopFin streaming CPU audio decoder. SPDX-License-Identifier: GPL-3.0-or-later */
#include "software_audio.hpp"
#ifndef SLOPFIN_SOFTWARE_AUDIO
namespace slopfin::software_audio
{
Decoder::~Decoder() = default;
bool Decoder::open(Codec) noexcept
{
    return false;
}
bool Decoder::feed(const std::uint8_t *, std::size_t, std::int64_t, Sink, void *) noexcept
{
    return false;
}
bool Decoder::finish(Sink, void *) noexcept
{
    return false;
}
void Decoder::close() noexcept
{
}
} // namespace slopfin::software_audio
#else
#include "bigalloc.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <new>
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}
namespace slopfin::software_audio
{
struct Decoder::State
{
    AVCodecContext *context = nullptr;
    AVCodecParserContext *parser = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    SwrContext *swr = nullptr;
    AVChannelLayout layout{};
    int format = -1, rate = 0;
    bool failed = false, finished = false;
    std::size_t unframed = 0;
    std::int64_t position = 0, origin = -1, emitted = 0;
    static constexpr int kFrames = 16384;
    std::uint8_t input[4096 + AV_INPUT_BUFFER_PADDING_SIZE]{};
    std::int16_t output[kFrames * 8]{};
    int map[8]{};

    ~State()
    {
        swr_free(&swr);
        av_channel_layout_uninit(&layout);
        av_frame_free(&frame);
        av_packet_free(&packet);
        if (parser)
            av_parser_close(parser);
        avcodec_free_context(&context);
    }
    bool configure() noexcept
    {
        if (frame->sample_rate < 8000 || frame->sample_rate > 192000 ||
            frame->ch_layout.nb_channels < 1 || frame->ch_layout.nb_channels > 8 ||
            !av_channel_layout_check(&frame->ch_layout))
            return false;
        if (swr)
        {
            // A layout/rate change requires reopening the output deliberately.
            // Reject it instead of interpreting old PCM with a new map.
            return rate == frame->sample_rate && format == frame->format &&
                   av_channel_layout_compare(&layout, &frame->ch_layout) == 0;
        }
        if (av_channel_layout_copy(&layout, &frame->ch_layout) < 0)
            return false;
        rate = frame->sample_rate;
        format = frame->format;
        constexpr AVChannel slots[] = {
            AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT, AV_CHAN_FRONT_CENTER, AV_CHAN_LOW_FREQUENCY,
            AV_CHAN_BACK_LEFT,  AV_CHAN_BACK_RIGHT,  AV_CHAN_SIDE_LEFT,    AV_CHAN_SIDE_RIGHT};
        unsigned covered = 0;
        for (int i = 0; i < 8; ++i)
        {
            map[i] = std::max(-1, av_channel_layout_index_from_channel(&layout, slots[i]));
            if (map[i] >= 0)
                covered |= 1u << map[i];
        }
        if (layout.nb_channels == 1 && map[2] == 0)
        {
            map[0] = map[1] = 0;
            map[2] = -1;
        }
        // Unknown/height/wide layouts must not silently lose speakers.
        if (covered != (1u << layout.nb_channels) - 1)
            return false;
        return swr_alloc_set_opts2(&swr, &layout, AV_SAMPLE_FMT_S16, 48000, &layout,
                                   static_cast<AVSampleFormat>(format), rate, 0, nullptr) >= 0 &&
               swr_init(swr) >= 0;
    }
    bool emit(int count, Sink sink, void *opaque) noexcept
    {
        if (count < 0)
            return false;
        if (!count)
            return true;
        Pcm pcm{output,
                static_cast<std::size_t>(count),
                static_cast<unsigned>(layout.nb_channels),
                origin < 0 ? -1 : origin + av_rescale(emitted, 90000, 48000),
                {}};
        std::copy(map, map + 8, pcm.map);
        if (!sink(opaque, pcm))
            return false;
        emitted += count;
        return true;
    }
    bool drain(Sink sink, void *opaque) noexcept
    {
        int result;
        while ((result = avcodec_receive_frame(context, frame)) >= 0)
        {
            if (!configure())
                return false;
            if (origin < 0 && frame->pts != AV_NOPTS_VALUE && frame->pts >= 0)
                origin = frame->pts - av_rescale(emitted, 90000, 48000);
            const std::int64_t capacity = av_rescale_rnd(
                swr_get_delay(swr, rate) + frame->nb_samples, 48000, rate, AV_ROUND_UP);
            if (frame->nb_samples <= 0 || capacity > kFrames)
                return false;
            const std::uint8_t *planes[8]{};
            const int n = av_sample_fmt_is_planar(static_cast<AVSampleFormat>(format))
                              ? layout.nb_channels
                              : 1;
            for (int i = 0; i < n; ++i)
                planes[i] = frame->extended_data[i];
            std::uint8_t *out[] = {reinterpret_cast<std::uint8_t *>(output)};
            const int count = swr_convert(swr, out, kFrames, planes, frame->nb_samples);
            av_frame_unref(frame);
            if (!emit(count, sink, opaque))
                return false;
        }
        return result == AVERROR(EAGAIN) || result == AVERROR_EOF;
    }
    bool send(Sink sink, void *opaque) noexcept
    {
        packet->pts = parser->pts;
        packet->dts = parser->dts;
        int result = avcodec_send_packet(context, packet);
        if (result == AVERROR(EAGAIN))
        {
            if (!drain(sink, opaque))
                return false;
            result = avcodec_send_packet(context, packet);
        }
        return result >= 0 && drain(sink, opaque);
    }
};
Decoder::~Decoder()
{
    close();
}
void Decoder::close() noexcept
{
    if (!state_)
        return;
    state_->~State();
    bigalloc::release(state_);
    state_ = nullptr;
}
bool Decoder::open(Codec codec) noexcept
{
    close();
    auto *storage = bigalloc::allocate(sizeof(State));
    if (!storage)
        return false;
    state_ = new (storage) State;
    auto &s = *state_;
    const AVCodecID id = codec == Codec::truehd ? AV_CODEC_ID_TRUEHD
                         : codec == Codec::eac3 ? AV_CODEC_ID_EAC3
                                                : AV_CODEC_ID_DTS;
    const auto *decoder = avcodec_find_decoder(id);
    s.context = decoder ? avcodec_alloc_context3(decoder) : nullptr;
    s.parser = av_parser_init(id);
    s.packet = av_packet_alloc();
    s.frame = av_frame_alloc();
    if (s.context && s.parser && s.packet && s.frame)
    {
        s.context->thread_count = 1;
        s.context->thread_type = 0;
        s.context->pkt_timebase = {1, 90000};
        if (avcodec_open2(s.context, decoder, nullptr) >= 0)
            return true;
    }
    close();
    return false;
}
bool Decoder::feed(const std::uint8_t *data, std::size_t bytes, std::int64_t pts, Sink sink,
                   void *opaque) noexcept
{
    if (!state_ || state_->failed || state_->finished || !sink || (!data && bytes))
        return false;
    auto &s = *state_;
    int empty_steps = 0;
    while (bytes)
    {
        const auto chunk = std::min<std::size_t>(4096, bytes);
        std::memcpy(s.input, data, chunk);
        std::memset(s.input + chunk, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        std::size_t at = 0;
        while (at < chunk)
        {
            const int consumed =
                av_parser_parse2(s.parser, s.context, &s.packet->data, &s.packet->size,
                                 s.input + at, static_cast<int>(chunk - at),
                                 pts < 0 ? AV_NOPTS_VALUE : pts, AV_NOPTS_VALUE, s.position);
            if (consumed < 0 || consumed > static_cast<int>(chunk - at))
            {
                s.failed = true;
                return false;
            }
            // TrueHD may acquire major sync without consuming any bytes.
            empty_steps = consumed == 0 ? empty_steps + 1 : 0;
            s.unframed += consumed;
            if (empty_steps > 4 || s.unframed > 1024u * 1024u)
            {
                s.failed = true;
                return false;
            }
            if (consumed)
            {
                pts = -1;
                at += consumed;
                s.position += consumed;
            }
            if (s.packet->size)
            {
                s.unframed = 0;
                if (!s.send(sink, opaque))
                {
                    s.failed = true;
                    return false;
                }
            }
        }
        data += chunk;
        bytes -= chunk;
    }
    return true;
}
bool Decoder::finish(Sink sink, void *opaque) noexcept
{
    if (!state_ || state_->failed || !sink)
        return false;
    auto &s = *state_;
    if (s.finished)
        return true;
    s.finished = true;
    const int result = av_parser_parse2(s.parser, s.context, &s.packet->data, &s.packet->size,
                                        nullptr, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, s.position);
    bool good = result >= 0 && (!s.packet->size || s.send(sink, opaque));
    if (good)
        good = avcodec_send_packet(s.context, nullptr) >= 0 && s.drain(sink, opaque);
    if (good && s.swr)
    {
        std::uint8_t *out[] = {reinterpret_cast<std::uint8_t *>(s.output)};
        int count;
        do
        {
            count = swr_convert(s.swr, out, State::kFrames, nullptr, 0);
            good = s.emit(count, sink, opaque);
        } while (good && count > 0);
    }
    s.failed = !good;
    return good;
}
} // namespace slopfin::software_audio
#endif
