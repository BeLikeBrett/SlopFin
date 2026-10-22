/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "direct_source.hpp"

#include "bigalloc.hpp"
#include "mkv.hpp"
#include "mp4.hpp"

#include <vector>

namespace slopfin::direct
{
namespace
{
constexpr std::size_t kHead = 512u * 1024u;
/* Far beyond any real index; a bigger claim means the bytes are wrong. */
constexpr std::uint64_t kMaxIndex = 64u * 1024u * 1024u;

/* A buffer in flexible memory: indexes run to megabytes, the heap does not. */
struct Big
{
    std::uint8_t *data = nullptr;
    std::size_t size = 0;
    Big() = default;
    Big(const Big &) = delete;
    Big &operator=(const Big &) = delete;
    ~Big()
    {
        reset();
    }
    bool allocate(std::size_t bytes) noexcept
    {
        reset();
        data = static_cast<std::uint8_t *>(bigalloc::allocate(bytes));
        size = data != nullptr ? bytes : 0;
        return data != nullptr;
    }
    void reset() noexcept
    {
        if (data != nullptr)
            bigalloc::release(data);
        data = nullptr;
        size = 0;
    }
};

/* The last title's index. Tracks of an MP4 point into its moov buffer. */
struct Kept
{
    std::string key;
    mkv::Info info;
    std::vector<mkv::CuePoint> cues;
    Big moov;
    mp4::Movie movie;
};

Kept &kept() noexcept
{
    static Kept value;
    return value;
}

bool read_exact(Ranges &ranges, std::uint64_t offset, std::size_t size, std::uint8_t *out) noexcept
{
    std::size_t got = 0;
    return ranges.read(offset, size, out, got) && got == size;
}

/* Reads a whole element or box whose header starts at `offset`. */
template <typename Header>
bool read_whole(Ranges &ranges, std::uint64_t offset, std::uint64_t file_size, Big &out,
                Header header) noexcept
{
    std::uint8_t first[16] = {};
    std::size_t got = 0;
    if (!ranges.read(offset, sizeof(first), first, got) || got < 8)
        return false;
    std::uint64_t whole = 0;
    if (!header(first, got, whole) || whole == 0 || whole > kMaxIndex ||
        (file_size > 0 && offset + whole > file_size))
        return false;
    return out.allocate(static_cast<std::size_t>(whole)) &&
           read_exact(ranges, offset, static_cast<std::size_t>(whole), out.data);
}

bool mkv_whole(const std::uint8_t *data, std::size_t size, std::uint64_t &whole)
{
    mkv::Header header;
    if (!mkv::read_header(data, size, header) || header.unknown_size)
        return false;
    whole = static_cast<std::uint64_t>(header.length) + header.size;
    return true;
}

bool mp4_whole(const std::uint8_t *data, std::size_t size, std::uint64_t &whole)
{
    mp4::Box box;
    if (!mp4::read_box(data, size, box) || box.size == 0)
        return false;
    whole = box.size;
    return true;
}

bool index_mkv(const Source &source, Ranges &ranges, Kept &index, std::string &why)
{
    Big head;
    std::size_t got = 0;
    if (!head.allocate(static_cast<std::size_t>(source.size > 0 && source.size < kHead ? source.size
                                                                                       : kHead)) ||
        !ranges.read(0, head.size, head.data, got) || !mkv::parse_head(head.data, got, index.info))
    {
        why = "not a readable Matroska header";
        return false;
    }
    mkv::Info &info = index.info;
    Big element;
    if (!info.have_tracks && info.tracks_at != 0 &&
        read_whole(ranges, info.tracks_at, source.size, element, mkv_whole))
        (void)mkv::parse_element(element.data, element.size, info);
    if (!info.have_info && info.info_at != 0 &&
        read_whole(ranges, info.info_at, source.size, element, mkv_whole))
        (void)mkv::parse_element(element.data, element.size, info);
    if (!info.have_tracks)
    {
        why = "no track list";
        return false;
    }
    std::uint64_t video = 0;
    for (const mkv::Track &track : info.tracks)
        if (track.type == 1 && video == 0)
            video = track.number;
    if (info.cues_at != 0 && read_whole(ranges, info.cues_at, source.size, element, mkv_whole))
        index.cues = mkv::parse_cues(element.data, element.size, info, video);
    return true;
}

bool index_mp4(const Source &source, Ranges &ranges, Kept &index, std::string &why)
{
    /* Top-level boxes, header by header: the moov may follow the media. */
    std::uint64_t at = 0, moov = 0;
    for (int boxes = 0; boxes < 64 && (source.size == 0 || at + 8 <= source.size); ++boxes)
    {
        std::uint8_t header[16] = {};
        std::size_t got = 0;
        mp4::Box box;
        if (!ranges.read(at, sizeof(header), header, got) || !mp4::read_box(header, got, box))
            break;
        if (box.type == mp4::fourcc("moov"))
        {
            moov = at;
            break;
        }
        if (box.type == mp4::fourcc("moof"))
        {
            why = "fragmented MP4";
            return false;
        }
        if (box.size == 0)
            break;
        at += box.size;
    }
    if (!read_whole(ranges, moov, source.size, index.moov, mp4_whole) || moov == 0 ||
        !mp4::parse_moov(index.moov.data, index.moov.size, index.movie))
    {
        why = "no readable MP4 index";
        return false;
    }
    if (index.movie.fragmented)
    {
        why = "fragmented MP4";
        return false;
    }
    return true;
}

ts::Codec player_codec(const std::string &jellyfin) noexcept
{
    return jellyfin == "aac"      ? ts::Codec::aac_adts
           : jellyfin == "ac3"    ? ts::Codec::ac3
           : jellyfin == "eac3"   ? ts::Codec::eac3
           : jellyfin == "dts"    ? ts::Codec::dts
           : jellyfin == "truehd" ? ts::Codec::truehd
           : jellyfin == "mp3"    ? ts::Codec::mp3
                                  : ts::Codec::unknown;
}

bool open_mkv(const Source &source, double seconds, Kept &index, Opened &out)
{
    const mkv::Info &info = index.info;
    const mkv::Track *video = nullptr;
    for (const mkv::Track &track : info.tracks)
        if (track.type == 1 && video == nullptr)
            video = &track;
    const mkv::Track *audio =
        source.audio_ordinal >= 0 ? mkv::nth_track(info, 2, source.audio_ordinal) : nullptr;
    if (video == nullptr || (source.audio_ordinal >= 0 && audio == nullptr))
    {
        out.why = "the selected tracks are not in the file";
        return false;
    }
    if (audio != nullptr && mkv::codec_of(*audio) != player_codec(source.audio_codec))
    {
        out.why = "audio track " + audio->codec_id + " is not the expected " + source.audio_codec;
        return false;
    }
    const mkv::Track *subtitle =
        source.subtitle_ordinal >= 0 ? mkv::nth_track(info, 17, source.subtitle_ordinal) : nullptr;
    if (source.subtitle_ordinal >= 0 &&
        (!subtitle || (source.subtitle_codec == "pgssub" ? subtitle->codec_id != "S_HDMV/PGS"
                                                         : subtitle->codec_id != "S_VOBSUB")))
    {
        out.why = "the selected picture subtitle is not in the file";
        return false;
    }
    if (subtitle)
    {
        out.subtitle_codec = source.subtitle_codec;
        out.subtitle_private = subtitle->codec_private;
        out.subtitle_compression = subtitle->compression;
        out.subtitle_compression_settings = subtitle->compression_settings;
        /* VobSub CodecPrivate supplies the DVD canvas; do not substitute 4K video dimensions. */
        out.subtitle_width = source.subtitle_codec == "dvdsub" ? 0 : video->width;
        out.subtitle_height = source.subtitle_codec == "dvdsub" ? 0 : video->height;
    }
    auto demuxer = std::make_unique<mkv::Demuxer>();
    if (!demuxer->configure(info, *video, audio, subtitle))
    {
        out.why = "unsupported track setup (" + video->codec_id + ")";
        return false;
    }
    std::uint64_t offset = info.first_cluster != 0 ? info.first_cluster : info.segment_data;
    if (seconds > 0.5)
    {
        const std::uint64_t cluster = mkv::cluster_for(index.cues, info, seconds);
        if (cluster == 0)
        {
            out.why = "no seek index";
            return false;
        }
        offset = cluster;
    }
    demuxer->present_from(static_cast<std::int64_t>(seconds * 90000.0));
    out.offset = offset;
    out.demuxer = std::move(demuxer);
    return true;
}

bool open_mp4(const Source &source, double seconds, Kept &index, Opened &out)
{
    const mp4::Movie &movie = index.movie;
    const mp4::Track *video = nullptr;
    for (const mp4::Track &track : movie.tracks)
        if (track.handler == mp4::fourcc("vide") && video == nullptr &&
            (track.player_codec == ts::Codec::h264 || track.player_codec == ts::Codec::hevc))
            video = &track;
    const mp4::Track *audio = source.audio_ordinal >= 0
                                  ? mp4::nth_track(movie, mp4::fourcc("soun"), source.audio_ordinal)
                                  : nullptr;
    if (video == nullptr || (source.audio_ordinal >= 0 && audio == nullptr))
    {
        out.why = "the selected tracks are not in the file";
        return false;
    }
    if (audio != nullptr && audio->player_codec != player_codec(source.audio_codec))
    {
        out.why = "audio track is not the expected " + source.audio_codec;
        return false;
    }
    auto demuxer = std::make_unique<mp4::Demuxer>();
    const double from = seconds > 0.5 ? seconds : 0.0;
    std::uint64_t offset = 0;
    if (!demuxer->configure(*video, mp4::video_start(*video, from), audio,
                            audio != nullptr ? mp4::audio_start(*audio, from) : 0, offset))
    {
        out.why = "tracks stored too far apart to read in one pass";
        return false;
    }
    demuxer->present_from(static_cast<std::int64_t>(from * 90000.0));
    out.offset = offset;
    out.demuxer = std::move(demuxer);
    return true;
}
} // namespace

bool open(const Source &source, double seconds, Ranges &ranges, Opened &out) noexcept
{
    out = Opened{};
    Kept &index = kept();
    if (index.key != source.key || source.key.empty())
    {
        forget();
        const bool indexed = source.container == "mkv"   ? index_mkv(source, ranges, index, out.why)
                             : source.container == "mp4" ? index_mp4(source, ranges, index, out.why)
                                                         : false;
        if (!indexed)
        {
            if (out.why.empty())
                out.why = "not MKV or MP4";
            forget();
            return false;
        }
        index.key = source.key;
    }
    return source.container == "mkv" ? open_mkv(source, seconds, index, out)
                                     : open_mp4(source, seconds, index, out);
}

void forget() noexcept
{
    Kept &index = kept();
    index.key.clear();
    index.info = mkv::Info{};
    index.cues.clear();
    index.cues.shrink_to_fit();
    index.movie = mp4::Movie{};
    index.moov.reset();
}

} // namespace slopfin::direct
