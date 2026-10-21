/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * MP4 (ISO base media), read straight from the library file.
 *
 * The moov box is the index: every sample's file offset, size and time. It
 * is read once into memory the caller owns; tracks refer into it rather than
 * copying tables that run to megabytes for a long film. Playback then reads
 * the file linearly from the first sample it needs, and the demuxer slices
 * samples out of the bytes as they arrive, in file order.
 *
 * Fragmented files (moof) and files whose tracks are stored far apart are
 * refused: the first has no complete index, the second cannot be read in one
 * pass without buffering most of the film. The server path handles both.
 *
 * Free of PS5 headers so it can be tested on a workstation.
 */
#ifndef SLOPFIN_MP4_HPP
#define SLOPFIN_MP4_HPP

#include "annexb.hpp"
#include "media_demux.hpp"

#include <cstdint>
#include <deque>
#include <vector>

namespace slopfin::mp4
{

constexpr std::uint32_t fourcc(const char (&name)[5]) noexcept
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(name[0])) << 24 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(name[1])) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(name[2])) << 8 |
           static_cast<unsigned char>(name[3]);
}

struct Box
{
    std::uint32_t type = 0;
    std::uint64_t size = 0; /* whole box, header included; 0 runs to end of file */
    int header = 0;
};

/* Reads a box header; false when the bytes are too few or malformed. */
bool read_box(const std::uint8_t *data, std::size_t available, Box &out) noexcept;

/* A sample table: entries of `width` bytes, big-endian, inside the moov. */
struct Table
{
    const std::uint8_t *data = nullptr;
    std::uint32_t count = 0;
};

struct Track
{
    std::uint32_t handler = 0; /* 'vide' or 'soun' */
    std::uint32_t timescale = 0;
    std::uint32_t codec = 0; /* sample entry type: avc1, hvc1, mp4a, ac-3, ... */
    ts::Codec player_codec = ts::Codec::unknown;
    annexb::ParameterSets sets;
    annexb::AacConfig aac;
    int channels = 0;
    int sample_rate = 0;
    std::int64_t edit_shift = 0; /* media-time units subtracted from presentation */

    Table stts, ctts, stss, stsc, stsz, chunks;
    int ctts_version = 0;
    bool chunks_64 = false;
    std::uint32_t fixed_size = 0; /* stsz: one size for every sample, or 0 */
    std::uint32_t samples = 0;
};

struct Movie
{
    std::vector<Track> tracks;
    bool fragmented = false;
};

/*
 * Parses a moov box (its header included). Tracks point into `moov`, which
 * must outlive them. False when it is not a readable moov.
 */
bool parse_moov(const std::uint8_t *moov, std::size_t size, Movie &movie);

/* The i-th track with a handler, in file order, or nullptr. */
const Track *nth_track(const Movie &movie, std::uint32_t handler, int ordinal) noexcept;

/* One sample's place in the file and time, for a cursor walking a track. */
struct Sample
{
    std::uint64_t offset = 0;
    std::uint32_t size = 0;
    std::int64_t dts = 0; /* track timescale */
    std::int64_t pts = 0;
    bool key = false;
};

/* Walks a track's samples in order from any starting sample. */
class Cursor
{
  public:
    bool start(const Track &track, std::uint32_t index) noexcept;
    [[nodiscard]] bool done() const noexcept
    {
        return track_ == nullptr || index_ >= track_->samples;
    }
    [[nodiscard]] const Sample &sample() const noexcept
    {
        return sample_;
    }
    [[nodiscard]] std::uint32_t index() const noexcept
    {
        return index_;
    }
    void advance() noexcept;

  private:
    void load() noexcept;
    const Track *track_ = nullptr;
    std::uint32_t index_ = 0;
    Sample sample_;
    /* Run-length positions, so advancing is constant time. */
    std::uint32_t stts_entry_ = 0, stts_left_ = 0;
    std::uint32_t ctts_entry_ = 0, ctts_left_ = 0;
    std::uint32_t stsc_entry_ = 0, chunk_ = 0, chunk_left_ = 0;
    std::uint32_t stss_entry_ = 0;
    std::uint64_t chunk_offset_ = 0;
    std::int64_t dts_ = 0;
};

/* The sync sample to start from for `seconds`, and the first audio sample at
   or after it. */
std::uint32_t video_start(const Track &video, double seconds) noexcept;
std::uint32_t audio_start(const Track &audio, double seconds) noexcept;

class Demuxer final : public media::Demuxer
{
  public:
    /*
     * Prepares to read from the given starting samples. `first_offset` is
     * where the caller must begin feeding bytes. False when the tracks cannot
     * be read in one pass.
     */
    bool configure(const Track &video, std::uint32_t video_sample, const Track *audio,
                   std::uint32_t audio_sample, std::uint64_t &first_offset);
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
        return video_ != nullptr ? video_->player_codec : ts::Codec::unknown;
    }
    [[nodiscard]] ts::Codec audio_codec() const noexcept override
    {
        return audio_ != nullptr ? audio_->player_codec : ts::Codec::unknown;
    }
    [[nodiscard]] bool broken() const noexcept override
    {
        return broken_;
    }

  private:
    void emit(const Track &track, const Sample &sample, const std::uint8_t *bytes) noexcept;
    const Track *video_ = nullptr;
    const Track *audio_ = nullptr;
    Cursor video_cursor_, audio_cursor_;
    std::vector<std::uint8_t> pending_;
    std::uint64_t base_ = 0; /* file offset of pending_[0] */
    std::int64_t present_from_ = 0;
    bool broken_ = false;
    std::deque<ts::AccessUnit> ready_;
};

} // namespace slopfin::mp4

#endif
