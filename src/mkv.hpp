/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Matroska, read straight from the library file.
 *
 * The header (track list and codec setup) and the index of keyframe clusters
 * (Cues) are parsed from small range reads; playback then reads the file
 * linearly from the cluster nearest the start position, and the streaming
 * demuxer turns its blocks into access units. A seek is just a new range.
 *
 * Free of PS5 headers so it can be tested on a workstation.
 */
#ifndef SLOPFIN_MKV_HPP
#define SLOPFIN_MKV_HPP

#include "annexb.hpp"
#include "media_demux.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace slopfin::mkv
{

enum : std::uint32_t
{
    kEbml = 0x1A45DFA3,
    kSegment = 0x18538067,
    kSeekHead = 0x114D9B74,
    kInfo = 0x1549A966,
    kTracks = 0x1654AE6B,
    kCues = 0x1C53BB6B,
    kCluster = 0x1F43B675,
};

struct Track
{
    std::uint64_t number = 0;
    int type = 0; /* 1 video, 2 audio, 17 subtitle */
    std::string codec_id;
    std::vector<std::uint8_t> codec_private;
    double sample_rate = 0.0;
    int channels = 0;
    int width = 0, height = 0;
    std::uint64_t codec_delay = 0; /* ns of encoder priming, subtracted from timestamps */
    bool encoded = false;          /* compressed or header-stripped: not readable here */
};

struct Info
{
    std::uint64_t segment_data = 0; /* file offset where the Segment's payload begins */
    std::uint64_t timestamp_scale = 1000000;
    double duration = 0.0; /* in timestamp units */
    std::vector<Track> tracks;
    /* File offsets from the SeekHead, 0 when not listed. */
    std::uint64_t info_at = 0, tracks_at = 0, cues_at = 0;
    std::uint64_t first_cluster = 0; /* 0 when not inside the bytes read */
    bool have_info = false, have_tracks = false;
};

struct CuePoint
{
    std::uint64_t time = 0;    /* timestamp units */
    std::uint64_t cluster = 0; /* file offset */
};

/* An element header read from bytes: id with its marker bits, size without. */
struct Header
{
    std::uint32_t id = 0;
    std::uint64_t size = 0;
    int length = 0; /* bytes in the header itself */
    bool unknown_size = false;
};

/* Reads a header; false when the bytes are too few or malformed. */
bool read_header(const std::uint8_t *data, std::size_t available, Header &out) noexcept;

/*
 * Parses the start of a file: EBML header, Segment, then top-level elements
 * until the first Cluster or the end of the bytes. Elements cut off by the end
 * are left for read_element(). False when this is not Matroska or WebM.
 */
bool parse_head(const std::uint8_t *data, std::size_t size, Info &info);

/* Parses one Info or Tracks element read separately from `data`. */
bool parse_element(const std::uint8_t *data, std::size_t size, Info &info);

/* Parses a Cues element; points for `track` (any track when none match). */
std::vector<CuePoint> parse_cues(const std::uint8_t *data, std::size_t size, const Info &info,
                                 std::uint64_t track);

/* The cluster to start reading from for `seconds`, or 0 when unknown. */
std::uint64_t cluster_for(const std::vector<CuePoint> &cues, const Info &info,
                          double seconds) noexcept;

/* The i-th track of a type, in file order, or nullptr. */
const Track *nth_track(const Info &info, int type, int ordinal) noexcept;

/* A selected track's codec as the player knows it; unknown when unsupported. */
ts::Codec codec_of(const Track &track) noexcept;

class Demuxer final : public media::Demuxer
{
  public:
    /* False when either track cannot be read as the player needs it. */
    bool configure(const Info &info, const Track &video, const Track *audio);
    /* Audio before this presentation time (90 kHz) is dropped. */
    void present_from(std::int64_t pts) noexcept
    {
        present_from_ = pts;
    }

    void feed(const std::uint8_t *bytes, std::size_t count) noexcept override;
    bool next(ts::AccessUnit &out) noexcept override;
    void finish() noexcept override;
    [[nodiscard]] ts::Codec video_codec() const noexcept override
    {
        return video_codec_;
    }
    [[nodiscard]] ts::Codec audio_codec() const noexcept override
    {
        return audio_codec_;
    }
    [[nodiscard]] bool broken() const noexcept override
    {
        return broken_;
    }

  private:
    void parse() noexcept;
    void block(const std::uint8_t *data, std::size_t size, bool simple, bool keyframe) noexcept;
    void block_group(const std::uint8_t *data, std::size_t size) noexcept;

    std::uint64_t scale_ = 1000000;
    std::uint64_t video_number_ = 0, audio_number_ = 0;
    std::int64_t audio_delay_ = 0; /* 90 kHz */
    std::int64_t present_from_ = 0;
    ts::Codec video_codec_ = ts::Codec::unknown;
    ts::Codec audio_codec_ = ts::Codec::unknown;
    annexb::ParameterSets sets_;
    annexb::AacConfig aac_;

    std::vector<std::uint8_t> pending_;
    std::size_t consumed_ = 0;   /* bytes of pending_ already parsed */
    std::uint64_t skip_ = 0;     /* bytes still to discard unread */
    std::uint64_t position_ = 0; /* stream bytes parsed so far */
    std::uint64_t cluster_end_ = 0;
    bool in_cluster_ = false;
    bool cluster_unknown_ = false;
    std::uint64_t cluster_time_ = 0;
    bool started_ = false; /* the first video keyframe has gone out */
    bool broken_ = false;
    std::deque<ts::AccessUnit> ready_;
};

} // namespace slopfin::mkv

#endif
