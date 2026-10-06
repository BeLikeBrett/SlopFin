/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_PLAYBACK_OPTIONS_HPP
#define SLOPFIN_PLAYBACK_OPTIONS_HPP
#include "config.hpp"
#include "jellyfin.hpp"

namespace slopfin::player
{
inline jellyfin::PlaybackRequest apply_preferences(jellyfin::PlaybackRequest request,
                                                   const config::Settings &settings,
                                                   bool software_available) noexcept
{
    request.allow_bitstream = settings.audio_passthrough;
    request.allow_software_audio = software_available && settings.software_audio;
    request.allow_dv_hdr10_base = settings.dv_hdr10_base;
    return request;
}
} // namespace slopfin::player
#endif
