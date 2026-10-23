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
/* Arrives on the Discover tab: opens it the first time, and afterwards keeps
   the category and position that were left, refreshing what may be stale. */
void enter() noexcept;
/* Release this tab's microphone/result when navigating elsewhere. */
void leave() noexcept;

/* What Discover's sidebar lists: the catalogue's categories, then requests. */
inline constexpr int kSectionCount = 6;
inline constexpr int kRequestsSection = kSectionCount - 1;
const char *section_label(int section) noexcept;
/* Which section is open, 0 .. kSectionCount - 1. */
int section() noexcept;
void set_section(int section) noexcept;
/* The request server's own name, for the sidebar's footer. */
std::string server_label() noexcept;
/* False when the viewer leaves; the caller returns to the sidebar. */
bool handle_input(float elapsed = 1.0f / 60.0f) noexcept;
bool modal_open() noexcept;
bool covers_background() noexcept;
bool take_settings_request() noexcept;
std::string take_open_item() noexcept;
void close_settings() noexcept;
#ifdef SLOPFIN_HOST
void preview_search(const std::string &query) noexcept;
#endif
/* content_focused is false while the app's sidebar holds the input, so no
   card or row here shows a focus of its own. */
void draw(bool content_focused = true) noexcept;
/* Services a queued keyboard. Call once per frame from the render loop. */
void poll() noexcept;
bool take_library_changed() noexcept;

/* Settings -> Discover: the server address, key, sign-in and status. */
void open_settings() noexcept;
void draw_settings() noexcept;
void handle_settings_input() noexcept;

} // namespace slopfin::seerr

#endif
