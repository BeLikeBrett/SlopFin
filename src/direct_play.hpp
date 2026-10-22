/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_DIRECT_PLAY_HPP
#define SLOPFIN_DIRECT_PLAY_HPP
#include "jellyfin.hpp"

#include <string>

namespace slopfin::jellyfin
{
/* The settings, what failed for this title, and the TV's list, for the shared rule. */
inline audio_route::Options route_options(const PlaybackRequest &request) noexcept
{
    audio_route::Options options;
    options.passthrough = request.allow_bitstream;
    options.software = request.allow_software_audio;
    options.channel_limit = request.max_audio_channels;
    options.conversion = request.audio_codec;
    options.sink = request.sink;
    return options;
}

/* The path the selected source track takes (docs/development/AUDIO_ROUTE.md). */
inline audio_route::Route source_route(const PlaybackPlan &plan,
                                       const PlaybackRequest &request) noexcept
{
    return audio_route::choose(
        {plan.audio_codec, plan.audio_profile, plan.audio_channels, plan.audio_sample_rate},
        route_options(request));
}

/*
 * Whether the console can take the selected audio track exactly as the file
 * stores it. Jellyfin's own direct-play check has let unlisted codecs
 * through, so the shared rule decides here as well.
 */
inline bool direct_audio_supported(const PlaybackPlan &plan,
                                   const PlaybackRequest &request) noexcept
{
    if (plan.audio_codec.empty())
        return true; /* no audio */
    return plan.audio_ordinal >= 0 && source_route(plan, request).path != audio_route::Path::server;
}
} // namespace slopfin::jellyfin
#endif
