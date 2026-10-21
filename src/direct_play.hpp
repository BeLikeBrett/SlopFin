/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_DIRECT_PLAY_HPP
#define SLOPFIN_DIRECT_PLAY_HPP
#include "jellyfin.hpp"

#include <string>

namespace slopfin::jellyfin
{
/*
 * Whether the console can take the selected audio track exactly as the file
 * stores it, under the current settings. Jellyfin's own direct-play check has
 * let unlisted codecs through, so this is decided here as well.
 */
inline bool direct_audio_supported(const PlaybackPlan &plan,
                                   const PlaybackRequest &request) noexcept
{
    if (plan.audio_codec.empty())
        return true; /* no audio */
    if (plan.audio_ordinal < 0 || plan.audio_sample_rate != 48000 ||
        (!request.audio_codec.empty() && request.audio_codec != plan.audio_codec))
        return false;
    const int limit = request.max_audio_channels;
    if (limit > 0 && plan.audio_channels > limit)
        return false;
    const std::string &codec = plan.audio_codec;
    if (codec == "aac" || codec == "mp3")
        return codec == "aac" || plan.audio_channels <= 2;
    if (codec == "ac3")
        return plan.audio_channels <= 6;
    const bool software =
        request.allow_software_audio && plan.audio_channels <= 8 &&
        (codec == "truehd" || codec == "eac3" || (codec == "dts" && plan.audio_profile == "DTS"));
    const bool bitstream =
        request.allow_bitstream && plan.audio_channels <= 8 && (codec == "eac3" || codec == "dts");
    return software || bitstream;
}
} // namespace slopfin::jellyfin
#endif
