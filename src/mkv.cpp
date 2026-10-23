/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "mkv.hpp"

#include <algorithm>
#include <cstring>

namespace slopfin::mkv
{
namespace
{
/* Larger than any video frame; anything bigger means the bytes are wrong. */
constexpr std::uint64_t kMaxElement = 64u * 1024u * 1024u;

enum : std::uint32_t
{
    kDocType = 0x4282,
    kSeek = 0x4DBB,
    kSeekId = 0x53AB,
    kSeekPosition = 0x53AC,
    kTimestampScale = 0x2AD7B1,
    kDuration = 0x4489,
    kTrackEntry = 0xAE,
    kTrackNumber = 0xD7,
    kTrackType = 0x83,
    kCodecId = 0x86,
    kCodecPrivate = 0x63A2,
    kVideo = 0xE0,
    kPixelWidth = 0xB0,
    kPixelHeight = 0xBA,
    kAudio = 0xE1,
    kSamplingFrequency = 0xB5,
    kChannels = 0x9F,
    kContentEncodings = 0x6D80,
    kCodecDelay = 0x56AA,
    kCuePoint = 0xBB,
    kCueTime = 0xB3,
    kCueTrackPositions = 0xB7,
    kCueTrack = 0xF7,
    kCueClusterPosition = 0xF1,
    kTimestamp = 0xE7,
    kSimpleBlock = 0xA3,
    kBlockGroup = 0xA0,
    kBlock = 0xA1,
    kBlockDuration = 0x9B,
    kReferenceBlock = 0xFB,
    kAttachments = 0x1941A469,
    kChapters = 0x1043A770,
    kTags = 0x1254C367,
};

std::uint64_t uint_value(const std::uint8_t *data, std::size_t size) noexcept
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size && i < 8; ++i)
        value = value << 8 | data[i];
    return value;
}

double float_value(const std::uint8_t *data, std::size_t size) noexcept
{
    const std::uint64_t bits = uint_value(data, size);
    if (size == 4)
    {
        const auto narrow = static_cast<std::uint32_t>(bits);
        float value;
        std::memcpy(&value, &narrow, sizeof(value));
        return value;
    }
    if (size == 8)
    {
        double value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    return 0.0;
}

/* Calls visit(id, body, size) for each child; false when the bytes are malformed. */
template <typename Visit> bool each_child(const std::uint8_t *data, std::size_t size, Visit visit)
{
    std::size_t at = 0;
    while (at < size)
    {
        Header header;
        if (!read_header(data + at, size - at, header))
            return false;
        at += static_cast<std::size_t>(header.length);
        const std::uint64_t body = header.unknown_size ? size - at : header.size;
        if (body > size - at)
            return false;
        visit(header.id, data + at, static_cast<std::size_t>(body));
        at += static_cast<std::size_t>(body);
    }
    return true;
}

bool level_one(std::uint32_t id) noexcept
{
    return id == kSeekHead || id == kInfo || id == kTracks || id == kCues || id == kCluster ||
           id == kAttachments || id == kChapters || id == kTags || id == kEbml || id == kSegment;
}

void parse_info(const std::uint8_t *data, std::size_t size, Info &info)
{
    info.have_info = each_child(data, size,
                                [&](std::uint32_t id, const std::uint8_t *body, std::size_t length)
                                {
                                    if (id == kTimestampScale)
                                        info.timestamp_scale = uint_value(body, length);
                                    else if (id == kDuration)
                                        info.duration = float_value(body, length);
                                });
    if (info.timestamp_scale == 0)
        info.timestamp_scale = 1000000;
}

void parse_encodings(const std::uint8_t *data, std::size_t size, Track &track)
{
    track.encoded = true;
    unsigned count = 0;
    bool valid = true;
    int algorithm = -1;
    const bool parsed = each_child(
        data, size,
        [&](std::uint32_t id, const std::uint8_t *body, std::size_t bytes)
        {
            if (id != 0x6240)
            {
                valid = false;
                return;
            } // ContentEncoding
            ++count;
            std::uint64_t order = 0, scope = 1, type = 0;
            valid &= each_child(
                body, bytes,
                [&](std::uint32_t field, const std::uint8_t *p, std::size_t n)
                {
                    if (field == 0x5031)
                        order = uint_value(p, n);
                    else if (field == 0x5032)
                        scope = uint_value(p, n);
                    else if (field == 0x5033)
                        type = uint_value(p, n);
                    else if (field == 0x5034) // ContentCompression; zlib is the default
                    {
                        algorithm = 0;
                        valid &= each_child(
                            p, n,
                            [&](std::uint32_t c, const std::uint8_t *v, std::size_t length)
                            {
                                if (c == 0x4254)
                                    algorithm = static_cast<int>(uint_value(v, length));
                                else if (c == 0x4255 && length <= 1024u * 1024u)
                                    track.compression_settings.assign(v, v + length);
                                else
                                    valid = false;
                            });
                    }
                    else
                        valid = false;
                });
            valid &= order == 0 && scope == 1 && type == 0;
        });
    if (parsed && valid && count == 1 && (algorithm == 0 || algorithm == 3))
        track.compression = algorithm;
}

void parse_tracks(const std::uint8_t *data, std::size_t size, Info &info)
{
    info.tracks.clear();
    info.have_tracks = each_child(
        data, size,
        [&](std::uint32_t id, const std::uint8_t *body, std::size_t length)
        {
            if (id != kTrackEntry)
                return;
            Track track;
            (void)each_child(
                body, length,
                [&](std::uint32_t field, const std::uint8_t *value, std::size_t bytes)
                {
                    if (field == kTrackNumber)
                        track.number = uint_value(value, bytes);
                    else if (field == kTrackType)
                        track.type = static_cast<int>(uint_value(value, bytes));
                    else if (field == kCodecId)
                        track.codec_id.assign(
                            reinterpret_cast<const char *>(value),
                            strnlen(reinterpret_cast<const char *>(value), bytes));
                    else if (field == kCodecPrivate)
                        track.codec_private.assign(value, value + bytes);
                    else if (field == kContentEncodings)
                        parse_encodings(value, bytes, track);
                    else if (field == kCodecDelay)
                        track.codec_delay = uint_value(value, bytes);
                    else if (field == kVideo)
                        (void)each_child(value, bytes,
                                         [&](std::uint32_t v, const std::uint8_t *p, std::size_t n)
                                         {
                                             if (v == kPixelWidth)
                                                 track.width = static_cast<int>(uint_value(p, n));
                                             else if (v == kPixelHeight)
                                                 track.height = static_cast<int>(uint_value(p, n));
                                         });
                    else if (field == kAudio)
                        (void)each_child(value, bytes,
                                         [&](std::uint32_t a, const std::uint8_t *p, std::size_t n)
                                         {
                                             if (a == kSamplingFrequency)
                                                 track.sample_rate = float_value(p, n);
                                             else if (a == kChannels)
                                                 track.channels =
                                                     static_cast<int>(uint_value(p, n));
                                         });
                });
            if (track.number != 0)
                info.tracks.push_back(std::move(track));
        });
}

void parse_seek_head(const std::uint8_t *data, std::size_t size, Info &info)
{
    (void)each_child(data, size,
                     [&](std::uint32_t id, const std::uint8_t *body, std::size_t length)
                     {
                         if (id != kSeek)
                             return;
                         std::uint32_t target = 0;
                         std::uint64_t position = 0;
                         bool positioned = false;
                         (void)each_child(
                             body, length,
                             [&](std::uint32_t field, const std::uint8_t *value, std::size_t bytes)
                             {
                                 if (field == kSeekId)
                                     target = static_cast<std::uint32_t>(uint_value(value, bytes));
                                 else if (field == kSeekPosition)
                                 {
                                     position = uint_value(value, bytes);
                                     positioned = true;
                                 }
                             });
                         if (!positioned)
                             return;
                         const std::uint64_t at = info.segment_data + position;
                         if (target == kInfo && info.info_at == 0)
                             info.info_at = at;
                         else if (target == kTracks && info.tracks_at == 0)
                             info.tracks_at = at;
                         else if (target == kCues && info.cues_at == 0)
                             info.cues_at = at;
                     });
}
} // namespace

bool read_header(const std::uint8_t *data, std::size_t available, Header &out) noexcept
{
    if (available < 2)
        return false;
    int id_length = 1;
    while (id_length <= 4 && !(data[0] & (0x80 >> (id_length - 1))))
        ++id_length;
    if (id_length > 4 || static_cast<std::size_t>(id_length) >= available)
        return false;
    std::uint32_t id = 0;
    for (int i = 0; i < id_length; ++i)
        id = id << 8 | data[i];
    const std::uint8_t first = data[id_length];
    int size_length = 1;
    while (size_length <= 8 && !(first & (0x80 >> (size_length - 1))))
        ++size_length;
    if (size_length > 8 || static_cast<std::size_t>(id_length + size_length) > available)
        return false;
    std::uint64_t size = first & (0xff >> size_length);
    bool all_ones = size == static_cast<std::uint64_t>(0xff >> size_length);
    for (int i = 1; i < size_length; ++i)
    {
        size = size << 8 | data[id_length + i];
        all_ones &= data[id_length + i] == 0xff;
    }
    out.id = id;
    out.size = size;
    out.length = id_length + size_length;
    out.unknown_size = all_ones;
    return true;
}

bool parse_head(const std::uint8_t *data, std::size_t size, Info &info)
{
    info = Info{};
    Header header;
    if (!read_header(data, size, header) || header.id != kEbml || header.unknown_size ||
        header.size > size - static_cast<std::size_t>(header.length))
        return false;
    bool matroska = false;
    (void)each_child(data + header.length, static_cast<std::size_t>(header.size),
                     [&](std::uint32_t id, const std::uint8_t *body, std::size_t length)
                     {
                         if (id != kDocType)
                             return;
                         const std::string type(
                             reinterpret_cast<const char *>(body),
                             strnlen(reinterpret_cast<const char *>(body), length));
                         matroska = type == "matroska" || type == "webm";
                     });
    std::size_t at = static_cast<std::size_t>(header.length + header.size);
    if (!matroska || !read_header(data + at, size - at, header) || header.id != kSegment)
        return false;
    at += static_cast<std::size_t>(header.length);
    info.segment_data = at;
    while (at < size)
    {
        if (!read_header(data + at, size - at, header))
            break;
        if (header.id == kCluster)
        {
            info.first_cluster = at;
            break;
        }
        if (header.unknown_size ||
            header.size > size - at - static_cast<std::size_t>(header.length))
            break; /* continues past what was read */
        const std::uint8_t *body = data + at + header.length;
        const auto length = static_cast<std::size_t>(header.size);
        if (header.id == kSeekHead)
            parse_seek_head(body, length, info);
        else if (header.id == kInfo)
            parse_info(body, length, info);
        else if (header.id == kTracks)
            parse_tracks(body, length, info);
        at += static_cast<std::size_t>(header.length) + length;
    }
    return true;
}

bool parse_element(const std::uint8_t *data, std::size_t size, Info &info)
{
    Header header;
    if (!read_header(data, size, header) || header.unknown_size ||
        header.size > size - static_cast<std::size_t>(header.length))
        return false;
    const std::uint8_t *body = data + header.length;
    const auto length = static_cast<std::size_t>(header.size);
    if (header.id == kInfo)
        parse_info(body, length, info);
    else if (header.id == kTracks)
        parse_tracks(body, length, info);
    else
        return false;
    return true;
}

std::vector<CuePoint> parse_cues(const std::uint8_t *data, std::size_t size, const Info &info,
                                 std::uint64_t track)
{
    Header header;
    if (!read_header(data, size, header) || header.id != kCues || header.unknown_size ||
        header.size > size - static_cast<std::size_t>(header.length))
        return {};
    std::vector<CuePoint> mine, any;
    (void)each_child(data + header.length, static_cast<std::size_t>(header.size),
                     [&](std::uint32_t id, const std::uint8_t *body, std::size_t length)
                     {
                         if (id != kCuePoint)
                             return;
                         std::uint64_t time = 0;
                         (void)each_child(
                             body, length,
                             [&](std::uint32_t field, const std::uint8_t *value, std::size_t bytes)
                             {
                                 if (field == kCueTime)
                                     time = uint_value(value, bytes);
                                 else if (field == kCueTrackPositions)
                                 {
                                     std::uint64_t cue_track = 0, cluster = 0;
                                     bool found = false;
                                     (void)each_child(
                                         value, bytes,
                                         [&](std::uint32_t f, const std::uint8_t *p, std::size_t n)
                                         {
                                             if (f == kCueTrack)
                                                 cue_track = uint_value(p, n);
                                             else if (f == kCueClusterPosition)
                                             {
                                                 cluster = uint_value(p, n);
                                                 found = true;
                                             }
                                         });
                                     if (!found)
                                         return;
                                     const CuePoint point{time, info.segment_data + cluster};
                                     (cue_track == track ? mine : any).push_back(point);
                                 }
                             });
                     });
    std::vector<CuePoint> &chosen = mine.empty() ? any : mine;
    std::sort(chosen.begin(), chosen.end(),
              [](const CuePoint &a, const CuePoint &b) { return a.time < b.time; });
    return std::move(chosen);
}

std::uint64_t cluster_for(const std::vector<CuePoint> &cues, const Info &info,
                          double seconds) noexcept
{
    if (cues.empty())
        return 0;
    const double target = seconds * 1e9 / static_cast<double>(info.timestamp_scale);
    std::uint64_t chosen = cues.front().cluster;
    for (const CuePoint &cue : cues)
    {
        if (static_cast<double>(cue.time) > target)
            break;
        chosen = cue.cluster;
    }
    return chosen;
}

const Track *nth_track(const Info &info, int type, int ordinal) noexcept
{
    for (const Track &track : info.tracks)
        if (track.type == type && ordinal-- == 0)
            return &track;
    return nullptr;
}

ts::Codec codec_of(const Track &track) noexcept
{
    if (track.encoded)
        return ts::Codec::unknown;
    const std::string &id = track.codec_id;
    if (id == "V_MPEG4/ISO/AVC")
        return ts::Codec::h264;
    if (id == "V_MPEGH/ISO/HEVC")
        return ts::Codec::hevc;
    if (id.rfind("A_AAC", 0) == 0)
        return ts::Codec::aac_adts;
    if (id == "A_AC3")
        return ts::Codec::ac3;
    if (id == "A_EAC3")
        return ts::Codec::eac3;
    if (id.rfind("A_DTS", 0) == 0)
        return ts::Codec::dts;
    if (id == "A_TRUEHD")
        return ts::Codec::truehd;
    if (id == "A_MPEG/L3")
        return ts::Codec::mp3;
    return ts::Codec::unknown;
}

bool Demuxer::configure(const Info &info, const Track &video, const Track *audio,
                        const Track *subtitle)
{
    if (subtitle)
    {
        if ((subtitle->encoded && subtitle->compression < 0) ||
            (subtitle->codec_id != "S_HDMV/PGS" && subtitle->codec_id != "S_VOBSUB"))
            return false;
        subtitle_number_ = subtitle->number;
    }
    scale_ = info.timestamp_scale;
    video_number_ = video.number;
    video_codec_ = codec_of(video);
    if (video_codec_ == ts::Codec::h264)
        sets_ = annexb::parse_avcc(video.codec_private.data(), video.codec_private.size());
    else if (video_codec_ == ts::Codec::hevc)
        sets_ = annexb::parse_hvcc(video.codec_private.data(), video.codec_private.size());
    if (!sets_.valid)
        return false;
    if (audio != nullptr)
    {
        audio_number_ = audio->number;
        audio_codec_ = codec_of(*audio);
        audio_delay_ = static_cast<std::int64_t>(audio->codec_delay * 9 / 100000);
        if (audio_codec_ == ts::Codec::unknown)
            return false;
        if (audio_codec_ == ts::Codec::aac_adts)
        {
            if (!audio->codec_private.empty())
                aac_ = annexb::parse_audio_specific_config(audio->codec_private.data(),
                                                           audio->codec_private.size());
            else
            {
                /* Old files name the profile in the codec ID instead. */
                const bool sbr = audio->codec_id.find("SBR") != std::string::npos;
                const int rate = static_cast<int>(audio->sample_rate) / (sbr ? 2 : 1);
                aac_.object_type = audio->codec_id.find("/MAIN") != std::string::npos ? 1 : 2;
                aac_.frequency_index = annexb::aac_frequency_index(rate);
                aac_.channels = audio->channels;
                aac_.valid = aac_.frequency_index >= 0 && aac_.channels >= 1 && aac_.channels <= 7;
            }
            if (!aac_.valid)
                return false;
        }
    }
    return true;
}

void Demuxer::feed(const std::uint8_t *bytes, std::size_t count) noexcept
{
    if (broken_)
        return;
    /* Skipped elements (an index, attachments) are discarded as they pass. */
    if (skip_ > 0 && consumed_ == pending_.size())
    {
        const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(skip_, count));
        skip_ -= take;
        position_ += take;
        bytes += take;
        count -= take;
    }
    if (consumed_ > 0 && (consumed_ == pending_.size() || consumed_ > 1024u * 1024u))
    {
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(consumed_));
        consumed_ = 0;
    }
    pending_.insert(pending_.end(), bytes, bytes + count);
    parse();
}

void Demuxer::parse() noexcept
{
    while (!broken_)
    {
        std::size_t available = pending_.size() - consumed_;
        if (skip_ > 0)
        {
            const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(skip_, available));
            skip_ -= take;
            consumed_ += take;
            position_ += take;
            if (skip_ > 0)
                return;
            available -= take;
        }
        if (in_cluster_ && !cluster_unknown_ && position_ >= cluster_end_)
            in_cluster_ = false;
        const std::uint8_t *at = pending_.data() + consumed_;
        Header header;
        if (!read_header(at, available, header))
        {
            /* Twelve bytes always hold a header; failing then is not a wait. */
            broken_ = available >= 12;
            return;
        }
        const auto consume = [this](std::uint64_t bytes)
        {
            consumed_ += static_cast<std::size_t>(bytes);
            position_ += bytes;
        };
        if (header.id == kSegment)
        {
            consume(static_cast<std::uint64_t>(header.length));
            continue;
        }
        if (header.id == kCluster)
        {
            consume(static_cast<std::uint64_t>(header.length));
            in_cluster_ = true;
            cluster_unknown_ = header.unknown_size;
            cluster_end_ = position_ + header.size;
            cluster_time_ = 0;
            continue;
        }
        const bool top = level_one(header.id);
        if (in_cluster_ && top)
            in_cluster_ = false; /* an unknown-size cluster ends where the next element begins */
        const bool wanted = in_cluster_ && (header.id == kTimestamp || header.id == kSimpleBlock ||
                                            header.id == kBlockGroup);
        if (header.unknown_size || header.size > (wanted ? kMaxElement : ~0ull >> 8))
        {
            broken_ = true;
            return;
        }
        if (!wanted)
        {
            consume(static_cast<std::uint64_t>(header.length));
            skip_ = header.size;
            continue;
        }
        const std::uint64_t whole = static_cast<std::uint64_t>(header.length) + header.size;
        if (available < whole)
            return; /* wait for the rest of the element */
        const std::uint8_t *body = at + header.length;
        const auto length = static_cast<std::size_t>(header.size);
        if (header.id == kTimestamp)
            cluster_time_ = uint_value(body, length);
        else if (header.id == kSimpleBlock)
            block(body, length, true, false);
        else
            block_group(body, length);
        consume(whole);
    }
}

void Demuxer::block_group(const std::uint8_t *data, std::size_t size) noexcept
{
    const std::uint8_t *payload = nullptr;
    std::size_t length = 0;
    bool referenced = false;
    std::uint64_t duration = 0;
    (void)each_child(data, size,
                     [&](std::uint32_t id, const std::uint8_t *body, std::size_t bytes)
                     {
                         if (id == kBlock)
                         {
                             payload = body;
                             length = bytes;
                         }
                         else if (id == kBlockDuration)
                             duration = uint_value(body, bytes);
                         else if (id == kReferenceBlock)
                             referenced = true;
                     });
    if (payload != nullptr)
        block(payload, length, false, !referenced, duration);
}

void Demuxer::block(const std::uint8_t *data, std::size_t size, bool simple, bool keyframe,
                    std::uint64_t duration) noexcept
{
    /* Track number: a size-style variable integer. */
    if (size < 4)
        return;
    int width = 1;
    while (width <= 8 && !(data[0] & (0x80 >> (width - 1))))
        ++width;
    if (width > 8 || static_cast<std::size_t>(width) + 3 > size)
        return;
    std::uint64_t number = data[0] & (0xff >> width);
    for (int i = 1; i < width; ++i)
        number = number << 8 | data[i];
    const bool video = number == video_number_;
    const bool subtitle = subtitle_number_ != 0 && number == subtitle_number_;
    if (!video && !subtitle && (audio_number_ == 0 || number != audio_number_))
        return;
    const auto relative = static_cast<std::int16_t>(data[width] << 8 | data[width + 1]);
    const std::uint8_t flags = data[width + 2];
    if (simple)
        keyframe = (flags & 0x80) != 0;
    std::size_t at = static_cast<std::size_t>(width) + 3;

    /* Lacing: several frames in one block, sizes written in one of three ways. */
    std::vector<std::size_t> sizes;
    const int lacing = (flags >> 1) & 3;
    if (lacing == 0)
        sizes.push_back(size - at);
    else
    {
        if (at >= size)
            return;
        const std::size_t count = static_cast<std::size_t>(data[at++]) + 1;
        std::size_t used = 0;
        if (lacing == 1) /* Xiph: runs of 255 */
        {
            for (std::size_t i = 0; i + 1 < count; ++i)
            {
                std::size_t frame = 0;
                for (;;)
                {
                    if (at >= size)
                        return;
                    const std::uint8_t piece = data[at++];
                    frame += piece;
                    if (piece != 255)
                        break;
                }
                sizes.push_back(frame);
                used += frame;
            }
        }
        else if (lacing == 3) /* EBML: a size, then signed differences */
        {
            std::int64_t previous = 0;
            for (std::size_t i = 0; i + 1 < count; ++i)
            {
                if (at >= size)
                    return;
                int n = 1;
                while (n <= 8 && !(data[at] & (0x80 >> (n - 1))))
                    ++n;
                if (n > 8 || at + static_cast<std::size_t>(n) > size)
                    return;
                std::uint64_t raw = data[at] & (0xff >> n);
                for (int j = 1; j < n; ++j)
                    raw = raw << 8 | data[at + static_cast<std::size_t>(j)];
                at += static_cast<std::size_t>(n);
                const std::int64_t value = i == 0 ? static_cast<std::int64_t>(raw)
                                                  : previous + static_cast<std::int64_t>(raw) -
                                                        ((std::int64_t{1} << (7 * n - 1)) - 1);
                if (value < 0)
                    return;
                sizes.push_back(static_cast<std::size_t>(value));
                used += static_cast<std::size_t>(value);
                previous = value;
            }
        }
        if (at > size || used > size - at)
            return;
        if (lacing == 2) /* fixed: equal sizes */
        {
            if ((size - at) % count != 0)
                return;
            sizes.assign(count, (size - at) / count);
        }
        else
            sizes.push_back(size - at - used);
    }

    std::int64_t time = static_cast<std::int64_t>(cluster_time_) + relative;
    if (time < 0)
        time = 0;
    ts::AccessUnit unit;
    unit.pts = time * static_cast<std::int64_t>(scale_) * 9 / 100000; /* ns to 90 kHz */
    unit.video = video;
    if (subtitle)
    {
        if (sizes.size() != 1 || duration > 86400000000000ull / scale_)
            return;
        unit.subtitle = true;
        unit.duration = static_cast<std::int64_t>(duration * scale_ * 9 / 100000);
        unit.data.assign(data + at, data + at + sizes[0]);
        ready_.push_back(std::move(unit));
        return;
    }
    if (video)
    {
        /* Decoding starts at a keyframe; anything before one cannot be shown. */
        if (!started_ && !keyframe)
            return;
        if (sizes.size() != 1 ||
            !annexb::convert(data + at, sizes[0], sets_, video_codec_ == ts::Codec::hevc, keyframe,
                             unit.data))
            return;
        started_ = true;
        ready_.push_back(std::move(unit));
        return;
    }
    /* Timestamps include the encoder's priming; the pictures' do not. */
    /* Well before zero is the encoder's priming, which a decoder discards
       anyway; within a millisecond is only Matroska's rounding. */
    unit.pts -= audio_delay_;
    std::size_t first = 0;
    if (unit.pts < -90)
    {
        if (sizes.size() == 1)
            return;
        /* A laced block: only its frames before zero are priming. AAC frames
           are 1024 samples; for other codecs the block is kept whole. */
        const int rate = annexb::aac_sample_rate(aac_.frequency_index);
        if (audio_codec_ == ts::Codec::aac_adts && rate > 0)
            for (; first < sizes.size() && unit.pts < -90; ++first)
            {
                at += sizes[first];
                unit.pts += 1024 * 90000 / rate;
            }
        if (first == sizes.size())
            return;
    }
    unit.pts = std::max<std::int64_t>(0, unit.pts);
    if (unit.pts < present_from_)
        return;
    for (std::size_t i = first; i < sizes.size(); ++i)
    {
        const std::size_t frame = sizes[i];
        if (audio_codec_ == ts::Codec::aac_adts)
        {
            if (!annexb::append_adts(aac_, data + at, frame, unit.data))
                return;
        }
        else
            unit.data.insert(unit.data.end(), data + at, data + at + frame);
        at += frame;
    }
    if (!unit.data.empty())
        ready_.push_back(std::move(unit));
}

bool Demuxer::next(ts::AccessUnit &out) noexcept
{
    if (ready_.empty())
        return false;
    out = std::move(ready_.front());
    ready_.pop_front();
    return true;
}

void Demuxer::finish() noexcept
{
    pending_.clear();
    consumed_ = 0;
}

} // namespace slopfin::mkv
