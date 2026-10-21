/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Converts what MKV and MP4 store into what the PS5 decoders were proven
 * with through MPEG-TS:
 *
 * - Video samples are length-prefixed NAL units, with the parameter sets kept
 *   once in the codec header (avcC/hvcC). The decoder takes Annex-B: start
 *   codes, an access unit delimiter first, and parameter sets in-band before
 *   each keyframe, which is exactly what FFmpeg's TS muxer produced.
 * - AAC frames are raw, configured by an AudioSpecificConfig. The audio path
 *   takes ADTS, so each frame gets its 7-byte header.
 *
 * Free of PS5 headers so it can be tested on a workstation.
 */
#ifndef SLOPFIN_ANNEXB_HPP
#define SLOPFIN_ANNEXB_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace slopfin::annexb
{

struct ParameterSets
{
    std::vector<std::uint8_t> annexb; /* every VPS/SPS/PPS, each behind a start code */
    int length_size = 4;              /* bytes in each sample's NAL length prefix */
    bool valid = false;
};

namespace detail
{
inline void start_code(std::vector<std::uint8_t> &out)
{
    out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
}

inline unsigned read16(const std::uint8_t *p) noexcept
{
    return static_cast<unsigned>(p[0]) << 8 | p[1];
}
} // namespace detail

/* AVCDecoderConfigurationRecord (MKV CodecPrivate, MP4 avcC). */
inline ParameterSets parse_avcc(const std::uint8_t *data, std::size_t size)
{
    ParameterSets result;
    if (data == nullptr || size < 7 || data[0] != 1)
        return result;
    result.length_size = (data[4] & 3) + 1;
    std::size_t at = 5;
    for (int kind = 0; kind < 2; ++kind) /* SPS, then PPS */
    {
        if (at >= size)
            return {};
        const unsigned count = kind == 0 ? data[at] & 0x1f : data[at];
        ++at;
        for (unsigned i = 0; i < count; ++i)
        {
            if (at + 2 > size)
                return {};
            const unsigned length = detail::read16(data + at);
            at += 2;
            if (length == 0 || at + length > size)
                return {};
            detail::start_code(result.annexb);
            result.annexb.insert(result.annexb.end(), data + at, data + at + length);
            at += length;
        }
    }
    result.valid = !result.annexb.empty() && result.length_size != 3;
    return result;
}

/* HEVCDecoderConfigurationRecord (MKV CodecPrivate, MP4 hvcC). */
inline ParameterSets parse_hvcc(const std::uint8_t *data, std::size_t size)
{
    ParameterSets result;
    if (data == nullptr || size < 23 || data[0] != 1)
        return result;
    result.length_size = (data[21] & 3) + 1;
    const unsigned arrays = data[22];
    std::size_t at = 23;
    for (unsigned a = 0; a < arrays; ++a)
    {
        if (at + 3 > size)
            return {};
        const unsigned count = detail::read16(data + at + 1);
        at += 3;
        for (unsigned i = 0; i < count; ++i)
        {
            if (at + 2 > size)
                return {};
            const unsigned length = detail::read16(data + at);
            at += 2;
            if (length == 0 || at + length > size)
                return {};
            detail::start_code(result.annexb);
            result.annexb.insert(result.annexb.end(), data + at, data + at + length);
            at += length;
        }
    }
    result.valid = !result.annexb.empty() && result.length_size != 3;
    return result;
}

/*
 * Rewrites one length-prefixed sample as an Annex-B access unit. Returns false
 * for a malformed sample (a length running past its end), leaving `out` empty.
 */
inline bool convert(const std::uint8_t *sample, std::size_t size, const ParameterSets &sets,
                    bool hevc, bool keyframe, std::vector<std::uint8_t> &out)
{
    out.clear();
    if (!sets.valid || sample == nullptr)
        return false;
    const std::size_t prefix = static_cast<std::size_t>(sets.length_size);
    /* One pass to see what is there; parameter sets in-band are kept as they are. */
    bool has_parameters = false;
    bool has_delimiter = false;
    std::size_t payload = 0;
    for (std::size_t at = 0; at < size;)
    {
        if (at + prefix > size)
            return false;
        std::size_t length = 0;
        for (std::size_t i = 0; i < prefix; ++i)
            length = length << 8 | sample[at + i];
        at += prefix;
        if (length == 0 || length > size - at)
            return false;
        const unsigned type = hevc ? (sample[at] >> 1) & 0x3f : sample[at] & 0x1f;
        has_parameters |= hevc ? type >= 32 && type <= 34 : type == 7 || type == 8;
        has_delimiter |= hevc ? type == 35 : type == 9;
        payload += 4 + length;
        at += length;
    }
    if (payload == 0)
        return false;
    out.reserve(payload + 8 + (keyframe ? sets.annexb.size() : 0));
    if (!has_delimiter)
    {
        detail::start_code(out);
        if (hevc)
            out.insert(out.end(), {0x46, 0x01, 0x50}); /* AUD, any picture type */
        else
            out.insert(out.end(), {0x09, 0xf0});
    }
    if (keyframe && !has_parameters)
        out.insert(out.end(), sets.annexb.begin(), sets.annexb.end());
    for (std::size_t at = 0; at < size;)
    {
        std::size_t length = 0;
        for (std::size_t i = 0; i < prefix; ++i)
            length = length << 8 | sample[at + i];
        at += prefix;
        detail::start_code(out);
        out.insert(out.end(), sample + at, sample + at + length);
        at += length;
    }
    return true;
}

/* ------------------------------------------------------------------ AAC */

struct AacConfig
{
    int object_type = 2; /* AAC LC */
    int frequency_index = 3;
    int channels = 2;
    bool valid = false;
};

inline int aac_frequency_index(int sample_rate) noexcept
{
    static constexpr int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                     22050, 16000, 12000, 11025, 8000,  7350};
    for (int i = 0; i < 13; ++i)
        if (kRates[i] == sample_rate)
            return i;
    return -1;
}

inline int aac_sample_rate(int frequency_index) noexcept
{
    static constexpr int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                     22050, 16000, 12000, 11025, 8000,  7350};
    return frequency_index >= 0 && frequency_index < 13 ? kRates[frequency_index] : 0;
}

/* AudioSpecificConfig: the first two bytes carry all ADTS needs. */
inline AacConfig parse_audio_specific_config(const std::uint8_t *data, std::size_t size)
{
    AacConfig config;
    if (data == nullptr || size < 2)
        return config;
    config.object_type = data[0] >> 3;
    config.frequency_index = (data[0] & 7) << 1 | data[1] >> 7;
    config.channels = (data[1] >> 3) & 0xf;
    /* ADTS cannot express escaped object types or explicit rates; HE-AAC's
       signalling is implicit in an ADTS LC stream, as broadcasters send it. */
    if (config.object_type == 5 || config.object_type == 29)
        config.object_type = 2;
    config.valid = config.object_type >= 1 && config.object_type <= 4 &&
                   config.frequency_index < 13 && config.channels >= 1 && config.channels <= 7;
    return config;
}

/* Prepends the ADTS header for one raw frame. */
inline bool append_adts(const AacConfig &config, const std::uint8_t *frame, std::size_t size,
                        std::vector<std::uint8_t> &out)
{
    const std::size_t total = size + 7;
    if (!config.valid || frame == nullptr || size == 0 || total > 0x1fff)
        return false;
    const unsigned profile = static_cast<unsigned>(config.object_type - 1);
    const unsigned index = static_cast<unsigned>(config.frequency_index);
    const unsigned channels = static_cast<unsigned>(config.channels);
    out.push_back(0xff);
    out.push_back(0xf1); /* MPEG-4, layer 0, no CRC */
    out.push_back(static_cast<std::uint8_t>(profile << 6 | index << 2 | channels >> 2));
    out.push_back(static_cast<std::uint8_t>((channels & 3) << 6 | total >> 11));
    out.push_back(static_cast<std::uint8_t>(total >> 3));
    out.push_back(static_cast<std::uint8_t>((total & 7) << 5 | 0x1f));
    out.push_back(0xfc); /* buffer fullness 0x7ff, one raw data block */
    out.insert(out.end(), frame, frame + size);
    return true;
}

} // namespace slopfin::annexb

#endif
