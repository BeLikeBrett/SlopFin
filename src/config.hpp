/*
 * SlopFin - persisted settings.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Stored outside the app so a redeploy does not sign the user out.
 */

#ifndef SLOPFIN_CONFIG_HPP
#define SLOPFIN_CONFIG_HPP

#include "autoplay.hpp"
#include "subtitle_style.hpp"

#include <map>
#include <string>

namespace slopfin::config
{

struct Settings
{
    std::string host; /* Empty on first launch: ask for the user's server. */
    int port = 8096;
    std::string report_server; /* Optional diagnostic receiver, separate from Jellyfin. */
    std::string token;         /* Jellyfin access token, empty until signed in. */
    std::string user_id;
    std::string device_id;
    /* Whether the triggers load up while seeking. Hardware that has no trigger
       effects ignores it. */
    bool trigger_feedback = true;
    bool audio_passthrough = true;
    bool software_audio = false;
    bool dv_hdr10_base = false;
    /* Seconds sooner each title's subtitles are shown, keyed by item id; a
       title at its original timing has no entry (subtitle_timing.hpp). */
    std::map<std::string, double> subtitle_offsets;
    /* How text subtitles look, and what to do with picture subtitles. */
    subtitle_style::Choices subtitle_look;
    /* What happens when an episode ends. Settings is the default; a show can
       override only the enable/disable choice from the player. */
    autoplay::Choices autoplay_look;
    std::map<std::string, bool> autoplay_series_overrides;

    [[nodiscard]] bool signed_in() const noexcept
    {
        return !token.empty() && !user_id.empty();
    }
};

Settings &current() noexcept;

/* Reads the stored settings, inventing a device id on first run. */
void load() noexcept;
void save() noexcept;

/* A title's subtitle offset, 0 when it has none. */
double subtitle_offset(const std::string &item_id) noexcept;
/* Records it in memory; the caller saves when the adjusting is done. */
void set_subtitle_offset(const std::string &item_id, double offset) noexcept;

/* Settings supplies the default; a player toggle persists for this series. */
[[nodiscard]] bool autoplay_enabled(const std::string &series_id) noexcept;
void set_autoplay_enabled(const std::string &series_id, bool enabled) noexcept;

} // namespace slopfin::config

#endif
