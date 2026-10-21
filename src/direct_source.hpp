/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Opens an MKV or MP4 file for direct play: reads its track list and index
 * with byte ranges, chooses the tracks the negotiation selected, and says
 * where in the file to start reading for a given position.
 *
 * The last title's index is kept, so a seek costs one range request for the
 * media itself and nothing else. Free of PS5 headers, apart from the large
 * buffer allocator, so it can be tested on a workstation.
 */
#ifndef SLOPFIN_DIRECT_SOURCE_HPP
#define SLOPFIN_DIRECT_SOURCE_HPP

#include "media_demux.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace slopfin::direct
{

/* Reads bytes of the file; `got` short of `size` only at its end. */
class Ranges
{
  public:
    virtual ~Ranges() = default;
    virtual bool read(std::uint64_t offset, std::size_t size, std::uint8_t *out,
                      std::size_t &got) noexcept = 0;
};

struct Source
{
    std::string key;       /* identifies the file: media source and size */
    std::string container; /* "mkv" or "mp4" */
    std::uint64_t size = 0;
    int audio_ordinal = -1;  /* -1: play without audio */
    std::string audio_codec; /* as Jellyfin names it, to check against the file */
};

struct Opened
{
    std::unique_ptr<media::Demuxer> demuxer;
    std::uint64_t offset = 0; /* where the media stream must start */
    std::string why;          /* when opening failed */
};

/*
 * Prepares to play from `seconds`. Audio before then is dropped by the
 * demuxer; video starts at the keyframe before, and the caller hides the
 * pictures ahead of `seconds`.
 */
bool open(const Source &source, double seconds, Ranges &ranges, Opened &out) noexcept;

/* Releases the kept index. */
void forget() noexcept;

} // namespace slopfin::direct

#endif
