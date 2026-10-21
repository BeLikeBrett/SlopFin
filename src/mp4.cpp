/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "mp4.hpp"

#include <algorithm>

namespace slopfin::mp4
{
namespace
{
/* Tracks stored further apart than this are not read in one pass. */
constexpr std::uint64_t kMaxSpread = 48u * 1024u * 1024u;

std::uint32_t u32(const std::uint8_t *p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}

std::uint64_t u64(const std::uint8_t *p) noexcept
{
    return static_cast<std::uint64_t>(u32(p)) << 32 | u32(p + 4);
}

unsigned u16(const std::uint8_t *p) noexcept
{
    return static_cast<unsigned>(p[0]) << 8 | p[1];
}

/* Calls visit(type, body, size) for each child box; false when malformed. */
template <typename Visit> bool each_box(const std::uint8_t *data, std::size_t size, Visit visit)
{
    std::size_t at = 0;
    while (at + 8 <= size)
    {
        Box box;
        if (!read_box(data + at, size - at, box))
            return false;
        const std::uint64_t whole = box.size == 0 ? size - at : box.size;
        if (whole < static_cast<std::uint64_t>(box.header) || whole > size - at)
            return false;
        visit(box.type, data + at + box.header, static_cast<std::size_t>(whole) - box.header);
        at += static_cast<std::size_t>(whole);
    }
    return true;
}

/* A full box's table: version/flags, a 32-bit count, then entries of `width`. */
bool table(const std::uint8_t *body, std::size_t size, std::size_t width, Table &out,
           std::size_t skip = 0) noexcept
{
    if (size < 8 + skip)
        return false;
    const std::uint32_t count = u32(body + 4 + skip);
    if (static_cast<std::uint64_t>(count) * width > size - 8 - skip)
        return false;
    out.data = body + 8 + skip;
    out.count = count;
    return true;
}

/* MPEG-4 descriptor length: up to four bytes of seven bits. */
bool descriptor(const std::uint8_t *&at, const std::uint8_t *end, int &tag, std::size_t &length)
{
    if (at >= end)
        return false;
    tag = *at++;
    length = 0;
    for (int i = 0; i < 4; ++i)
    {
        if (at >= end)
            return false;
        const std::uint8_t byte = *at++;
        length = length << 7 | (byte & 0x7f);
        if (!(byte & 0x80))
            break;
    }
    return length <= static_cast<std::size_t>(end - at);
}

/* esds: the AAC AudioSpecificConfig, or the MP3 object type. */
void parse_esds(const std::uint8_t *body, std::size_t size, Track &track)
{
    if (size < 4)
        return;
    const std::uint8_t *at = body + 4;
    const std::uint8_t *const end = body + size;
    int tag = 0;
    std::size_t length = 0;
    if (!descriptor(at, end, tag, length) || tag != 3 || length < 3)
        return;
    const std::uint8_t flags = at[2];
    at += 3;
    if (flags & 0x80)
        at += 2;
    if ((flags & 0x40) && at < end)
        at += 1 + *at;
    if (flags & 0x20)
        at += 2;
    if (at >= end || !descriptor(at, end, tag, length) || tag != 4 || length < 13)
        return;
    const std::uint8_t object = at[0];
    at += 13;
    if (object == 0x6b || object == 0x69)
    {
        track.player_codec = ts::Codec::mp3;
        return;
    }
    if (object != 0x40 && object != 0x66 && object != 0x67)
        return;
    if (!descriptor(at, end, tag, length) || tag != 5)
        return;
    track.aac = annexb::parse_audio_specific_config(at, length);
    if (track.aac.valid)
        track.player_codec = ts::Codec::aac_adts;
}

void parse_stsd(const std::uint8_t *body, std::size_t size, Track &track)
{
    if (size < 16)
        return;
    const std::uint8_t *entry = body + 8;
    Box box;
    if (!read_box(entry, size - 8, box) || box.size > size - 8 || box.size < 16)
        return;
    track.codec = box.type;
    const std::uint8_t *fields = entry + box.header + 8; /* past reserved and data reference */
    const std::size_t total = static_cast<std::size_t>(box.size) - box.header - 8;
    std::size_t header = 0;
    if (track.handler == fourcc("vide"))
        header = 70;
    else if (track.handler == fourcc("soun"))
    {
        if (total < 20)
            return;
        const unsigned version = u16(fields);
        track.channels = static_cast<int>(u16(fields + 8));
        track.sample_rate = static_cast<int>(u32(fields + 16) >> 16);
        header = version == 1 ? 36 : version == 2 ? 56 : 20;
    }
    if (total < header)
        return;
    const std::uint8_t *children = fields + header;
    const std::size_t remaining = total - header;
    const std::uint32_t type = track.codec;
    if (type == fourcc("ac-3"))
        track.player_codec = ts::Codec::ac3;
    else if (type == fourcc("ec-3"))
        track.player_codec = ts::Codec::eac3;
    else if (type == fourcc(".mp3"))
        track.player_codec = ts::Codec::mp3;
    (void)each_box(
        children, remaining,
        [&](std::uint32_t child, const std::uint8_t *data, std::size_t length)
        {
            if (child == fourcc("avcC") && (type == fourcc("avc1") || type == fourcc("avc3")))
            {
                track.sets = annexb::parse_avcc(data, length);
                if (track.sets.valid)
                    track.player_codec = ts::Codec::h264;
            }
            else if (child == fourcc("hvcC") && (type == fourcc("hvc1") || type == fourcc("hev1") ||
                                                 type == fourcc("dvh1") || type == fourcc("dvhe")))
            {
                track.sets = annexb::parse_hvcc(data, length);
                if (track.sets.valid)
                    track.player_codec = ts::Codec::hevc;
            }
            else if (child == fourcc("esds") && type == fourcc("mp4a"))
                parse_esds(data, length, track);
        });
}

void parse_stbl(const std::uint8_t *body, std::size_t size, Track &track, bool &ok)
{
    bool have_size = false, have_chunks = false;
    (void)each_box(body, size,
                   [&](std::uint32_t type, const std::uint8_t *data, std::size_t length)
                   {
                       if (type == fourcc("stsd"))
                           parse_stsd(data, length, track);
                       else if (type == fourcc("stts"))
                           ok &= table(data, length, 8, track.stts);
                       else if (type == fourcc("ctts"))
                       {
                           ok &= table(data, length, 8, track.ctts);
                           track.ctts_version = length > 0 ? data[0] : 0;
                       }
                       else if (type == fourcc("stss"))
                           ok &= table(data, length, 4, track.stss);
                       else if (type == fourcc("stsc"))
                           ok &= table(data, length, 12, track.stsc);
                       else if (type == fourcc("stsz") && length >= 12)
                       {
                           track.fixed_size = u32(data + 4);
                           track.samples = u32(data + 8);
                           if (track.fixed_size == 0)
                               ok &= table(data, length, 4, track.stsz, 4);
                           have_size = true;
                       }
                       else if (type == fourcc("stco") || type == fourcc("co64"))
                       {
                           track.chunks_64 = type == fourcc("co64");
                           ok &= table(data, length, track.chunks_64 ? 8 : 4, track.chunks);
                           have_chunks = true;
                       }
                   });
    ok &= have_size && have_chunks && track.stts.count > 0 && track.stsc.count > 0;
}

void parse_elst(const std::uint8_t *body, std::size_t size, Track &track, std::uint32_t movie_scale)
{
    if (size < 8)
        return;
    const int version = body[0];
    const std::size_t width = version == 1 ? 20 : 12;
    const std::uint32_t count = u32(body + 4);
    std::int64_t empty = 0; /* movie timescale */
    for (std::uint32_t i = 0; i < count && 8 + (i + 1) * width <= size; ++i)
    {
        const std::uint8_t *entry = body + 8 + i * width;
        const std::uint64_t duration = version == 1 ? u64(entry) : u32(entry);
        const std::int64_t media = version == 1 ? static_cast<std::int64_t>(u64(entry + 8))
                                                : static_cast<std::int32_t>(u32(entry + 4));
        if (media == -1)
        {
            empty += static_cast<std::int64_t>(duration);
            continue;
        }
        /* Presentation starts at `media`, after any empty edit's delay. */
        const std::int64_t delay =
            movie_scale > 0 && track.timescale > 0
                ? empty * static_cast<std::int64_t>(track.timescale) / movie_scale
                : 0;
        track.edit_shift = media - delay;
        return;
    }
}

void parse_trak(const std::uint8_t *body, std::size_t size, std::uint32_t movie_scale, Movie &movie)
{
    Track track;
    bool ok = true, have_stbl = false;
    const std::uint8_t *edits = nullptr;
    std::size_t edits_size = 0;
    (void)each_box(
        body, size,
        [&](std::uint32_t type, const std::uint8_t *data, std::size_t length)
        {
            if (type == fourcc("edts"))
                (void)each_box(data, length,
                               [&](std::uint32_t child, const std::uint8_t *p, std::size_t n)
                               {
                                   if (child == fourcc("elst"))
                                   {
                                       edits = p;
                                       edits_size = n;
                                   }
                               });
            if (type != fourcc("mdia"))
                return;
            (void)each_box(data, length,
                           [&](std::uint32_t child, const std::uint8_t *p, std::size_t n)
                           {
                               if (child == fourcc("mdhd") && n >= 24)
                                   track.timescale = p[0] == 1 ? u32(p + 20) : u32(p + 12);
                               else if (child == fourcc("hdlr") && n >= 12)
                                   track.handler = u32(p + 8);
                               else if (child == fourcc("minf"))
                                   (void)each_box(p, n,
                                                  [&](std::uint32_t inner, const std::uint8_t *q,
                                                      std::size_t m)
                                                  {
                                                      if (inner == fourcc("stbl"))
                                                      {
                                                          have_stbl = true;
                                                          parse_stbl(q, m, track, ok);
                                                      }
                                                  });
                           });
        });
    if (edits != nullptr)
        parse_elst(edits, edits_size, track, movie_scale);
    if (have_stbl && ok && track.timescale > 0)
        movie.tracks.push_back(track);
}

std::uint32_t entry(const Table &t, std::uint32_t i, std::size_t width, std::size_t field) noexcept
{
    return u32(t.data + static_cast<std::size_t>(i) * width + field);
}

std::uint64_t chunk_offset(const Track &track, std::uint32_t chunk) noexcept
{
    if (chunk >= track.chunks.count)
        return 0;
    return track.chunks_64 ? u64(track.chunks.data + static_cast<std::size_t>(chunk) * 8)
                           : u32(track.chunks.data + static_cast<std::size_t>(chunk) * 4);
}

std::uint32_t sample_size(const Track &track, std::uint32_t index) noexcept
{
    if (track.fixed_size != 0)
        return track.fixed_size;
    return index < track.stsz.count ? u32(track.stsz.data + static_cast<std::size_t>(index) * 4)
                                    : 0;
}

/* The last sample whose decode time is at or before `time`. */
std::uint32_t sample_at(const Track &track, std::int64_t time) noexcept
{
    std::int64_t dts = 0;
    std::uint32_t index = 0;
    for (std::uint32_t i = 0; i < track.stts.count; ++i)
    {
        const std::uint32_t count = entry(track.stts, i, 8, 0);
        const std::uint32_t delta = entry(track.stts, i, 8, 4);
        const std::int64_t span = static_cast<std::int64_t>(count) * delta;
        if (delta > 0 && dts + span > time)
        {
            const auto inside =
                static_cast<std::uint32_t>(std::max<std::int64_t>(0, time - dts) / delta);
            return std::min(index + inside, track.samples > 0 ? track.samples - 1 : 0);
        }
        dts += span;
        index += count;
    }
    return track.samples > 0 ? track.samples - 1 : 0;
}
} // namespace

bool read_box(const std::uint8_t *data, std::size_t available, Box &out) noexcept
{
    if (available < 8)
        return false;
    std::uint64_t size = u32(data);
    out.type = u32(data + 4);
    out.header = 8;
    if (size == 1)
    {
        if (available < 16)
            return false;
        size = u64(data + 8);
        out.header = 16;
    }
    if (size != 0 && size < static_cast<std::uint64_t>(out.header))
        return false;
    out.size = size;
    return true;
}

bool parse_moov(const std::uint8_t *moov, std::size_t size, Movie &movie)
{
    movie = Movie{};
    Box box;
    if (!read_box(moov, size, box) || box.type != fourcc("moov") || box.size > size)
        return false;
    const std::size_t whole = box.size == 0 ? size : static_cast<std::size_t>(box.size);
    std::uint32_t movie_scale = 0;
    const std::uint8_t *body = moov + box.header;
    const std::size_t length = whole - static_cast<std::size_t>(box.header);
    if (!each_box(body, length,
                  [&](std::uint32_t type, const std::uint8_t *data, std::size_t n)
                  {
                      if (type == fourcc("mvhd") && n >= 24)
                          movie_scale = data[0] == 1 ? u32(data + 20) : u32(data + 12);
                      else if (type == fourcc("mvex"))
                          movie.fragmented = true;
                  }))
        return false;
    (void)each_box(body, length,
                   [&](std::uint32_t type, const std::uint8_t *data, std::size_t n)
                   {
                       if (type == fourcc("trak"))
                           parse_trak(data, n, movie_scale, movie);
                   });
    return !movie.tracks.empty();
}

const Track *nth_track(const Movie &movie, std::uint32_t handler, int ordinal) noexcept
{
    for (const Track &track : movie.tracks)
        if (track.handler == handler && ordinal-- == 0)
            return &track;
    return nullptr;
}

bool Cursor::start(const Track &track, std::uint32_t index) noexcept
{
    track_ = &track;
    index_ = index;
    if (index >= track.samples)
        return false;
    /* Decode time and composition offset: run-length tables. */
    dts_ = 0;
    std::uint32_t before = index;
    stts_entry_ = 0;
    for (; stts_entry_ < track.stts.count; ++stts_entry_)
    {
        const std::uint32_t count = entry(track.stts, stts_entry_, 8, 0);
        const std::uint32_t delta = entry(track.stts, stts_entry_, 8, 4);
        if (before < count)
        {
            dts_ += static_cast<std::int64_t>(before) * delta;
            stts_left_ = count - before;
            break;
        }
        dts_ += static_cast<std::int64_t>(count) * delta;
        before -= count;
    }
    if (stts_entry_ >= track.stts.count)
        return false;
    ctts_entry_ = 0;
    ctts_left_ = 0;
    before = index;
    for (; ctts_entry_ < track.ctts.count; ++ctts_entry_)
    {
        const std::uint32_t count = entry(track.ctts, ctts_entry_, 8, 0);
        if (before < count)
        {
            ctts_left_ = count - before;
            break;
        }
        before -= count;
    }
    /* Chunk: samples-per-chunk runs, then the sizes before it in its chunk. */
    before = index;
    stsc_entry_ = 0;
    bool found = false;
    for (; stsc_entry_ < track.stsc.count; ++stsc_entry_)
    {
        const std::uint32_t first = entry(track.stsc, stsc_entry_, 12, 0);
        const std::uint32_t per = entry(track.stsc, stsc_entry_, 12, 4);
        const std::uint32_t next = stsc_entry_ + 1 < track.stsc.count
                                       ? entry(track.stsc, stsc_entry_ + 1, 12, 0)
                                       : track.chunks.count + 1;
        if (first == 0 || next < first || per == 0)
        {
            if (per == 0 && next >= first)
                continue;
            return false;
        }
        const std::uint64_t run = static_cast<std::uint64_t>(next - first) * per;
        if (before < run)
        {
            chunk_ = first - 1 + before / per;
            const std::uint32_t inside = before % per;
            chunk_left_ = per - inside;
            chunk_offset_ = chunk_offset(track, chunk_);
            for (std::uint32_t i = index - inside; i < index; ++i)
                chunk_offset_ += sample_size(track, i);
            found = true;
            break;
        }
        before -= static_cast<std::uint32_t>(run);
    }
    if (!found || chunk_ >= track.chunks.count)
        return false;
    /* The first sync sample at or after this one. */
    std::uint32_t low = 0, high = track.stss.count;
    while (low < high)
    {
        const std::uint32_t middle = (low + high) / 2;
        if (entry(track.stss, middle, 4, 0) < index + 1)
            low = middle + 1;
        else
            high = middle;
    }
    stss_entry_ = low;
    load();
    return true;
}

void Cursor::load() noexcept
{
    const Track &track = *track_;
    sample_.offset = chunk_offset_;
    sample_.size = sample_size(track, index_);
    sample_.dts = dts_;
    std::int64_t composition = 0;
    if (ctts_entry_ < track.ctts.count)
    {
        const std::uint32_t raw = entry(track.ctts, ctts_entry_, 8, 4);
        /* Version 0 is unsigned by the letter, but writers store negative offsets in it. */
        composition = static_cast<std::int32_t>(raw);
    }
    sample_.pts = dts_ + composition - track.edit_shift;
    sample_.key = track.stss.count == 0 || (stss_entry_ < track.stss.count &&
                                            entry(track.stss, stss_entry_, 4, 0) == index_ + 1);
}

void Cursor::advance() noexcept
{
    if (done())
        return;
    const Track &track = *track_;
    chunk_offset_ += sample_.size;
    dts_ += entry(track.stts, stts_entry_, 8, 4);
    ++index_;
    if (index_ >= track.samples)
        return;
    if (--stts_left_ == 0)
        while (++stts_entry_ < track.stts.count &&
               (stts_left_ = entry(track.stts, stts_entry_, 8, 0)) == 0)
        {
        }
    if (ctts_entry_ < track.ctts.count && --ctts_left_ == 0)
        while (++ctts_entry_ < track.ctts.count &&
               (ctts_left_ = entry(track.ctts, ctts_entry_, 8, 0)) == 0)
        {
        }
    if (--chunk_left_ == 0)
    {
        do
        {
            ++chunk_;
            while (stsc_entry_ + 1 < track.stsc.count &&
                   chunk_ + 1 >= entry(track.stsc, stsc_entry_ + 1, 12, 0))
                ++stsc_entry_;
            chunk_left_ = entry(track.stsc, stsc_entry_, 12, 4);
        } while (chunk_left_ == 0 && chunk_ < track.chunks.count);
        chunk_offset_ = chunk_offset(track, chunk_);
    }
    while (stss_entry_ < track.stss.count && entry(track.stss, stss_entry_, 4, 0) < index_ + 1)
        ++stss_entry_;
    load();
}

std::uint32_t video_start(const Track &video, double seconds) noexcept
{
    const auto time = static_cast<std::int64_t>(seconds * video.timescale) + video.edit_shift;
    const std::uint32_t index = sample_at(video, time);
    if (video.stss.count == 0)
        return index;
    /* The last sync sample at or before it. */
    std::uint32_t low = 0, high = video.stss.count;
    while (low < high)
    {
        const std::uint32_t middle = (low + high) / 2;
        if (entry(video.stss, middle, 4, 0) <= index + 1)
            low = middle + 1;
        else
            high = middle;
    }
    return low == 0 ? 0 : entry(video.stss, low - 1, 4, 0) - 1;
}

std::uint32_t audio_start(const Track &audio, double seconds) noexcept
{
    const auto time = static_cast<std::int64_t>(seconds * audio.timescale) + audio.edit_shift;
    return time <= 0 ? 0 : sample_at(audio, time);
}

bool Demuxer::configure(const Track &video, std::uint32_t video_sample, const Track *audio,
                        std::uint32_t audio_sample, std::uint64_t &first_offset)
{
    video_ = &video;
    audio_ = audio;
    if ((video.player_codec != ts::Codec::h264 && video.player_codec != ts::Codec::hevc) ||
        !video.sets.valid || !video_cursor_.start(video, video_sample))
        return false;
    first_offset = video_cursor_.sample().offset;
    if (audio != nullptr)
    {
        if (audio->player_codec == ts::Codec::unknown || !audio_cursor_.start(*audio, audio_sample))
            return false;
        const std::uint64_t a = audio_cursor_.sample().offset;
        const std::uint64_t spread = a > first_offset ? a - first_offset : first_offset - a;
        if (spread > kMaxSpread)
            return false;
        first_offset = std::min(first_offset, a);
    }
    base_ = first_offset;
    return true;
}

void Demuxer::feed(const std::uint8_t *bytes, std::size_t count) noexcept
{
    if (broken_)
        return;
    pending_.insert(pending_.end(), bytes, bytes + count);
    for (;;)
    {
        Cursor *next = nullptr;
        if (!video_cursor_.done())
            next = &video_cursor_;
        if (audio_ != nullptr && !audio_cursor_.done() &&
            (next == nullptr || audio_cursor_.sample().offset < next->sample().offset))
            next = &audio_cursor_;
        if (next == nullptr)
        {
            pending_.clear();
            return;
        }
        const Sample &sample = next->sample();
        if (sample.offset < base_)
        {
            next->advance(); /* overlaps bytes already passed: a damaged table */
            continue;
        }
        const std::uint64_t have = base_ + pending_.size();
        if (sample.offset + sample.size > have)
            break;
        emit(next == &video_cursor_ ? *video_ : *audio_, sample,
             pending_.data() + (sample.offset - base_));
        next->advance();
    }
    /* Release everything before the next sample either track needs. */
    std::uint64_t needed = base_ + pending_.size();
    if (!video_cursor_.done())
        needed = std::min(needed, video_cursor_.sample().offset);
    if (audio_ != nullptr && !audio_cursor_.done())
        needed = std::min(needed, audio_cursor_.sample().offset);
    const auto drop = static_cast<std::size_t>(needed - base_);
    if (drop == pending_.size() || drop >= 1024u * 1024u)
    {
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(drop));
        base_ += drop;
    }
    if (pending_.size() > kMaxSpread + 16u * 1024u * 1024u)
        broken_ = true;
}

namespace
{
/* Track time to 90 kHz, rounded to nearest as FFmpeg does. */
std::int64_t to_90k(std::int64_t time, std::int64_t scale) noexcept
{
    const std::int64_t scaled = time * 90000;
    return (scaled >= 0 ? scaled + scale / 2 : scaled - scale / 2) / scale;
}
} // namespace

void Demuxer::emit(const Track &track, const Sample &sample, const std::uint8_t *bytes) noexcept
{
    ts::AccessUnit unit;
    const auto scale = static_cast<std::int64_t>(track.timescale);
    unit.pts = std::max<std::int64_t>(0, to_90k(sample.pts, scale));
    if (&track == video_)
    {
        unit.video = true;
        /* Before the edit's start a decode time is negative: leave it unstated. */
        const std::int64_t dts = to_90k(sample.dts - track.edit_shift, scale);
        unit.dts = dts >= 0 ? dts : -1;
        if (!annexb::convert(bytes, sample.size, track.sets, track.player_codec == ts::Codec::hevc,
                             sample.key, unit.data))
            return;
    }
    else
    {
        if (sample.pts < 0 || unit.pts < present_from_)
            return; /* priming before the edit's start, or before the seek target */
        if (track.player_codec == ts::Codec::aac_adts)
        {
            if (!annexb::append_adts(track.aac, bytes, sample.size, unit.data))
                return;
        }
        else
            unit.data.assign(bytes, bytes + sample.size);
    }
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
}

} // namespace slopfin::mp4
