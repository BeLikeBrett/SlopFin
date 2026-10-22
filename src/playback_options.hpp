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
    /* The PS5 decodes what it can and plays Dolby Vision 7 and 8.1 from their
       HDR10 layer, always: a failure on a title falls back to Jellyfin for
       that title (player::recover()). Only passthrough is a choice. */
    request.allow_bitstream = settings.audio_passthrough;
    request.allow_software_audio = software_available;
    request.allow_dv_hdr10_base = true;
    return request;
}
} // namespace slopfin::player
#endif
