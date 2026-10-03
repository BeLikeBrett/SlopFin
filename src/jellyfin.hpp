/*
 * SlopFin - Jellyfin API client.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every call blocks, so the UI drives these from a worker thread rather than
 * the render loop.
 */

#ifndef SLOPFIN_JELLYFIN_HPP
#define SLOPFIN_JELLYFIN_HPP

#include "http.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace slopfin::jellyfin
{

/* One playable file behind a title: a 4K and a 1080p cut are two of these. */
/* One selectable audio or subtitle stream within a version. */
struct Track
{
    int index = -1;    /* the server's stream index, used to select it */
    std::string label; /* DisplayTitle, e.g. "English - Dolby Digital+ - 5.1" */
    std::string language;
    std::string codec;
    /* The server's Profile: "DTS-HD MA" against plain "DTS", or
       "Dolby TrueHD + Dolby Atmos". The codec alone cannot tell them apart. */
    std::string profile;
    int channels = 0;
    long long bitrate = 0; /* bits per second, when the server knows it */
    bool is_default = false;
    bool is_forced = false;
    bool is_text = false; /* text can be drawn by the app; images are burned in */
    bool is_external = false;
};

struct MediaSource
{
    std::string id;
    std::string name;        /* the file's display name */
    std::string video_codec; /* h264, hevc, av1 */
    std::string audio_codec; /* eac3, ac3, dts, aac */
    int width = 0;
    int height = 0;
    double frame_rate = 0.0; /* the video's own rate, 23.976 and so on */
    int audio_channels = 0;
    long long bitrate = 0;
    bool has_subtitles = false;
    bool subtitles_sdh = false;
    std::vector<Track> audio_tracks;
    std::vector<Track> subtitle_tracks;
    /* The file itself, for the Details sheet. */
    std::string path;      /* as the server sees it */
    std::string container; /* mkv, mp4, ts */
    long long size_bytes = 0;
    std::string video_profile; /* Main 10, High */
    double video_level = 0.0;  /* as the server reports it: 153 for HEVC 5.1, 41 for H.264 4.1 */
    int video_bit_depth = 0;
    std::string video_range; /* VideoRangeType: SDR, HDR10, HDR10Plus, HLG, DOVIWithEL ... */
    std::string video_dovi;  /* "Dolby Vision Profile 7.6 (HDR10)" when present */
    long long video_bitrate = 0;
    std::string aspect_ratio;  /* "16:9", "2.40:1" */
    int default_audio = -1;    /* chosen by the server from the user's preferences */
    int default_subtitle = -1; /* -1 means off by default */

    /* Short badges: 1080p, HEVC, DD+, 5.1 and so on. */
    [[nodiscard]] std::vector<std::string> badges() const noexcept;
    [[nodiscard]] std::string quality_label() const noexcept;
};

/* Someone credited on a title.  */
struct Person
{
    std::string id;
    std::string name;
    std::string role;      /* the character, for an actor */
    std::string type;      /* Actor, Director, Writer, Producer, GuestStar */
    std::string image_tag; /* their photo, empty when none */
};

struct Item
{
    std::string id;
    std::string name;
    std::string overview;
    std::string type;        /* Movie, Series, Episode, CollectionFolder */
    std::string series_name; /* Episodes only */
    std::string image_tag;   /* Primary image, empty when absent */
    std::string backdrop_tag;
    std::string series_id; /* Episodes: the show they belong to */
    /* An episode's own Primary image is a 16:9 still, and a still squeezed
       into a poster-shaped card loses most of the frame. The show's poster is
       the right picture for a card, so the server is asked for its tag too. */
    std::string series_primary_tag;
    std::string parent_backdrop_id; /* Whichever item owns the backdrop */
    std::string parent_backdrop_tag;
    /* The show's own lettering, when it has any: series carry it more often
       than episodes do, so the owner is recorded alongside the tag. */
    int child_count = 0; /* episodes in a season, seasons in a series */
    std::string logo_tag;
    std::string parent_logo_id;
    std::string parent_logo_tag;
    int production_year = 0;
    int index_number = 0;        /* Episode number */
    int parent_index_number = 0; /* Season number */
    int runtime_minutes = 0;
    double community_rating = 0.0; /* TMDB audience score, out of 10 */
    double critic_rating = 0.0;    /* Rotten Tomatoes Tomatometer, out of 100 */
    double played_percentage = 0.0;
    double resume_ticks = 0.0;
    bool played = false;
    std::string genres;
    std::string official_rating;
    std::string premiere_date; /* ISO 8601 as the server sends it */
    std::string date_created;  /* when the server added it, ISO 8601 */
    /* The detail page's extra facts . */
    std::vector<Person> people;
    std::string studios; /* "Epix, MGM+" */
    std::string tagline;
    std::string status;           /* a series: Continuing or Ended */
    std::string end_date;         /* a series: when its last episode aired */
    std::string season_id;        /* an episode's season */
    int recursive_item_count = 0; /* a series: its episodes */
    /* Playback segment markers, in title seconds. MediaSegments win; named
       chapters are the fallback. Intro requires a real end so Skip Intro never
       guesses how far to jump. */
    double intro_start_seconds = -1.0;
    double intro_end_seconds = -1.0;
    double credits_start_seconds = -1.0;
    std::vector<MediaSource> sources;
};

struct Library
{
    std::string id;
    std::string name;
    std::string collection_type; /* movies, tvshows, music, ... */
};

/* Sign-in without a keyboard: the console shows a code, the user approves it. */
struct QuickConnectSession
{
    std::string secret;
    std::string code;
    bool video_delivery_known = false;
    bool valid = false;
};

QuickConnectSession quick_connect_begin() noexcept;

/* Polls once. Returns true when the session has been approved and stored. */
bool quick_connect_poll(const QuickConnectSession &session) noexcept;

/* Verifies a stored token still works. */
bool verify_session() noexcept;

/* Classic sign-in, for servers where Quick Connect is off. */
bool authenticate(const std::string &username, const std::string &password) noexcept;

/* Confirms a host answers as a Jellyfin server, and reports its name. */
bool probe(const std::string &host, int port, std::string &server_name) noexcept;

void sign_out() noexcept;

/* Marks an item watched, which is how it leaves Continue Watching. */
bool mark_played(const std::string &item_id) noexcept;

std::vector<Library> libraries() noexcept;

/* Newest items across the whole account, for the home rows. */
std::vector<Item> latest(const std::string &library_id, int limit) noexcept;
std::vector<Item> resume(int limit) noexcept;
std::vector<Item> next_up(int limit) noexcept;
/* The one episode Next Up holds for a single series, or none.  */
std::vector<Item> next_up(int limit, const std::string &series_id) noexcept;
/*
 * `total_out`, when given, receives how many items the library holds in all,
 * which is not the same as how many were returned: the grid asks for a page.
 */
std::vector<Item> items_in(const std::string &library_id, int start, int limit,
                           int *total_out = nullptr) noexcept;

/*
 * One page of every item of the given types in the whole account, in name
 * order, carrying little beyond ids and image tags. For warming artwork, not
 * for display. Safe to call from any thread: unlike the calls above it does
 * not write last_error().
 */
std::vector<Item> catalogue(const std::string &types, int start, int limit,
                            int *total_out = nullptr) noexcept;
/*
 * Searching inside a category means searching that category. `library_id`
 * empty searches the whole account; `types` is a comma-separated list of
 * Jellyfin item types, empty meaning films, series and episodes together.
 */
std::vector<Item> search(const std::string &term, int limit, const std::string &library_id = {},
                         const std::string &types = {}) noexcept;

/* Full record for one item, including fields the list endpoints omit. */
bool details(const std::string &item_id, Item &out) noexcept;

/* Seasons of a series, then episodes of a season. */
std::vector<Item> seasons(const std::string &series_id) noexcept;
std::vector<Item> episodes(const std::string &series_id, const std::string &season_id) noexcept;

/* Raw bytes of an item's artwork, ready for the image decoder. */
std::vector<unsigned char> image(const std::string &item_id, const std::string &tag,
                                 const char *kind, int max_height,
                                 int *status_out = nullptr) noexcept;

/*
 * What the server decided to send for one item. SlopFin declares its real
 * decoder limits, so the server can copy the video stream into a transport
 * stream instead of re-encoding it whenever the source is already within them.
 */
/* Everything the user can choose about one play of one item. */
struct PlaybackRequest
{
    std::string item_id;
    std::string media_source_id; /* empty: the server's default version */
    double start_seconds = 0.0;
    int audio_index = -1;      /* -1: the server's default track */
    int subtitle_index = -1;   /* -1: subtitles off */
    long long max_bitrate = 0; /* bits per second; 0: no limit */
    /* Empty asks the server to keep the source codec where it can, which is
       what leaves a stream copied rather than re-encoded. */
    std::string video_codec;
    int max_audio_channels = 0; /* 0: as many as the console can take */
    /* A format the audio is converted to -- "eac3", "ac3", "aac" -- or empty
       for the track's own. Chosen in the player's Audio panel. */
    std::string audio_codec;
    bool allow_software_audio = false; /* opt-in CPU streaming trial */
    bool allow_bitstream = false;      /* E-AC-3 and DTS may be copied for the TV to decode */
    bool allow_dv_hdr10_base = false;  /* opt-in, metadata-validated base-layer trial */
};

struct PlaybackPlan
{
    std::string path;         /* request path, query string included */
    std::string play_session; /* session id, for progress reporting */
    std::string video_codec;  /* source codec, not necessarily delivered codec */
    std::string audio_codec;
    std::string audio_profile;
    int audio_channels = 2;
    int audio_sample_rate = 0; /* selected source track; sink requires 48000 */
    bool direct = false;       /* static TS direct play; false can still copy video */
    bool hdr = false;          /* source is BT.2020 PQ and needs tone mapping */
    bool dv_hdr10_base = false;
    std::string source_video_range;
    int bit_depth = 8;         /* 10 means the decoder must open as Main 10 */
    int width = 0, height = 0; /* source picture; sizes the decoder, 0 when unknown */
    double frame_rate = 0.0;   /* the source video's rate, for display cadence */
    std::string media_source_id;
    std::string subtitle_url;         /* SubRip to fetch and draw; empty when none */
    bool subtitle_burned = false;     /* the server draws it into the picture */
    bool absolute_timestamps = false; /* stream time equals title time */
    bool video_delivery_known = false;
    bool valid = false;
};

/*
 * Negotiates playback through /Items/{id}/PlaybackInfo with a device profile.
 * Returns an invalid plan if negotiation fails; never silently ignores choices.
 */
PlaybackPlan playback_plan(const PlaybackRequest &request) noexcept;

/* GET a text resource from the server, such as a subtitle; empty on failure. */
std::string fetch_text(const std::string &path) noexcept;

/* POST a JSON body; true when the server accepted it. */
bool post_json(const std::string &path, const std::string &body) noexcept;

/* Read the server's live copy/transcode decisions after opening the stream. */
std::string playback_delivery(PlaybackPlan &plan) noexcept;

/* ------------------------------------------------ account
 *
 * These report failure through `error` rather than last_error(): the profile
 * and dashboard screens call them from threads of their own, and last_error()
 * belongs to the main data thread.
 */
struct User
{
    std::string id;
    std::string name;
    std::string primary_image_tag; /* empty when the user has no picture */
    std::string last_activity;     /* ISO 8601 */
    std::string last_login;
    bool is_admin = false;
    bool is_disabled = false;
    bool has_password = false;
    /* The policy switches the dashboard offers.  */
    bool remote_access = true;
    bool can_download = false;
    bool live_tv = false;
    bool can_play = true;
};

/* The signed-in user. */
bool current_user(User &out, std::string &error) noexcept;
/* Every user on the server; administrators only. */
std::vector<User> users(std::string &error) noexcept;
/* A user's picture, as JPEG bytes. */
std::vector<unsigned char> user_image(const std::string &user_id, const std::string &tag) noexcept;
/* Changes the signed-in user's password. */
bool change_password(const std::string &current, const std::string &next,
                     std::string &error) noexcept;
/* Replaces the signed-in user's picture with these JPEG bytes, or removes it. */
bool upload_user_image(const std::vector<unsigned char> &jpeg, std::string &error) noexcept;
bool delete_user_image(std::string &error) noexcept;

/* The Authorization header, for a stream the dashboard reads itself. */
std::vector<std::string> authorization() noexcept;

/* Any authorised call, for the dashboard. `body` is sent as `content_type`. */
http::Response api(std::string_view method, const std::string &path, const std::string &body = {},
                   std::string_view content_type = "application/json") noexcept;

/* Human-readable description of the last failure, for the UI. */
const std::string &last_error() noexcept;

} // namespace slopfin::jellyfin

#endif
