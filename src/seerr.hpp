/*
 * SlopFin - Discover: a Seerr/Jellyseerr/Overseerr request client inside the
 * Jellyfin app.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SLOPFIN_SEERR_HPP
#define SLOPFIN_SEERR_HPP

#include <string>
#include <vector>

namespace slopfin::seerr
{

/* UI states mapped from the request server's media status. */
enum class Availability : unsigned char
{
    unknown = 0,    /* never asked for */
    pending = 1,    /* requested, nothing grabbed yet */
    processing = 2, /* approved; may be queued or downloading */
    partial = 3,    /* some episodes or seasons have arrived */
    available = 4,  /* request server reports the title available */
};

/* One title in the request catalogue (a TMDB movie or series). */
struct Title
{
    int tmdb_id = 0;
    std::string media_type; /* "movie" or "tv" */
    std::string name;
    std::string overview;
    std::string jellyfin_id;
    std::string poster_path; /* TMDB path, empty when there is none */
    int year = 0;
    std::string release_date;
    double popularity = 0;
    double rating = 0.0; /* TMDB audience score out of 10 */
    Availability availability = Availability::unknown;
    Availability availability4k = Availability::unknown;
    bool requested = false;
};

/* One request, with the download state the queue reports. */
struct Request
{
    int id = 0;
    int tmdb_id = 0;
    std::string media_type;
    std::string name;
    std::string jellyfin_id;
    std::string poster_path;
    std::string requested_by;
    int requested_by_id = 0;
    Availability availability = Availability::unknown;
    bool is4k = false;
    std::string profile_name;
    std::vector<int> seasons;
    int approval = 0; /* 1 pending approval, 2 approved, 3 declined, 0 unknown */
    bool downloading = false;
    double progress = -1.0; /* 0..1 while downloading, -1 when not */
    long long size_bytes = 0;
    long long remaining_bytes = 0;
    long long rate_bytes = 0; /* bytes per second, estimated between refreshes */
    std::string eta;          /* as the server words it, e.g. "12 minutes" */
};

/* Opens the screen from the sidebar; kicks a refresh if needed. */
void open() noexcept;
/* False when the viewer leaves; the caller returns to the sidebar. */
bool handle_input() noexcept;
bool modal_open() noexcept;
bool take_settings_request() noexcept;
std::string take_open_item() noexcept;
void close_settings() noexcept;
#ifdef SLOPFIN_HOST
void preview_search(const std::string &query) noexcept;
#endif
void draw() noexcept;
/* Services a queued keyboard. Call once per frame from the render loop. */
void poll() noexcept;

/* Settings -> Requests: the server address, key, sign-in and status. */
void draw_settings() noexcept;
void handle_settings_input() noexcept;

} // namespace slopfin::seerr

#endif
