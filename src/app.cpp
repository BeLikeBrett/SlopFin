/*
 * SlopFin - screens, navigation and the render loop's contents.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Network work happens on one background thread. The render loop only reads a
 * snapshot of shared state behind a mutex, so it never blocks on the server.
 */

#include "app.hpp"

#include "audio.hpp"
#include "bigalloc.hpp"
#include "account.hpp"
#include "audiocaps.hpp"
#include "autoplay.hpp"
#include "bitstream_probe.hpp"
#include "background.hpp"
#include "crash.hpp"
#include "dashboard.hpp"

#include "config.hpp"
#include "diagnostics.hpp"
#include "delivery_summary.hpp"
#include "details_sheet.hpp"
#include "gfx.hpp"
#include "home_rows.hpp"
#include "icons.hpp"
#include "images.hpp"
#include "ime.hpp"
#include "jellyfin.hpp"
#include "motion.hpp"
#include "pad.hpp"
#include "player.hpp"
#include "seek.hpp"
#include "server_address.hpp"
#include "search_sections.hpp"
#include "subtitle_style.hpp"
#include "subtitle_timing.hpp"
#include "subtitles.hpp"
#include "telemetry.hpp"
#include "text.hpp"
#include "trace.hpp"
#include "ui_common.hpp"
#include "warm.hpp"

#include <atomic>
#include <condition_variable>
#include <optional>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

extern "C"
{
    int scePthreadCreate(void **thread, const void *attr, void *(*entry)(void *), void *argument,
                         const char *name);
    int sceKernelUsleep(std::uint32_t microseconds);

    int sceKernelAvailableFlexibleMemorySize(std::size_t *size);
    int scePthreadSetaffinity(void *thread, std::uint64_t mask);
    int unlink(const char *path);
    int open(const char *path, int flags, ...);
    long read(int descriptor, void *buffer, unsigned long count);
    int close(int descriptor);
}
constexpr int kReadOnly = 0;

namespace
{
using namespace slopfin;
using text::Weight;

/* ---------------------------------------------------------------- layout */

/* The safe area, sidebar and content edges live in ui_common.hpp. */
using ui::kContentRight;
using ui::kContentWidth;
using ui::kContentX;
using ui::kGridTop;
using ui::kSafeX;
using ui::kSafeY;
using ui::kSidebarWidth;
/* The play/pause badge, and the row of transport controls it sits at. */
constexpr int kTransportBadge = 76;
constexpr int kTransportRow = gfx::kHeight - 306;

constexpr int kCardWidth = 240;
constexpr int kCardHeight = 360; /* 2:3 poster */
/* A season's poster in the strip under a show's page. Warming asks for it at
   this size, so what is warmed is what gets drawn. */
constexpr int kSeasonCardHeight = 315;
/* An episode's still in the strip under a season's page. */
constexpr int kEpisodeStripHeight = 225;
/* The Next Up still on a series page, in the hero's top right. */
constexpr int kNextUpCardWidth = 480;
constexpr int kNextUpCardHeight = 270;
/* What detail_action held when Next Up could take the focus; it no longer can.
   -1 is the strip, 0.. the buttons. */
constexpr int kDetailNextUp = -2;
/* The Cast & Crew row, when it has the focus. */
constexpr int kDetailCast = -3;
/* The Version bar above the buttons, for a title with several files. */
constexpr int kDetailVersion = -4;
constexpr int kCastPhotoHeight = 180;

constexpr int kCardGap = 26;
/* One radius for every poster on every screen. Artwork is masked to it
   directly, so nothing is drawn behind a card for its corners to reveal. */
constexpr int kCardRadius = 10;
/* Resume and next-up rows use landscape episode thumbnails. */
/* 480x270 rather than 400x225: next to a 360-tall film poster the shorter card
   read as the lesser of the two, when the two are equals. */
constexpr int kWideWidth = 480;
constexpr int kWideHeight = 270;
/* Movies retain poster artwork in mixed rows. */
constexpr int kRowTitleHeight = 54;
/* Card, both label lines, then clear air before the next row title. */
/* Tight enough that the rows and the hero read as one screen rather than two
   separated by an empty band. */
constexpr int kRowsTop = 498;

constexpr int kGridColumns = 5;
constexpr int kGridRowHeight = kCardHeight + 96;
/* Where the scrolling region starts, and how tall it is. The cards are
   clipped to this and the header is redrawn over its top edge, so a row
   half-way off the top fades out instead of being cut with a knife. */
constexpr int kGridClipTop = kGridTop - 30;
constexpr int kGridFade = 34;
constexpr int kGridViewHeight = gfx::kHeight - kGridTop - 40;
/* Search stays in its originating category; the PS5 owns text entry. */
constexpr int kSearchBarX = kContentX;
constexpr int kSearchBarY = kSafeY + 78;
constexpr int kSearchBarH = 72;
constexpr int kSearchBarW = 832;

/* The position bar: clear of the last column, inside the title-safe edge. */
constexpr int kBarWidth = 6;
constexpr int kBarX = gfx::kWidth - kSafeX - 14;
constexpr int kBarTop = kGridTop - 12;
constexpr int kBarHeight = gfx::kHeight - kBarTop - 56;

/* ------------------------------------------------------------------ state */

enum class Screen
{
    connecting,
    server_setup,
    sign_in,
    home,
    grid, /* a library, or search results */
    detail,
    playing,
    settings,
    profile,   /* the signed-in user's page (account.cpp) */
    dashboard, /* the server's administration pages (dashboard.cpp) */
};

struct Row
{
    std::string title;
    std::vector<jellyfin::Item> items;
    /* Landscape cards, for the rows whose pictures are frames rather than
       posters: what to carry on with, and what is next. */
    bool wide = false;
};

/* What the UI wants the worker to do next. */
enum class Request
{
    none,
    probe_server,
    quick_connect,
    password_login,
    reload_home,
    /* Continue Watching and Next Up only. Separate from reload_home because
       that one also re-walks the libraries and restarts the artwork warm,
       which is far too much to do every time playback ends. */
    refresh_progress,
    /* The negotiation Play would run, for the Details sheet's "On this PS5". */
    probe_delivery,
    open_library,
    run_search,
    open_detail,
    /* The episodes either side of the one playing, so the controls can offer
       the next one without going back to the series page. */
    load_siblings,
    /* Full details for a neighbouring episode, which the episode list does not
       carry: playback needs its media sources. */
    play_sibling,
    mark_played,
    sign_out,
};

struct Shared
{
    std::mutex mutex;
    Screen screen = Screen::connecting;
    std::string status = "Connecting";
    std::string detail;
    std::string server_name;

    jellyfin::QuickConnectSession session;
    std::vector<jellyfin::Library> libraries;
    std::uint64_t libraries_epoch = 0;
    std::vector<Row> rows;
    std::uint64_t rows_epoch = 0;

    std::vector<jellyfin::Item> grid_items;
    std::uint64_t grid_epoch = 0;
    std::string grid_title;
    std::string grid_library_id; /* so the grid can be asked for again */
    /* Where a return to this grid wants the cursor. Applied by the render
       thread once the items are there, because until then the clamp would
       lose it. */
    int grid_restore = -1;
    int grid_total = 0; /* what the library holds, not what this page returned */
    bool grid_loading = false;

    /* Search results live apart from grid_items so that closing the search
       leaves the category exactly as it was. */
    std::vector<jellyfin::Item> search_items;
    std::uint64_t search_epoch = 0;
    std::string search_term;
    std::string search_error;
    std::string search_target_term;
    std::string search_target_scope;
    std::string search_target_types;
    bool search_loading = false;

    bool report_ready = false;
    bool report_sent = false;
    std::string report_message;

    jellyfin::Item detail_item;
    std::uint64_t detail_epoch = 0;
    /* Which detail page the in-flight lower-row request belongs to. Results
       for an older page are discarded instead of flashing into the new one. */
    std::string detail_target_id;
    std::vector<jellyfin::Item> detail_episodes;
    /* A series' own Next Up, shown above its seasons; empty id when none. */
    jellyfin::Item detail_next_up;
    bool detail_loading = false;

    /* Cached episode order and the active sibling index. */
    std::vector<jellyfin::Item> siblings;
    int sibling_index = -1;
    /* Prepared on the worker, started on the render thread. */
    jellyfin::Item pending_play;
    bool pending_play_ready = false;
    /* Start the pending episode where it was left, rather than at the top: the
       series page's Next Up does, the next-episode button does not. */
    bool pending_play_resume = false;
    bool sibling_loading = false;

    /* Marks Home progress rows stale after playback or a watched-state change. */
    bool progress_stale = false;

    /* "On this PS5": what was asked, for which title and version, and the
       answer once the worker has it. */
    jellyfin::PlaybackRequest delivery_request;
    std::string delivery_key;
    std::vector<details::Line> delivery_lines;
    bool delivery_ready = false;
    /* Rebuild generation; partial refreshes preserve the card reveal state. */
    long home_epoch = 0;

    Request request = Request::none;
    std::string request_text;   /* host, search term, or username */
    std::string request_scope;  /* library to search inside; empty means all */
    std::string request_types;  /* item types to return; empty means all three */
    std::string request_secret; /* password, cleared immediately after use */
    int request_port = 8096;
    bool busy = false;
};

Shared g_shared;
std::condition_variable g_request_wake;

/* Cursor, animation and everything else only the render thread touches. */
struct View
{
    int sidebar_index = 0;  /* where the cursor is while browsing the list */
    int active_section = 0; /* which section is actually open */
    bool sidebar_focused = false;

    int row_index = 0;
    std::vector<int> column;
    motion::Spring scroll;
    std::vector<motion::Spring> row_offset;

    int grid_index = 0;
    motion::Spring grid_scroll;

    /* How far the content is dimmed while the sidebar holds the input. */
    motion::Spring sidebar_dim;
    motion::Spring sidebar_scroll;

    std::string server_draft;
    bool server_draft_initialized = false;
    int setup_index = 0; /* server_setup and sign_in menus */
    int settings_index = 0;
    /* Settings is a menu of screens: 0 the menu, then Subtitles, Controller,
       Server and Account in menu order. */
    int settings_page = 0;
    int account_row = 0; /* the row focused on the Account screen */
    /* Where Circle goes from the Profile and Dashboard screens. */
    Screen return_screen = Screen::home;
    int subtitle_row = 1;      /* the row focused on the Subtitles screen */
    int detail_action = 0;     /* 0 Play, 1 Watched, 2 Version */
    int detail_source = 0;     /* which media version is selected */
    bool details_open = false; /* the Details sheet over the detail screen */
    /* A category's cards on their way out while the next one arrives. */
    std::vector<jellyfin::Item> leaving_items;
    float leaving_scroll = 0.0f;
    long leaving_frame = -1;
    int details_scroll = 0; /* first line of the sheet on screen */
    int detail_episode = 0;
    motion::Spring detail_scroll;
    /* The detail page scrolling as a whole, and its Cast & Crew row. */
    motion::Spring detail_page;
    motion::Spring detail_cast_scroll;
    int detail_cast_index = 0;
    std::string detail_strip_placed_for; /* the episode whose strip was opened at itself */
    /* 0 the moment the focus moves, 1 once the card has taken it. The spring
       overshoots a little, which is the whole point: a card that eases to a
       stop has no weight. */
    motion::Spring focus;
    /* Shared page-transition progress. */
    motion::Spring page_in{1.0f};
    /*
     * The offer to send a crash report, shown once per session when the
     * console is holding one the server has not seen .
     */
    enum class ReportPrompt : unsigned char
    {
        none,
        offer,
        sending,
        sent,
        failed
    };
    ReportPrompt report_prompt = ReportPrompt::none;
    int report_choice = 0; /* 0 send, 1 not now */
    int report_leaves = 0; /* frames until the answer clears itself */
    int diagnostics_row = 0;
    int diagnostics_hold = 0;
    int playback_row = 0;
    std::string report_line;
    Screen page_shown = Screen::connecting;
    bool page_back = false; /* going back moves the other way */
    int frames = 0;

    /*
     * Player overlay. The controls appear on any input and retire by
     * themselves, which is what keeps a picture unobstructed while it plays.
     */
    bool osd_visible = false;
    bool osd_dismissed = false; /* put away on purpose, so pausing leaves it away */
    /* Directional input owner for the playback controls. */
    enum class OsdFocus : std::uint8_t
    {
        bar = 0,  /* left and right scrub */
        modules,  /* left and right move along the settings row */
        episodes, /* left and right choose previous or next */
    } osd_focus = OsdFocus::bar;
    int episode_choice = 0; /* 0 previous, 1 next */
    /* What happens when the episode runs out; see autoplay.hpp. */
    autoplay::State autoplay;
    motion::Spring up_next_in{0.0f};
    /* Intro prompt: metadata decides the interval, the player only remembers
       whether this episode's prompt was dismissed. */
    bool intro_visible = false;
    bool intro_dismissed = false;
    bool intro_seek_failed = false;
    motion::Spring intro_in{0.0f};
    /* The episode it is offering, held so the card can draw its picture. */
    jellyfin::Item up_next;
    /* What is playing, for the screen that follows it. */
    jellyfin::Item playing_item;
    int end_choice = 0;
    int ask_choice = 0;
    /* Retain the last playback state for the end card. */
    bool finished = false;
    /* A seek restarts the stream; while that is happening the wait needs no
       words, because the scrubber is already showing where it is going. */
    bool seek_restart = false;
    std::uint64_t osd_idle = 0; /* time of last input, in microseconds */
    int osd_button = 0;
    bool panel_open = false;
    int panel_kind = 0;   /* 0 subtitles, 1 audio, 2 quality */
    int panel_anchor = 0; /* the button it rises from */
    int panel_index = 0;
    /* Persisted per-item subtitle timing, in seconds. */
    double subtitle_offset = 0.0;
    std::string subtitle_offset_item;
    bool subtitle_offset_dirty = false;
    int timing_held = 0; /* frames Cross has been held on a timing row */
    bool show_debug = false;
    /* A run of skips is gathered and committed once, because every seek
       restarts the stream. */
    double seek_target = -1.0;
    double seek_origin = -1.0; /* where the run of skips started from */
    double seek_pending = 0.0; /* how far it has gathered, signed */
    int seek_commit = 0;
    /* Whether the picture was running when the skipping started, so it can be
       put back the way it was found. */
    bool seek_was_playing = false;
    /* Frames the current continuous seek input has been held, which is what
       the acceleration is a function of. */
    int seek_hold = 0;
    int flash = 0; /* frames left of the ring on the transport badge */
    /* The version being played, for the track lists the panel offers. */
    jellyfin::MediaSource playing_source;
    /* What the controls say this is: the show or film, the episode line under
       it, and whichever item owns the show's lettering. */
    std::string playing_series;  /* "Silo", or a film's title */
    std::string playing_episode; /* "S3:E5 - Memory", empty for a film */
    std::string playing_logo_id;
    std::string playing_logo_tag;
    /* Whether there is an episode either side of this one, which is what
       decides whether the two buttons under the timeline are offered. */
    bool has_previous = false;
    bool has_next = false;
    std::string previous_id;
    std::string next_id;
    bool changing_episode = false;

    /* One scoped search state shared across browsing screens. */
    struct Search
    {
        bool open = false;
        std::string draft;     /* what is being typed */
        std::string committed; /* what has actually been asked for */
        std::string committed_scope, committed_types;
        std::string keyboard_notice;
        bool refresh_requested = false;
        int pending = 0; /* submit after the system dialog returns */
        int result_index = 0;
        motion::Spring scroll;
        std::array<motion::Spring, 3> rail;
        motion::Spring reveal; /* 0 closed, 1 open */
    } search;

    /* What the pending keyboard is for. */
    enum class Prompt
    {
        none,
        host,
        username,
        password,
        search,
    } prompt = Prompt::none;
    std::string pending_username;
    std::string pending_password;
    bool credential_form = false;
    bool show_password = false;
    int ime_input_guard = 0;
};

View g_view;

/* Sidebar entries use server library IDs; labels are display-only. */
struct SidebarEntry
{
    enum class Kind : std::uint8_t
    {
        home = 0,
        search,
        library,
        settings,
    };

    std::string label;
    Kind kind = Kind::home;
    std::string library_id;
    bool gap_before = false; /* a little air between the groups */
};

std::vector<SidebarEntry> build_sidebar(const std::vector<jellyfin::Library> &libraries) noexcept
{
    std::vector<SidebarEntry> entries;
    entries.push_back({"Home", SidebarEntry::Kind::home, {}, false});

    bool first_library = true;
    for (const jellyfin::Library &library : libraries)
    {
        /* Skip the views this client cannot show anything useful for. */
        if (library.collection_type == "music" || library.collection_type == "books" ||
            library.collection_type == "photos" || library.collection_type == "livetv")
            continue;
        entries.push_back({library.name, SidebarEntry::Kind::library, library.id, first_library});
        first_library = false;
    }

    entries.push_back({"Settings", SidebarEntry::Kind::settings, {}, true});
    return entries;
}

/* Render-loop timing, so a stutter can be attributed rather than guessed at. */
extern "C" int sceKernelGettimeofday(void *timeval);
/* FreeBSD's timezone: minutes west of Greenwich, and whether DST applies. */
extern "C" int sceKernelGettimezone(void *timezone);

std::uint64_t g_draw_us = 0;
std::uint64_t g_loop_us = 0;
std::uint64_t g_last_loop_us = 0;
std::uint64_t g_present_us = 0;
std::uint64_t g_pad_us = 0;
long g_last_shown = 0;

/* ------------------------------------------------------------- animation */

/* Use measured frame time so springs remain stable after a missed refresh. */
float frame_seconds() noexcept
{
    if (g_loop_us == 0)
        return 1.0f / 60.0f;
    return std::clamp(static_cast<float>(g_loop_us) / 1000000.0f, 1.0f / 240.0f, 1.0f / 15.0f);
}

/* Fade newly decoded artwork over a short interval. */
std::unordered_map<std::string, int> g_art_first_frame;

/* Track placeholders so cached images do not fade unnecessarily. */
std::unordered_set<std::string> g_awaited;

/* Record whether artwork was cached when a card first appeared. */
/* Card reveal starts when a category result arrives. */
std::unordered_set<std::string> g_first_drawn;
int g_reveal_frame = -1;
std::string g_reveal_of;

/* Refresh Home after playback and periodically while idle. */
bool g_was_playing = false;
long g_progress_ready_frame = -1;
long g_progress_asked_frame = -1;
constexpr long kProgressSettleFrames = 45; /* 0.75 s at 60 Hz */
constexpr long kProgressPollFrames = 1800; /* re-ask every 30 s sitting on Home */

/* Ease-out cubic: quick off the mark, settling rather than stopping. */
float ease_out(float t) noexcept
{
    t = std::clamp(t, 0.0f, 1.0f);
    const float inverse = 1.0f - t;
    return 1.0f - inverse * inverse * inverse;
}

/* Stagger card reveals by row and column. */
/* Card reveals combine opacity and a small translation. */
#ifndef SLOPFIN_REVEAL_FRAMES
#define SLOPFIN_REVEAL_FRAMES 39.0f /* 0.65-second card reveal */
#endif
constexpr float kRevealFrames = SLOPFIN_REVEAL_FRAMES;
constexpr float kCascadeFrames = 18.0f; /* the whole sweep's spread, 0.3 s */
constexpr float kRevealRise = 14.0f;    /* pixels a card rises through */
/* Home's rows run sideways, so there cards slide in from the right instead
   . Farther than the
   rise, because a horizontal move across wide cards reads as smaller. */
constexpr float kRevealSlide = 56.0f;
bool g_reveal_sideways = false; /* set by draw_home around its cards */
/* Diagonal steps from the first card on screen to the last, set by whoever
   is drawing the cards just before it draws them. */
int g_reveal_span = 1;

float card_reveal(int column, int row) noexcept
{
    if (g_reveal_frame < 0)
        return 1.0f;
    const float stagger =
        std::clamp(kCascadeFrames / static_cast<float>(std::max(1, g_reveal_span)), 1.5f, 4.0f);
    const float elapsed = static_cast<float>(g_view.frames - g_reveal_frame);
    const float delay = static_cast<float>(column + row) * stagger;
    return std::clamp((elapsed - delay) / kRevealFrames, 0.0f, 1.0f);
}

/* 0 to 1 with no jump at either end. */
float smoothstep(float edge0, float edge1, float x) noexcept
{
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
long g_first_ready = 0;
long g_first_missing = 0;
long g_first_reported = 0;

std::uint8_t artwork_fade(const std::string &key) noexcept
{
    constexpr int kFadeFrames = 9;
    if (g_awaited.find(key) == g_awaited.end())
        return 255;
    auto [entry, inserted] = g_art_first_frame.try_emplace(key, g_view.frames);
    if (!inserted && entry->second > g_view.frames)
        entry->second = g_view.frames; /* the frame counter was reset */
    const int age = g_view.frames - entry->second;
    if (age >= kFadeFrames)
        return 255;
    const float t = motion::ease_out_cubic(static_cast<float>(age) / kFadeFrames);
    return static_cast<std::uint8_t>(t * 255.0f + 0.5f);
}

/* Restart focus growth when the selected item changes. */
void restart_focus() noexcept
{
    g_view.focus.precision(0.002f);
    g_view.focus.jump(0.0f);
    g_view.focus.aim(1.0f);
}

/* One frame of every animation in the interface. */
void advance_scroll_hint() noexcept;

void advance_motion() noexcept
{
    const float dt = frame_seconds();
    ui::begin_frame(dt);
    advance_scroll_hint();
    g_view.focus.step(motion::kFocus, dt);
    g_view.scroll.step(motion::kScroll, dt);
    g_view.grid_scroll.step(motion::kScroll, dt);
    g_view.detail_scroll.step(motion::kSlide, dt);
    g_view.detail_page.step(motion::kScroll, dt);
    g_view.detail_cast_scroll.step(motion::kSlide, dt);
    g_view.search.scroll.step(motion::kScroll, dt);
    for (auto &offset : g_view.search.rail)
        offset.step(motion::kSlide, dt);
    g_view.search.reveal.step(motion::kFade, dt);
    g_view.page_in.step(motion::kFade, dt);
    g_view.up_next_in.aim(g_view.autoplay.card_visible() ? 1.0f : 0.0f);
    g_view.up_next_in.step(motion::kFade, dt);
    g_view.intro_in.aim(g_view.intro_visible ? 1.0f : 0.0f);
    g_view.intro_in.step(motion::kFade, dt);
    g_view.sidebar_dim.precision(0.004f);
    g_view.sidebar_dim.aim(g_view.sidebar_focused ? 1.0f : 0.0f);
    g_view.sidebar_dim.step(motion::kFade, dt);
    g_view.sidebar_scroll.step(motion::kSlide, dt);
    for (motion::Spring &row : g_view.row_offset)
        row.step(motion::kSlide, dt);
}

std::mutex g_detail_cache_mutex;
std::unordered_map<std::string, jellyfin::Item> g_detail_cache;
struct CachedDetailRows
{
    std::vector<jellyfin::Item> entries;
    jellyfin::Item next_up;
};
std::unordered_map<std::string, CachedDetailRows> g_detail_rows_cache;
std::string g_prefetching;
std::string g_prefetching_rows;

std::optional<jellyfin::Item> cached_detail(const std::string &id) noexcept
{
    std::lock_guard<std::mutex> guard(g_detail_cache_mutex);
    const auto found = g_detail_cache.find(id);
    if (found == g_detail_cache.end())
        return std::nullopt;
    return found->second;
}

void remember_detail(const jellyfin::Item &item) noexcept
{
    if (item.id.empty())
        return;
    std::lock_guard<std::mutex> guard(g_detail_cache_mutex);
    /* A few dozen titles is a couple of hundred kilobytes; a whole library
       would not be. Oldest out first, roughly, by clearing when it fills. */
    if (g_detail_cache.size() > 48)
        g_detail_cache.clear();
    g_detail_cache[item.id] = item;
}

std::optional<CachedDetailRows> cached_detail_rows(const std::string &id) noexcept
{
    std::lock_guard<std::mutex> guard(g_detail_cache_mutex);
    const auto found = g_detail_rows_cache.find(id);
    if (found == g_detail_rows_cache.end())
        return std::nullopt;
    return found->second;
}

void remember_detail_rows(const std::string &id, const std::vector<jellyfin::Item> &entries,
                          const jellyfin::Item &next_up) noexcept
{
    if (id.empty())
        return;
    std::lock_guard<std::mutex> guard(g_detail_cache_mutex);
    if (g_detail_rows_cache.size() > 32)
        g_detail_rows_cache.clear();
    g_detail_rows_cache[id] = CachedDetailRows{entries, next_up};
}

void fetch_detail_rows(const jellyfin::Item &item, std::vector<jellyfin::Item> &entries,
                       jellyfin::Item &next_up) noexcept
{
    if (item.type == "Series")
    {
        if (std::vector<jellyfin::Item> next = jellyfin::next_up(1, item.id); !next.empty())
            next_up = std::move(next.front());
        entries = jellyfin::seasons(item.id);
        if (entries.empty())
            entries = jellyfin::episodes(item.id, {});
    }
    else if (item.type == "Season" && !item.series_id.empty())
        entries = jellyfin::episodes(item.series_id, item.id);
    else if (item.type == "Episode" && !item.series_id.empty() && !item.season_id.empty())
        entries = jellyfin::episodes(item.series_id, item.season_id);
}

void warm_detail_rows(const jellyfin::Item &item, const std::vector<jellyfin::Item> &entries,
                      const jellyfin::Item &next_up) noexcept
{
    if (!next_up.id.empty())
        images::warm(next_up.id, next_up.image_tag, kNextUpCardHeight, true);
    for (std::size_t i = 0; i < item.people.size() && i < 10; ++i)
        if (!item.people[i].image_tag.empty())
            images::warm(item.people[i].id, item.people[i].image_tag, kCastPhotoHeight, true);
    for (const jellyfin::Item &entry : entries)
        images::warm(entry.id, entry.image_tag,
                     entry.type == "Season" ? kSeasonCardHeight : kEpisodeStripHeight, true);
}

/* Prefetch settled selections without blocking input. */
void prefetch_detail(const std::string &id) noexcept
{
    if (id.empty())
        return;
    {
        std::lock_guard<std::mutex> guard(g_detail_cache_mutex);
        if (g_detail_cache.count(id) != 0 || g_prefetching == id)
            return;
        g_prefetching = id;
    }
    background::run(
        [id]
        {
            jellyfin::Item item;
            if (jellyfin::details(id, item))
            {
                remember_detail(item);
                /* The metadata prefetch is only useful if opening the page does
                   not immediately stall on its artwork. Queue the hero/poster on
                   the same idle hint; image workers do the I/O/decode off-core. */
                if (!item.image_tag.empty())
                    images::prefetch(item.id, item.image_tag, images::Kind::primary, 560);
                if (!item.backdrop_tag.empty())
                    images::prefetch(item.id, item.backdrop_tag, images::Kind::backdrop, 1080);
                else if (!item.parent_backdrop_tag.empty())
                {
                    const std::string owner =
                        item.parent_backdrop_id.empty() ? item.series_id : item.parent_backdrop_id;
                    if (!owner.empty())
                        images::prefetch(owner, item.parent_backdrop_tag, images::Kind::backdrop,
                                         1080);
                }
            }
            std::lock_guard<std::mutex> guard(g_detail_cache_mutex);
            if (g_prefetching == id)
                g_prefetching.clear();
        });
}

/* Defer expensive detail-row prefetch until the selection has remained stable. */
void prefetch_detail_rows(const std::string &id) noexcept
{
    jellyfin::Item item;
    {
        std::lock_guard<std::mutex> guard(g_detail_cache_mutex);
        if (g_detail_rows_cache.count(id) != 0 || g_prefetching_rows == id)
            return;
        const auto found = g_detail_cache.find(id);
        if (found == g_detail_cache.end())
            return;
        item = found->second;
        g_prefetching_rows = id;
    }
    background::run(
        [id, item = std::move(item)]
        {
            std::vector<jellyfin::Item> entries;
            jellyfin::Item next_up;
            fetch_detail_rows(item, entries, next_up);
            warm_detail_rows(item, entries, next_up);
            remember_detail_rows(id, entries, next_up);
            std::lock_guard<std::mutex> guard(g_detail_cache_mutex);
            if (g_prefetching_rows == id)
                g_prefetching_rows.clear();
        });
}

/* What the cursor is resting on, and since when. */
std::string g_resting_id;
int g_resting_since = 0;

/* Which page the transition is showing; see the draw function. */
Screen g_page_screen = Screen::connecting;
std::string g_page_id;
bool g_page_seen = false;
bool g_page_changed = false;

/* For the crash report, which says what was on screen. */
const char *screen_name(Screen screen) noexcept
{
    switch (screen)
    {
    case Screen::connecting:
        return "connecting";
    case Screen::server_setup:
        return "server setup";
    case Screen::sign_in:
        return "sign in";
    case Screen::home:
        return "home";
    case Screen::grid:
        return "library";
    case Screen::detail:
        return "detail";
    case Screen::playing:
        return "playing";
    case Screen::settings:
        return "settings";
    case Screen::profile:
        return "profile";
    case Screen::dashboard:
        return "dashboard";
    }
    return "?";
}

/* The table only ever grows by one entry per distinct picture, but a long
   session over a large library is a long session, so old ones are dropped. */
void trim_artwork_fades() noexcept
{
    if (g_art_first_frame.size() < 1024)
        return;
    const int cutoff = g_view.frames - 1800;
    for (auto it = g_art_first_frame.begin(); it != g_art_first_frame.end();)
    {
        if (it->second >= cutoff)
        {
            ++it;
            continue;
        }
        g_awaited.erase(it->first);
        it = g_art_first_frame.erase(it);
    }
}

/* ---------------------------------------------------------------- worker */

void set_status(Screen screen, const std::string &status, const std::string &detail = {}) noexcept
{
    std::lock_guard<std::mutex> guard(g_shared.mutex);
    g_shared.screen = screen;
    g_shared.status = status;
    g_shared.detail = detail;
}

/* Defined with the card helpers it uses, further down. */
void await_first_screen(const std::vector<Row> &rows) noexcept;

/*
 * The libraries Home draws its "Latest in ..." rows from, kept so those rows
 * can be refreshed without listing every library again.
 */
std::mutex g_home_libraries_mutex;
std::vector<jellyfin::Library> g_home_libraries;

void load_home() noexcept
{
    std::vector<Row> rows;
    if (std::vector<jellyfin::Item> items = jellyfin::resume(16); !items.empty())
        rows.push_back(Row{"Continue Watching", std::move(items), true});
    if (std::vector<jellyfin::Item> items = jellyfin::next_up(16); !items.empty())
        rows.push_back(Row{"Next Up", std::move(items), true});

    const std::vector<jellyfin::Library> libraries = jellyfin::libraries();
    std::vector<jellyfin::Library> shown;
    for (const jellyfin::Library &library : libraries)
    {
        if (library.collection_type != "movies" && library.collection_type != "tvshows" &&
            !library.collection_type.empty())
            continue;
        shown.push_back(library);
        std::vector<jellyfin::Item> items = jellyfin::latest(library.id, 16);
        if (!items.empty())
            rows.push_back(Row{"Latest in " + library.name, std::move(items), false});
    }
    {
        std::lock_guard<std::mutex> guard(g_home_libraries_mutex);
        g_home_libraries = std::move(shown);
    }

    /* Preload the initial Home artwork before leaving the loading screen. */
    bool arriving = false;
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        arriving = g_shared.screen == Screen::connecting;
    }
    if (arriving)
        await_first_screen(rows);

    std::lock_guard<std::mutex> guard(g_shared.mutex);
    g_shared.libraries = libraries;
    ++g_shared.libraries_epoch;
    g_shared.rows = std::move(rows);
    ++g_shared.rows_epoch;
    ++g_shared.home_epoch;
    if (g_shared.screen != Screen::playing)
        g_shared.screen = Screen::home;
    g_shared.detail =
        g_shared.rows.empty()
            ? "No titles to show yet. Open a library from the sidebar, or search with Triangle."
            : std::string{};
    g_shared.status = "Home";
    /* Warm remaining library artwork after sign-in. */
    /* /data/slopfin-nowarm turns it off, so the difference it makes can be
       measured on the same build. */
    if (const int marker = open("/data/slopfin-nowarm", 0); marker >= 0)
    {
        (void)close(marker);
        trace::mark("warm: disabled by /data/slopfin-nowarm");
    }
    else
    {
        warm::begin(kCardHeight + 120, kSeasonCardHeight);
    }
}

/* Refresh progress rows without replacing unrelated Home rows. */
std::atomic<bool> g_latest_due{true};
int g_refresh_turn = 0;

void refresh_progress_rows() noexcept
{
    const bool with_latest = g_latest_due.exchange(false) || (g_refresh_turn++ % 4) == 0;
    std::vector<jellyfin::Item> resumed = jellyfin::resume(16);
    std::vector<jellyfin::Item> next = jellyfin::next_up(16);

    std::vector<Row> refreshed;
    std::vector<std::string> owned{"Continue Watching", "Next Up"};
    if (!resumed.empty())
        refreshed.push_back(Row{"Continue Watching", std::move(resumed), true});
    if (!next.empty())
        refreshed.push_back(Row{"Next Up", std::move(next), true});

    std::vector<jellyfin::Library> libraries;
    if (with_latest)
    {
        std::lock_guard<std::mutex> guard(g_home_libraries_mutex);
        libraries = g_home_libraries;
    }
    for (const jellyfin::Library &library : libraries)
    {
        const std::string title = "Latest in " + library.name;
        owned.push_back(title);
        std::vector<jellyfin::Item> items = jellyfin::latest(library.id, 16);
        if (!items.empty())
            refreshed.push_back(Row{title, std::move(items), false});
    }

    /* A server that answered nothing at all is a network blip, not a library
       that emptied; leaving the rows alone beats blanking the screen. */
    if (refreshed.empty())
    {
        trace::mark("home: refresh came back empty; keeping the rows");
        return;
    }
    trace::mark("home: refreshed " + std::to_string(refreshed.size()) +
                (with_latest ? " rows with libraries" : " progress rows"));

    std::lock_guard<std::mutex> guard(g_shared.mutex);
    /* Every title is named whether or not it came back, so a row that has
       emptied since the last look is dropped rather than left behind. */
    g_shared.rows = home::merge_progress(std::move(refreshed), std::move(g_shared.rows), owned,
                                         [](const Row &row) { return row.title; });
    ++g_shared.rows_epoch;
}

void begin_quick_connect() noexcept
{
    jellyfin::QuickConnectSession session = jellyfin::quick_connect_begin();
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        g_shared.session = session;
        g_shared.screen = Screen::sign_in;
        g_shared.status = session.valid ? "Waiting for approval" : "Quick Connect unavailable";
        g_shared.detail = session.valid ? std::string{} : jellyfin::last_error();
    }
    if (!session.valid)
        return;

    for (int attempt = 0; attempt < 300; ++attempt)
    {
        (void)sceKernelUsleep(2000000);
        {
            /* A different sign-in path may have taken over in the meantime. */
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            if (g_shared.request != Request::none || g_shared.screen != Screen::sign_in)
                return;
        }
        if (jellyfin::quick_connect_poll(session))
        {
            set_status(Screen::connecting, "Loading your library");
            load_home();
            return;
        }
    }
    set_status(Screen::sign_in, "Quick Connect code expired", "Choose Quick Connect to try again.");
}

void *worker(void *) noexcept
{
    if (config::current().host.empty())
    {
        set_status(Screen::server_setup, "Choose your server");
    }
    else if (config::current().signed_in() && jellyfin::verify_session())
    {
        set_status(Screen::connecting, "Loading your library");
        /* The name only arrived through the setup flow, so starting with a
           stored sign-in left the interface with nothing to call the server. */
        std::string name;
        const config::Settings &settings = config::current();
        if (jellyfin::probe(settings.host, settings.port, name))
        {
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            g_shared.server_name = name;
        }
        load_home();
    }
    else
    {
        std::string name;
        const config::Settings &settings = config::current();
        if (jellyfin::probe(settings.host, settings.port, name))
        {
            {
                std::lock_guard<std::mutex> guard(g_shared.mutex);
                g_shared.server_name = name;
            }
            begin_quick_connect();
        }
        else
        {
            set_status(Screen::server_setup, "Could not reach your server",
                       "Check the address and make sure Jellyfin is running. For a local IP, use "
                       "the same network as your PS5.");
        }
    }

    for (;;)
    {
        Request request = Request::none;
        std::string text;
        std::string scope;
        std::string types;
        std::string secret;
        int port = 8096;
        {
            std::unique_lock<std::mutex> guard(g_shared.mutex);
            g_request_wake.wait(guard, [] { return g_shared.request != Request::none; });
            request = g_shared.request;
            text = g_shared.request_text;
            scope = g_shared.request_scope;
            types = g_shared.request_types;
            secret = g_shared.request_secret;
            port = g_shared.request_port;
            g_shared.request = Request::none;
            g_shared.request_secret.clear();
            g_shared.busy = true;
        }

        switch (request)
        {
        case Request::none:
            break;

        case Request::probe_server:
        {
            std::string name;
            if (jellyfin::probe(text, port, name))
            {
                config::Settings &settings = config::current();
                if (server_address::origin(settings.host, settings.port) !=
                    server_address::origin(text, port))
                {
                    settings.token.clear();
                    settings.user_id.clear();
                }
                settings.host = text;
                settings.port = port;
                config::save();
                {
                    std::lock_guard<std::mutex> guard(g_shared.mutex);
                    g_shared.server_name = name;
                }
                begin_quick_connect();
            }
            else
            {
                set_status(Screen::server_setup, "Could not reach your server",
                           "Check the address and make sure Jellyfin is running. For a local IP, "
                           "use the same network as your PS5.");
            }
            break;
        }

        case Request::quick_connect:
            begin_quick_connect();
            break;

        case Request::password_login:
            set_status(Screen::sign_in, "Signing in");
            if (jellyfin::authenticate(text, secret))
            {
                set_status(Screen::connecting, "Loading your library");
                load_home();
            }
            else
            {
                set_status(Screen::sign_in, "Sign in", jellyfin::last_error());
            }
            /* Overwrite rather than just release the buffer. */
            std::fill(secret.begin(), secret.end(), '\0');
            break;

        case Request::reload_home:
            load_home();
            break;

        case Request::refresh_progress:
            refresh_progress_rows();
            break;

        case Request::probe_delivery:
        {
            jellyfin::PlaybackRequest request;
            std::string key;
            {
                std::lock_guard<std::mutex> guard(g_shared.mutex);
                request = g_shared.delivery_request;
                key = g_shared.delivery_key;
            }
            const jellyfin::PlaybackPlan plan =
                jellyfin::playback_plan(player::with_console_options(request));
            std::vector<details::Line> lines = delivery::describe(plan, request.subtitle_index);
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            /* A different title opened meanwhile: this answer is not for it. */
            if (key == g_shared.delivery_key)
            {
                g_shared.delivery_lines = std::move(lines);
                g_shared.delivery_ready = true;
            }
            break;
        }

        case Request::open_library:
        {
            int total = 0;
            std::vector<jellyfin::Item> items = jellyfin::items_in(text, 0, 120, &total);
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            /* The viewer may have crossed two more sidebar categories while
               this request was on the wire. Never publish yesterday's answer
               into today's grid. */
            if (g_shared.screen == Screen::grid && g_shared.grid_library_id == text)
            {
                g_shared.grid_items = std::move(items);
                ++g_shared.grid_epoch;
                g_shared.grid_total = total;
                g_shared.grid_loading = false;
            }
            break;
        }

        case Request::run_search:
        {
            std::vector<jellyfin::Item> items;
            std::string search_error;
            // Independent budgets keep episode matches from crowding out titles.
            for (const char *type : {"Movie", "Series", "Episode"})
            {
                if (!search_sections::includes(types, type))
                    continue;
                auto matches = jellyfin::search(text, std::string_view(type) == "Episode" ? 60 : 40,
                                                scope, type);
                if (!jellyfin::last_error().empty())
                    search_error = "Some matches could not load. Edit the search to retry.";
                for (auto &match : matches)
                    items.push_back(std::move(match));
            }
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            if (g_shared.search_target_term == text && g_shared.search_target_scope == scope &&
                g_shared.search_target_types == types)
            {
                g_shared.search_items = std::move(items);
                g_shared.search_error = search_error;
                ++g_shared.search_epoch;
                g_shared.search_term = text;
                g_shared.search_loading = false;
            }
            break;
        }

        case Request::open_detail:
        {
            jellyfin::Item item;
            std::vector<jellyfin::Item> episodes;
            jellyfin::Item next_up;
            /* Cursor prefetch may already have the expensive full Item. Reuse
               it and spend this request only on the lower rows. */
            if (const std::optional<jellyfin::Item> cached = cached_detail(text);
                cached.has_value())
                item = *cached;
            else if (jellyfin::details(text, item))
                remember_detail(item);
            if (!item.id.empty())
            {
                if (const std::optional<CachedDetailRows> cached = cached_detail_rows(text);
                    cached.has_value())
                {
                    episodes = cached->entries;
                    next_up = cached->next_up;
                }
                else
                {
                    fetch_detail_rows(item, episodes, next_up);
                    remember_detail_rows(text, episodes, next_up);
                }
                warm_detail_rows(item, episodes, next_up);
            }
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            if (g_shared.screen == Screen::detail && g_shared.detail_target_id == text)
            {
                g_shared.detail_item = std::move(item);
                g_shared.detail_episodes = std::move(episodes);
                g_shared.detail_next_up = std::move(next_up);
                ++g_shared.detail_epoch;
                g_shared.detail_loading = false;
            }
            break;
        }

        case Request::load_siblings:
        {
            const std::string current = scope;
            std::vector<jellyfin::Item> run = jellyfin::episodes(text, {});
            int at = -1;
            for (std::size_t i = 0; i < run.size(); ++i)
                if (run[i].id == current)
                    at = static_cast<int>(i);
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            g_shared.siblings = std::move(run);
            g_shared.sibling_index = at;
            break;
        }

        case Request::play_sibling:
        {
            jellyfin::Item next;
            const bool ok = jellyfin::details(text, next);
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            g_shared.sibling_loading = false;
            if (ok)
            {
                g_shared.pending_play = std::move(next);
                g_shared.pending_play_ready = true;
            }
            break;
        }

        case Request::mark_played:
            /* Marking something played moves it out of Continue Watching and
               pulls the next episode into Next Up. Nothing else on Home
               changes, so this no longer re-walks the libraries. */
            if (jellyfin::mark_played(text))
                refresh_progress_rows();
            break;

        case Request::sign_out:
            jellyfin::sign_out();
            {
                std::lock_guard<std::mutex> guard(g_shared.mutex);
                g_shared.rows.clear();
                ++g_shared.rows_epoch;
                g_shared.grid_items.clear();
                ++g_shared.grid_epoch;
            }
            begin_quick_connect();
            break;
        }

        {
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            g_shared.busy = false;
        }
    }
    return nullptr;
}

void submit(Request request, const std::string &text = {}, const std::string &secret = {},
            int port = 8096) noexcept
{
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        g_shared.request = request;
        g_shared.request_text = text;
        g_shared.request_secret = secret;
        g_shared.request_port = port;
    }
    g_request_wake.notify_one();
}

/* Loading the run of episodes needs both the show and the episode in it. */
void submit_siblings(const std::string &series_id, const std::string &item_id) noexcept
{
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        g_shared.request = Request::load_siblings;
        g_shared.request_text = series_id;
        g_shared.request_scope = item_id;
    }
    g_request_wake.notify_one();
}

/* A search carries where to look as well as what to look for. */
void submit_search(const std::string &term, const std::string &scope,
                   const std::string &types) noexcept
{
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        g_shared.request = Request::run_search;
        g_shared.request_text = term;
        g_shared.request_scope = scope;
        g_shared.request_types = types;
        g_shared.search_target_term = term;
        g_shared.search_target_scope = scope;
        g_shared.search_target_types = types;
        g_shared.search_loading = true;
    }
    g_request_wake.notify_one();
}

/* --------------------------------------------------------------- drawing */

/* The page's own colours, in one place, because three things now have to
   agree about them: the background, the band redrawn over a scrolling list,
   and the fade that softens its edge. */
constexpr gfx::Color kPageBottom = gfx::rgb(0x0d, 0x0d, 0x12);
constexpr gfx::Color kPageTop = gfx::rgb(0x24, 0x16, 0x34);
constexpr int kPageRamp = 460;

void draw_background() noexcept
{
    gfx::clear(kPageBottom);
    gfx::vertical_gradient(0, 0, gfx::kWidth, kPageRamp, kPageTop, kPageBottom);
}

/* What the background is at one row, so something drawn over a list can match
   it exactly rather than approximately. */
gfx::Color background_at(int y) noexcept
{
    if (y >= kPageRamp)
        return kPageBottom;
    const int t = (std::max(0, y) * 255) / kPageRamp;
    const auto channel = [t](unsigned shift)
    {
        const int from = static_cast<int>((kPageTop >> shift) & 0xffu);
        const int to = static_cast<int>((kPageBottom >> shift) & 0xffu);
        return static_cast<std::uint8_t>(from + (to - from) * t / 255);
    };
    return gfx::rgb(channel(16), channel(8), channel(0));
}

/* Fade the clipped edge of a scrolling region. */
void draw_scroll_fade(int y) noexcept
{
    gfx::vertical_gradient(0, y, gfx::kWidth, kGridFade, background_at(y),
                           gfx::with_alpha(background_at(y + kGridFade), 0));
}

/* The Jellyfin silhouette: three nested rounded triangles, drawn as arcs. */
void draw_mark(int x, int y, int size, gfx::Color color) noexcept
{
    gfx::rounded_rect(x, y, size, size, size / 3, color);
    const int inset = size / 5;
    gfx::rounded_rect(x + inset, y + inset + size / 12, size - inset * 2, size - inset * 2,
                      (size - inset * 2) / 3, gfx::rgb(0x0d, 0x0d, 0x12));
    const int core = size / 3;
    gfx::rounded_rect(x + (size - core) / 2, y + (size - core) / 2 + size / 14, core, core,
                      core / 3, color);
}

void draw_wordmark(int x, int y) noexcept
{
    /* The project's own mark, swept purple to blue as its artwork is. */
    icons::draw_gradient(icons::Icon::jellyfin, x - 4, y - 10, 58, gfx::rgb(0xaa, 0x5c, 0xc3),
                         gfx::rgb(0x00, 0xa4, 0xdc));
    text::draw(x + 64, y + 3, "SlopFin", 36, Weight::bold, gfx::palette::text);
}

void draw_sidebar(Screen screen, const std::vector<SidebarEntry> &entries,
                  const std::string &server_name) noexcept
{
    /* Dim the inactive side of the interface. */
    const bool lit = g_view.sidebar_focused;
    /* Keep the sidebar opaque over bright backdrops. */
    gfx::fill_rect(0, 0, kSidebarWidth, gfx::kHeight,
                   gfx::rgba(0x08, 0x08, 0x0c, lit ? 0xdc : 0xf0));
    draw_wordmark(kSafeX - 8, kSafeY + 16);

    const bool navigable =
        screen == Screen::home || screen == Screen::grid || screen == Screen::settings;
    if (!navigable)
        return;

    if (!server_name.empty())
    {
        text::draw(kSafeX, gfx::kHeight - 108, "Connected to", 18, Weight::regular,
                   gfx::rgb(0x6c, 0x6c, 0x78));
        text::draw_ellipsized(kSafeX, gfx::kHeight - 82, kSidebarWidth - kSafeX - 20, server_name,
                              22, Weight::medium, gfx::rgb(0x9a, 0x9a, 0xa6));
    }

    constexpr int kWashX = kSafeX - 26;
    constexpr int kStep = 92;
    /* The extra air a rule gets, on top of the row it already occupies. */
    constexpr int kGroupGap = 34;
    /* Visible label bounds used to space sidebar separators. */
    constexpr int kInkTop = 10;
    constexpr int kInkBottom = 18;

    constexpr int kListTop = 208;
    const int list_bottom = gfx::kHeight - 148;
    int selected_y = 232;
    int last_y = 232;
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        if (entries[i].gap_before)
            last_y += kGroupGap;
        if (static_cast<int>(i) == g_view.sidebar_index)
            selected_y = last_y;
        if (i + 1 < entries.size())
            last_y += kStep;
    }
    float target = g_view.sidebar_scroll.target();
    if (selected_y - 12 - target < kListTop)
        target = static_cast<float>(selected_y - 12 - kListTop);
    if (selected_y + 44 - target > list_bottom)
        target = static_cast<float>(selected_y + 44 - list_bottom);
    target = std::clamp(target, 0.0f, static_cast<float>(std::max(0, last_y + 44 - list_bottom)));
    g_view.sidebar_scroll.aim(target);
    const int scroll = static_cast<int>(g_view.sidebar_scroll.value());
    gfx::push_clip(0, kListTop, kSidebarWidth, list_bottom - kListTop);
    if (lit)
    {
        int focus_y = 232 - scroll;
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            if (entries[i].gap_before)
                focus_y += kGroupGap;
            if (static_cast<int>(i) == g_view.sidebar_index)
                ui::focus_pill(kWashX, focus_y - 12, kSidebarWidth - kWashX - 20, 56,
                               ui::FocusGroup::sidebar);
            focus_y += kStep;
        }
    }
    int y = 232 - scroll;
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        if (entries[i].gap_before)
        {
            /* Place separators midway between adjacent labels. */
            const int ink_above = y - kStep + kInkBottom;
            const int ink_below = y + kGroupGap + kInkTop;
            gfx::fill_rect(kWashX, (ink_above + ink_below) / 2, kSidebarWidth - kSafeX - 20, 1,
                           gfx::rgba(0xff, 0xff, 0xff, 0x14));
            y += kGroupGap;
        }
        const bool active = static_cast<int>(i) == g_view.sidebar_index;

        /* Brightness carries the state; nothing is drawn behind the words. */

        if (active)
        {
            if (!lit)
                gfx::rounded_rect(kWashX + 8, y + 4, 3, 24, 1, gfx::palette::accent_alt);
        }
        const gfx::Color label = active ? gfx::rgba(0xff, 0xff, 0xff, lit ? 0xff : 0xbe)
                                        : gfx::rgba(0xff, 0xff, 0xff, lit ? 0xb8 : 0x92);
        ui::control_label(kSafeX + 4, y - 12, kSidebarWidth - kSafeX - 20, 56, entries[i].label, 28,
                          Weight::medium, label);
        y += kStep;
    }
    gfx::pop_clip();
}

std::string card_subtitle(const jellyfin::Item &item) noexcept
{
    if (item.type == "Episode" && item.parent_index_number > 0)
        return "S" + std::to_string(item.parent_index_number) + " E" +
               std::to_string(item.index_number);
    if (item.production_year > 0)
        return std::to_string(item.production_year);
    return item.type;
}

/* Use a muted placeholder until artwork arrives. */
void draw_card_placeholder(int x, int y, int w, int h, const jellyfin::Item &item) noexcept
{
    gfx::rounded_rect(x, y, w, h, kCardRadius, gfx::rgba(0xff, 0xff, 0xff, 0x0b));
    gfx::stroke_rounded_rect(x, y, w, h, kCardRadius, 1, gfx::rgba(0xff, 0xff, 0xff, 0x14));
    const std::string initial = item.name.empty() ? "?" : item.name.substr(0, 1);
    const int size = std::max(28, h / 8);
    const int width = text::measure(initial, size, Weight::bold);
    text::draw(x + (w - width) / 2, y + (h - size) / 2 - size / 8, initial, size, Weight::bold,
               gfx::rgba(0xff, 0xff, 0xff, 0x26));
}

/* Draw artwork directly into the rounded card mask. */
/* Poster cards use series artwork for episodes; landscape cards prefer episode stills. */
enum class CardArt : std::uint8_t
{
    show,    /* the series poster for an episode: libraries and search */
    episode, /* the episode's own still, in a poster-shaped card */
    wide,    /* a landscape card: the still, or a film's backdrop */
};

struct ArtSource
{
    const std::string &id;
    const std::string &tag;
};

ArtSource card_artwork(const jellyfin::Item &item, CardArt kind) noexcept
{
    /* Prefer landscape artwork for landscape cards. */
    if (kind == CardArt::wide && item.type != "Episode" && !item.backdrop_tag.empty())
        return {item.id, item.backdrop_tag};
    if (item.type == "Episode" && !item.series_id.empty() && !item.series_primary_tag.empty())
    {
        /* Fall back to the series poster when an episode has no suitable artwork. */
        if (kind == CardArt::show || item.image_tag.empty())
            return {item.series_id, item.series_primary_tag};
    }
    return {item.id, item.image_tag};
}

/* Request artwork for nearby off-screen cards. */
void warm_card(const jellyfin::Item &item, CardArt art_kind, int height = kCardHeight) noexcept
{
    const ArtSource source = card_artwork(item, art_kind);
    images::prefetch(source.id, source.tag,
                     art_kind == CardArt::wide && source.tag == item.backdrop_tag
                         ? images::Kind::backdrop
                         : images::Kind::primary,
                     height + 120);
}

void draw_card(const jellyfin::Item &item, int x, int y, bool focused, float grow,
               CardArt art_kind = CardArt::show, int base_w = kCardWidth, int base_h = kCardHeight,
               float reveal = 1.0f, bool episode_context = false) noexcept
{
    /* Reveal cards by easing opacity and vertical offset. */
    const float opacity = smoothstep(0.0f, 0.6f, reveal);
    const auto reveal_alpha = [opacity](std::uint8_t a)
    { return static_cast<std::uint8_t>(static_cast<float>(a) * opacity); };
    const auto reveal_colour = [&reveal_alpha](gfx::Color c)
    { return gfx::with_alpha(c, reveal_alpha(static_cast<std::uint8_t>((c >> 24) & 0xff))); };
    if (g_reveal_sideways)
        x += static_cast<int>(std::lround((1.0f - ease_out(reveal)) * kRevealSlide));
    else
        y += static_cast<int>(std::lround((1.0f - ease_out(reveal)) * kRevealRise));
    /* The spring overshoots slightly; the geometry is allowed to follow it,
       because that tick past the target is what gives the focus its weight. */
    const float rise = focused ? std::clamp(grow, 0.0f, 1.15f) : 0.0f;
    const int lift = static_cast<int>(rise * 12.0f);
    const int expand = static_cast<int>(rise * 9.0f);
    const int cx = x - expand;
    const int cy = y - expand - lift;
    const int cw = base_w + expand * 2;
    const int ch = base_h + expand * 2;

    const ArtSource source = card_artwork(item, art_kind);
    const gfx::Bitmap *art = images::acquire(
        source.id, source.tag,
        art_kind == CardArt::wide && source.tag == item.backdrop_tag ? images::Kind::backdrop
                                                                     : images::Kind::primary,
        base_h + 120);
    const std::string key = source.id + "|p";
    if (g_first_drawn.insert(key).second)
        ++(art != nullptr ? g_first_ready : g_first_missing);
    if (art == nullptr)
        g_awaited.insert(key);
    const std::uint8_t fade = art != nullptr ? artwork_fade(key) : 0;
    if (fade < 255 && reveal >= 1.0f)
        draw_card_placeholder(cx, cy, cw, ch, item);
    if (art != nullptr)
        gfx::blit_cover(*art, cx, cy, cw, ch, kCardRadius, reveal_alpha(fade));

    if (item.played_percentage > 0.5)
    {
        /* Sat on the poster's bottom edge rather than floating above it, so it
           reads as part of the artwork instead of a bar laid over it. */
        const int inset = 12;
        const int track = cw - inset * 2;
        const int filled = std::max(3, static_cast<int>(track * item.played_percentage / 100.0));
        const int bar_y = cy + ch - 13;
        gfx::rounded_rect(cx + inset, bar_y, track, 4, 2,
                          reveal_colour(gfx::rgba(0x00, 0x00, 0x00, 0x88)));
        gfx::rounded_rect(cx + inset, bar_y, filled, 4, 2, reveal_colour(gfx::palette::accent));
    }

    /* One white ring, and nothing outside it. A tinted halo sat behind this
       for a while and read as a second outline around the poster rather than
       as light; the white ring already says which card has the focus. */
    if (focused)
        gfx::stroke_rounded_rect(cx - 3, cy - 3, cw + 6, ch + 6, kCardRadius + 3, 3,
                                 reveal_colour(gfx::palette::text));

    const int label_y = cy + ch + 16;
    text::draw_ellipsized(cx, label_y, cw, item.name, 23,
                          focused ? Weight::medium : Weight::regular,
                          reveal_colour(focused ? gfx::palette::text : gfx::rgb(0xb0, 0xb0, 0xba)));
    text::draw_ellipsized(cx, label_y + 31, cw,
                          episode_context && !item.series_name.empty()
                              ? item.series_name + " · " + card_subtitle(item)
                              : card_subtitle(item),
                          20, Weight::regular, reveal_colour(gfx::palette::text_faint));
}

/* Runtime, year and rating, joined only where a value actually exists. */
std::string hero_meta(const jellyfin::Item &item) noexcept
{
    std::string line;
    const auto add = [&line](const std::string &piece)
    {
        if (piece.empty())
            return;
        if (!line.empty())
            line += "   \u00b7   ";
        line += piece;
    };
    if (item.type == "Episode" && item.parent_index_number > 0)
        add("Season " + std::to_string(item.parent_index_number) + ", Episode " +
            std::to_string(item.index_number));
    if (item.production_year > 0)
        add(std::to_string(item.production_year));
    if (item.runtime_minutes > 0)
    {
        const int hours = item.runtime_minutes / 60;
        const int minutes = item.runtime_minutes % 60;
        add(hours > 0 ? std::to_string(hours) + "h " + std::to_string(minutes) + "m"
                      : std::to_string(minutes) + "m");
    }
    if (item.community_rating > 0.0)
    {
        const int tenths = static_cast<int>(item.community_rating * 10.0 + 0.5);
        add(std::to_string(tenths / 10) + "." + std::to_string(tenths % 10));
    }
    return line;
}

/* Detail strips use poster seasons and landscape episodes. */
struct StripShape
{
    bool seasons = false;
    int y = kSafeY + 60 + 450 + 80; /* under the detail hero, whose poster ends there */
    int card_w = 400;
    int card_h = kEpisodeStripHeight;
    int pitch = 428;
    int heading = 54; /* how far above the cards the heading sits */
};

StripShape strip_shape(const std::vector<jellyfin::Item> &entries) noexcept
{
    StripShape shape;
    shape.seasons = !entries.empty() && entries.front().type == "Season";
    if (shape.seasons)
    {
        /* Portrait cards are taller, so they start lower and their heading
           sits closer, clear of the poster above rather than across it. */
        shape.y = kSafeY + 60 + 450 + 72;
        shape.card_w = 210;
        shape.card_h = kSeasonCardHeight;
        shape.pitch = shape.card_w + 26;
        shape.heading = 40;
    }
    return shape;
}

/* Cache the composited backdrop to avoid repeated full-screen blending. */
struct Wash
{
    std::uint32_t *pixels = nullptr;
    std::size_t words = 0;
    std::string key; /* what it was composed from */
    int shape = -1;  /* which layout composed it */
    bool ready = false;
};
Wash g_wash;
/* Counts compositions, so the trace can say whether the cache is being hit. */
unsigned g_wash_composed = 0;

/* Reuse the cached background until its item or composition key changes. */
bool wash_begin(const std::string &key, int shape, bool draw_base = true) noexcept
{
    const std::size_t words = gfx::surface_words();
    if (g_wash.words != words)
    {
        if (g_wash.pixels != nullptr)
            slopfin::bigalloc::release(g_wash.pixels);
        g_wash.pixels = static_cast<std::uint32_t *>(
            slopfin::bigalloc::allocate(words * sizeof(std::uint32_t)));
        g_wash.words = g_wash.pixels != nullptr ? words : 0;
        g_wash.ready = false;
    }
    if (g_wash.pixels == nullptr)
        return true; /* no cache: draw straight to the frame, as before */

    if (g_wash.ready && g_wash.shape == shape && g_wash.key == key)
    {
        if (shape == 0)
            gfx::blit_surface_height(g_wash.pixels, 720);
        else
            gfx::blit_surface(g_wash.pixels);
        return false;
    }
    g_wash.key = key;
    g_wash.shape = shape;
    g_wash.ready = false;
    /* Hero art covers only the upper part of Home, so it needs the page base
       underneath. Detail backdrops cover the whole surface and can skip that
       otherwise wasted clear+gradient pass. */
    (void)gfx::push_target(g_wash.pixels);
    if (draw_base)
        draw_background();
    return true;
}

/* Finishes a composition begun by wash_begin and puts it on the frame. */
void wash_end() noexcept
{
    if (g_wash.pixels == nullptr || g_wash.ready)
        return;
    gfx::pop_target();
    g_wash.ready = true;
    ++g_wash_composed;
    if (g_wash.shape == 0)
        gfx::blit_surface_height(g_wash.pixels, 720);
    else
        gfx::blit_surface(g_wash.pixels);
}

/* Render the backdrop and metadata of the focused Home item. */
/* The picture behind the hero: the item's own backdrop, else its parent's.
   Asked for at the height it is drawn at, so nothing is invented on the way to
   the screen. Safe from any thread. */
const gfx::Bitmap *hero_art(const jellyfin::Item &item) noexcept
{
    const gfx::Bitmap *art = nullptr;
    if (!item.backdrop_tag.empty())
        art = images::acquire(item.id, item.backdrop_tag, images::Kind::backdrop, 720);
    if (art == nullptr && !item.parent_backdrop_tag.empty())
    {
        const std::string owner =
            item.parent_backdrop_id.empty() ? item.series_id : item.parent_backdrop_id;
        if (!owner.empty())
            art = images::acquire(owner, item.parent_backdrop_tag, images::Kind::backdrop, 720);
    }
    return art;
}

void draw_hero(const jellyfin::Item &item, float reveal = 1.0f) noexcept
{
    const gfx::Bitmap *art = hero_art(item);
    if (art != nullptr)
    {
        /* frame() has already painted the page base. The cached hero owns only
           these top 720 logical pixels, so composing a second full background
           into its offscreen surface is pure memory traffic. */
        if (wash_begin(item.id + "+", 0, false))
        {
            gfx::blit_cover(*art, 0, 0, gfx::kWidth, 720, 0, 255);
            gfx::media_scrim(720, gfx::rgb(0x0d, 0x0d, 0x12), 0x8c, 240, 1420, 0xf0);
        }
        wash_end();
    }

    /* Arriving on Home, the words slide in from the left as the cards come
       in from the right, and fade with them. */
    const float opacity = smoothstep(0.0f, 0.6f, reveal);
    const auto faded = [opacity](gfx::Color c)
    {
        return gfx::with_alpha(
            c, static_cast<std::uint8_t>(static_cast<float>((c >> 24) & 0xffu) * opacity));
    };
    const int x = kContentX - static_cast<int>(std::lround((1.0f - ease_out(reveal)) * 32.0f));
    int y = kSafeY + 96;
    if (item.type == "Episode" && !item.series_name.empty())
    {
        text::draw_ellipsized(x, y, 1000, item.series_name, 25, Weight::medium,
                              faded(gfx::palette::accent_alt));
        y += 40;
    }
    text::draw_ellipsized(x, y, 1160, item.name, 58, Weight::bold, faded(gfx::palette::text));
    y += 80;

    const std::string meta = hero_meta(item);
    if (!meta.empty())
    {
        text::draw(x, y, meta, 24, Weight::regular, faded(gfx::rgb(0xb4, 0xb4, 0xbe)));
        y += 44;
    }
    if (!item.overview.empty())
        text::draw_wrapped(x, y, 940, 3, 36, item.overview, 24, Weight::regular,
                           faded(gfx::rgb(0xcf, 0xcf, 0xd6)));
}

void draw_search_affordance() noexcept;
/* The top of the profile badge and the search chip, centred on a page title. */
constexpr int kTopBarY = kSafeY + 90;

/* Accumulate variable row heights to locate the selected row. */
/* The rule for a resume row: episodes are wide, everything else a poster. */
bool card_is_wide(const jellyfin::Item &item) noexcept
{
    return item.type == "Episode";
}

int card_w_for(const jellyfin::Item &item, const Row &row) noexcept
{
    return row.wide && card_is_wide(item) ? kWideWidth : kCardWidth;
}

int card_h_for(const jellyfin::Item &item, const Row &row) noexcept
{
    return row.wide && card_is_wide(item) ? kWideHeight : kCardHeight;
}

/* The tallest card in the row, which is the floor every card stands on. A row
   of nothing but episodes stays as short as it always was. */
int row_card_h(const Row &row) noexcept
{
    int tallest = row.wide ? kWideHeight : kCardHeight;
    for (const jellyfin::Item &item : row.items)
        tallest = std::max(tallest, card_h_for(item, row));
    return tallest;
}

int row_card_w(const Row &row) noexcept
{
    return row.wide ? kWideWidth : kCardWidth;
}

/* Which picture a card in a row carries: a poster card gets the poster, not a
   backdrop cropped down to one. */
CardArt card_art_for(const jellyfin::Item &item, const Row &row) noexcept;

/* Wait for initial visible artwork, with a deadline so missing images cannot block startup. */
void await_first_screen(const std::vector<Row> &rows) noexcept
{
    if (rows.empty())
        return;
    static constexpr std::size_t kTopRows = 2;
    static constexpr std::size_t kCardsAcross = 6;
    constexpr std::uint64_t kLongestUs = 2500000;
    constexpr std::uint32_t kPollUs = 50000;

    int wanted = 0;
    const auto count_ready = [&rows, &wanted]()
    {
        int ready = 0;
        wanted = 0;
        const jellyfin::Item &hero = rows.front().items.front();
        if (!hero.backdrop_tag.empty() || !hero.parent_backdrop_tag.empty())
        {
            ++wanted;
            ready += hero_art(hero) != nullptr ? 1 : 0;
        }
        for (std::size_t r = 0; r < std::min(rows.size(), kTopRows); ++r)
        {
            for (std::size_t c = 0; c < std::min(rows[r].items.size(), kCardsAcross); ++c)
            {
                const jellyfin::Item &item = rows[r].items[c];
                const CardArt art = card_art_for(item, rows[r]);
                const ArtSource source = card_artwork(item, art);
                if (source.tag.empty())
                    continue; /* nothing to wait for: it draws a placeholder */
                ++wanted;
                const images::Kind kind = art == CardArt::wide && source.tag == item.backdrop_tag
                                              ? images::Kind::backdrop
                                              : images::Kind::primary;
                ready += images::acquire(source.id, source.tag, kind,
                                         card_h_for(item, rows[r]) + 120) != nullptr
                             ? 1
                             : 0;
            }
        }
        return ready;
    };

    const std::uint64_t began = slopfin::app::now_us();
    int ready = count_ready();
    while (ready < wanted && slopfin::app::now_us() - began < kLongestUs)
    {
        (void)sceKernelUsleep(kPollUs);
        ready = count_ready();
    }
    trace::mark("home: loading screen held " +
                std::to_string((slopfin::app::now_us() - began) / 1000u) + " ms, " +
                std::to_string(ready) + " of " + std::to_string(wanted) + " pictures ready");
}

CardArt card_art_for(const jellyfin::Item &item, const Row &row) noexcept
{
    if (!row.wide)
        return CardArt::episode;
    return card_is_wide(item) ? CardArt::wide : CardArt::episode;
}

/* How far card `index` sits from the left edge of its row. */
int card_offset(const Row &row, std::size_t index) noexcept
{
    int at = 0;
    const std::size_t last = std::min(index, row.items.size());
    for (std::size_t i = 0; i < last; ++i)
        at += card_w_for(row.items[i], row) + kCardGap;
    return at;
}

int row_height(const Row &row) noexcept
{
    return row_card_h(row) + kRowTitleHeight + 108;
}

int row_top(const std::vector<Row> &rows, std::size_t index) noexcept
{
    int at = kRowsTop;
    for (std::size_t i = 0; i < index && i < rows.size(); ++i)
        at += row_height(rows[i]);
    return at;
}

void draw_home(const std::vector<Row> &rows) noexcept
{
    if (rows.empty())
        return;
    const Row &focused_row = rows[static_cast<std::size_t>(g_view.row_index)];
    if (!focused_row.items.empty())
        draw_hero(focused_row.items[static_cast<std::size_t>(
                      g_view.column[static_cast<std::size_t>(g_view.row_index)])],
                  card_reveal(0, 0));

    draw_search_affordance();

    /* Start the clip at the row titles: any row scrolled above this is gone,
       labels included, rather than leaving orphaned text behind. */
    gfx::push_clip(kContentX - 24, kRowsTop - 16, kContentWidth + 48, gfx::kHeight - kRowsTop + 16);

    std::vector<int> first_visible(rows.size(), -1);
    int visible_rows = 0;
    int widest = 0;
    for (std::size_t r = 0; r < rows.size(); ++r)
    {
        const int row_y = row_top(rows, r) - static_cast<int>(g_view.scroll.value());
        if (row_y > gfx::kHeight || row_y + row_height(rows[r]) < 120)
            continue;
        int at = 0;
        int count = 0;
        for (std::size_t c = 0; c < rows[r].items.size(); ++c)
        {
            const int card_w = card_w_for(rows[r].items[c], rows[r]);
            const int x = kContentX + at - static_cast<int>(g_view.row_offset[r].value());
            at += card_w + kCardGap;
            if (x > kContentRight || x + card_w < kContentX - 48)
                continue;
            if (first_visible[r] < 0)
                first_visible[r] = static_cast<int>(c);
            ++count;
        }
        widest = std::max(widest, count);
        ++visible_rows;
    }
    g_reveal_span = std::max(1, (widest - 1) + (visible_rows - 1));
    int visible_row = 0;
    g_reveal_sideways = true;

    for (std::size_t r = 0; r < rows.size(); ++r)
    {
        const int row_y = row_top(rows, r) - static_cast<int>(g_view.scroll.value());
        const int card_h = row_card_h(rows[r]);
        if (row_y > gfx::kHeight || row_y + row_height(rows[r]) < 120)
            continue;
        const int row_order = visible_row++;

        const bool active = static_cast<int>(r) == g_view.row_index;
        /* A row's title arrives with its first card. */
        const float title_reveal = card_reveal(0, row_order);
        const gfx::Color title_colour = active ? gfx::palette::text : gfx::rgb(0x96, 0x96, 0xa0);
        text::draw(kContentX + static_cast<int>(std::lround((1.0f - ease_out(title_reveal)) *
                                                            kRevealSlide * 0.5f)),
                   row_y, rows[r].title, 30, Weight::medium,
                   gfx::with_alpha(
                       title_colour,
                       static_cast<std::uint8_t>(255.0f * smoothstep(0.0f, 0.6f, title_reveal))));

        const int cards_y = row_y + kRowTitleHeight;
        gfx::push_clip(kContentX - 24, cards_y - 26, kContentWidth + 48, card_h + 100);
        int at = 0;
        for (std::size_t c = 0; c < rows[r].items.size(); ++c)
        {
            const int card_w = card_w_for(rows[r].items[c], rows[r]);
            const int item_h = card_h_for(rows[r].items[c], rows[r]);
            const CardArt art = card_art_for(rows[r].items[c], rows[r]);
            const int x = kContentX + at - static_cast<int>(g_view.row_offset[r].value());
            at += card_w + kCardGap;
            if (x > kContentRight || x + card_w < kContentX - 48)
            {
                /* Three cards past each edge: a row is stepped one card at a
                   time, so that is several presses of warning. */
                const int reach = (kWideWidth + kCardGap) * 3;
                if (x < kContentRight + reach && x + card_w > kContentX - reach)
                    warm_card(rows[r].items[c], art, item_h);
                continue;
            }
            const bool focused =
                active && !g_view.sidebar_focused && static_cast<int>(c) == g_view.column[r];
            draw_card(rows[r].items[c], x, cards_y + card_h - item_h, focused, g_view.focus.value(),
                      art, card_w, item_h,
                      card_reveal(static_cast<int>(c) - std::max(0, first_visible[r]), row_order));
        }
        gfx::pop_clip();
    }
    g_reveal_sideways = false;
    gfx::pop_clip();
}

/* How many rows a list of items occupies, and how far it can be scrolled. */
int grid_rows(std::size_t count) noexcept
{
    return static_cast<int>((count + kGridColumns - 1) / kGridColumns);
}

float grid_scroll_limit(std::size_t count) noexcept
{
    const int content = grid_rows(count) * kGridRowHeight;
    return static_cast<float>(std::max(0, content - kGridViewHeight));
}

/* Show the scroll position for long category grids. */
struct ScrollHint
{
    motion::Spring alpha;
    int quiet_frames = 0;
};
ScrollHint g_scroll_hint;

void note_scrolling() noexcept
{
    g_scroll_hint.quiet_frames = 0;
    g_scroll_hint.alpha.precision(0.004f);
    g_scroll_hint.alpha.aim(1.0f);
}

void advance_scroll_hint() noexcept
{
    /* About three quarters of a second of stillness before it starts to go. */
    constexpr int kHold = 45;
    if (++g_scroll_hint.quiet_frames > kHold)
        g_scroll_hint.alpha.aim(0.0f);
    g_scroll_hint.alpha.step(motion::kFade, frame_seconds());
}

void draw_scroll_bar(float scroll, float limit) noexcept
{
    const float shown = g_scroll_hint.alpha.value();
    if (shown <= 0.004f || limit <= 0.5f)
        return;

    const float content = limit + static_cast<float>(kGridViewHeight);
    const float fraction = std::clamp(static_cast<float>(kGridViewHeight) / content, 0.06f, 1.0f);
    const int thumb = std::max(40, static_cast<int>(kBarHeight * fraction));
    const float travel = std::clamp(scroll / limit, 0.0f, 1.0f);
    const int top = kBarTop + static_cast<int>((kBarHeight - thumb) * travel);

    const auto track_alpha = static_cast<std::uint8_t>(shown * 0x24);
    const auto thumb_alpha = static_cast<std::uint8_t>(shown * 0xcc);
    gfx::rounded_rect(kBarX, kBarTop, kBarWidth, kBarHeight, kBarWidth / 2,
                      gfx::rgba(0xff, 0xff, 0xff, track_alpha));
    gfx::rounded_rect(kBarX, top, kBarWidth, thumb, kBarWidth / 2,
                      gfx::rgba(0xff, 0xff, 0xff, thumb_alpha));
}

using ui::ButtonHint;
using ui::draw_button_chip;
using ui::draw_button_hints;
using ui::hint_colour;

/* ---------------------------------------------------------------- search */

/*
 * Which section the search belongs to, and therefore what it searches. Home
 * searches everything; a library searches that library, and asks only for the
 * kind of thing that library holds, because a list of episodes is not what
 * "search the shows" means.
 */
struct SearchScope
{
    std::string label;      /* shown in the bar */
    std::string library_id; /* empty searches the whole account */
    std::string types;      /* empty means films, series and episodes */
};

SearchScope search_scope(Screen screen, const std::vector<SidebarEntry> &sidebar,
                         const std::vector<jellyfin::Library> &libraries) noexcept
{
    if (screen != Screen::grid || g_view.active_section < 0 ||
        g_view.active_section >= static_cast<int>(sidebar.size()))
        return {"Everything", {}, {}};

    const SidebarEntry &entry = sidebar[static_cast<std::size_t>(g_view.active_section)];
    if (entry.kind != SidebarEntry::Kind::library)
        return {"Everything", {}, {}};

    std::string collection;
    for (const jellyfin::Library &library : libraries)
        if (library.id == entry.library_id)
            collection = library.collection_type;

    if (collection == "movies")
        return {entry.label, entry.library_id, "Movie"};
    if (collection == "tvshows")
        return {entry.label, entry.library_id, "Series,Episode"};
    return {entry.label, entry.library_id, {}};
}

/* Opening the search is reached from the screens above the handlers that
   define it, so it is named here. */
void open_search() noexcept;

/* Results remain visible while the system keyboard is open. */
int search_results_top() noexcept
{
    return kSearchBarY + kSearchBarH + 54;
}

void draw_search_bar(const SearchScope &scope, bool loading, std::size_t results) noexcept
{
    const int x = kSearchBarX;
    const int y = kSearchBarY;
    const int w = kSearchBarW;
    const int h = kSearchBarH;
    const bool active = ime::busy();

    /* The bar stays legible behind the native keyboard. */
    if (!active)
        gfx::drop_shadow(x, y, w, h, h / 2, 18, 0x70);
    gfx::rounded_rect(x, y, w, h, h / 2, gfx::rgba(0xff, 0xff, 0xff, active ? 0x16 : 0x10));
    gfx::stroke_rounded_rect(x, y, w, h, h / 2, 2,
                             active ? gfx::rgba(0xff, 0xff, 0xff, 0x8c)
                                    : gfx::rgba(0xff, 0xff, 0xff, 0x24));

    icons::draw(icons::Icon::search, x + 26, y + (h - 30) / 2, 30,
                gfx::rgba(0xff, 0xff, 0xff, active ? 0xdd : 0x88));

    const std::string &shown = g_view.search.committed;
    const int text_x = x + 70;
    const int text_w = w - 70 - 210;
    ui::control_label(text_x, y, text_w, h, shown.empty() ? "Search " + scope.label : shown, 28,
                      Weight::medium, shown.empty() ? gfx::palette::text_dim : gfx::palette::text);

    /* What the search is bounded to, on the right of the bar. */
    const int scope_w = text::measure(scope.label, 20, Weight::medium) + 34;
    const int scope_x = x + w - scope_w - 18;
    gfx::rounded_rect(scope_x, y + 19, scope_w, 34, 17, gfx::rgba(0xff, 0xff, 0xff, 0x14));
    text::draw(scope_x + 17, text::centered_y(y + 19, 34, scope.label, 20, Weight::medium),
               scope.label, 20, Weight::medium, gfx::rgba(0xff, 0xff, 0xff, 0xb4));

    /* Triangle edits the existing search without clearing its results. */
    const int chip_w = draw_button_chip(x + w + 26, y + (h - 44) / 2, icons::Icon::ps_triangle,
                                        active ? "Typing" : "Edit", active);

    /* A one-line answer beside it, rather than a screen that changes. */
    std::string note;
    if (loading)
        note = "Searching";
    else if (!g_view.search.committed.empty())
        note = results == 0 ? "No matches" : std::to_string(results) + " results";
    if (!note.empty())
        text::draw(x + w + 26 + chip_w + 22, y + 24, note, 22, Weight::regular,
                   gfx::palette::text_dim);
}

void draw_search_results(const std::vector<jellyfin::Item> &items,
                         const std::vector<search_sections::Section> &sections, bool loading,
                         const std::string &error) noexcept
{
    const int top = search_results_top();
    draw_button_hints(kContentX, gfx::kHeight - 48,
                      {{icons::Icon::ps_cross, "Open"},
                       {icons::Icon::ps_triangle, "Edit search"},
                       {icons::Icon::ps_circle, "Close"}});

    if (g_view.search.committed.empty())
    {
        text::draw(kContentX, top + 20,
                   g_view.search.keyboard_notice.empty()
                       ? "Search movies, series and episodes with the PlayStation keyboard."
                       : g_view.search.keyboard_notice,
                   25, Weight::regular, gfx::palette::text_dim);
        return;
    }
    if (items.empty())
    {
        text::draw(kContentX, top + 20,
                   !error.empty() ? error
                   : loading      ? "Finding matches..."
                                  : "No matches. Try another title or a shorter search.",
                   25, Weight::regular, gfx::palette::text_dim);
        return;
    }
    const auto [selected_row, selected_column] =
        search_sections::locate(sections, g_view.search.result_index);
    (void)selected_column;
    const int scroll = static_cast<int>(g_view.search.scroll.value());
    gfx::push_clip(kContentX - 24, top - 12, kContentWidth + 48, gfx::kHeight - top - 80);
    for (int row = 0; row < static_cast<int>(sections.size()); ++row)
    {
        const auto &section = sections[static_cast<std::size_t>(row)];
        const int y = top + section.top - scroll;
        if (y + section.extent() < top - 24 || y > gfx::kHeight)
            continue;
        text::draw(kContentX, y, section.label, 28, Weight::medium, gfx::palette::text);
        text::draw(kContentX + text::measure(section.label, 28, Weight::medium) + 18, y + 6,
                   std::to_string(section.indices.size()), 20, Weight::regular,
                   gfx::palette::text_dim);
        const int offset =
            static_cast<int>(g_view.search.rail[static_cast<std::size_t>(section.kind)].value());
        for (int column = 0; column < static_cast<int>(section.indices.size()); ++column)
        {
            const int index = section.indices[static_cast<std::size_t>(column)];
            const auto &item = items[static_cast<std::size_t>(index)];
            const int x = kContentX + column * (section.width() + 28) - offset;
            if (x + section.width() < kContentX - 24 || x > kContentRight + 24)
                continue;
            const bool focused =
                row == selected_row && index == g_view.search.result_index && !ime::busy();
            draw_card(item, x, y + 64, focused, g_view.focus.value(),
                      section.kind == 2 ? CardArt::episode : CardArt::show, section.width(),
                      section.height(), 1.0f, true);
        }
    }
    gfx::pop_clip();
    if (scroll > 0)
        draw_scroll_fade(top - 12);
    if (!error.empty())
        text::draw(kContentX + 660, gfx::kHeight - 48, error, 19, Weight::regular,
                   gfx::palette::danger);
}

/* Triangle opens search from the page header. */
void draw_search_affordance() noexcept
{
    if (g_view.search.open)
        return;
    /* Beside the profile badge, which holds the corner, at its size and on the
       line of the page title. */
    const int width = ui::top_chip_width("Search");
    (void)ui::draw_top_chip(kContentRight - account::badge_width() - 18 - width, kTopBarY,
                            icons::Icon::ps_triangle, "Search");
}

/* Fade outgoing cards beneath the new page for twelve frames. */
void draw_leaving_cards() noexcept
{
    if (g_view.leaving_frame < 0)
        return;
    constexpr float kLeaveFrames = 12.0f;
    const float t =
        static_cast<float>(static_cast<long>(g_view.frames) - g_view.leaving_frame) / kLeaveFrames;
    if (t >= 1.0f)
    {
        g_view.leaving_items.clear();
        g_view.leaving_frame = -1;
        return;
    }
    const float progress = 0.6f * (1.0f - t);
    const int scroll = static_cast<int>(g_view.leaving_scroll);
    const int step_x = kCardWidth + kCardGap;
    gfx::push_clip(kContentX - 24, kGridClipTop, kContentWidth + 48, gfx::kHeight - kGridClipTop);
    for (std::size_t i = 0; i < g_view.leaving_items.size(); ++i)
    {
        const int column = static_cast<int>(i) % kGridColumns;
        const int row = static_cast<int>(i) / kGridColumns;
        const int x = kContentX + column * step_x;
        const int y = kGridTop + row * kGridRowHeight - scroll;
        if (y > gfx::kHeight || y + kGridRowHeight < kGridClipTop - 60)
            continue;
        draw_card(g_view.leaving_items[i], x, y, false, 0.0f, CardArt::show, kCardWidth,
                  kCardHeight, progress);
    }
    gfx::pop_clip();
}

void draw_grid(const std::vector<jellyfin::Item> &items, const std::string &title, bool loading,
               int total) noexcept
{
    text::draw(kContentX, kSafeY + 96, title, 38, Weight::bold, gfx::palette::text);
    /*
     * How much is in here, beside the name and set well below it in weight and
     * brightness. A count is worth knowing and is never the reason anyone came
     * to the screen, so it sits on the title's baseline rather than taking a
     * line of its own.
     */
    if (total > 0)
        text::draw(kContentX + text::measure(title, 38, Weight::bold) + 18, kSafeY + 106,
                   std::to_string(total), 22, Weight::regular, gfx::rgba(0xff, 0xff, 0xff, 0x66));
    draw_search_affordance();
    draw_leaving_cards();
    /*
     * On a LAN the answer comes back in a fraction of a second, and flashing
     * "Loading" for that long read as a stutter between the old cards and the
     * new. Only say it once the wait is long enough to notice. The clock
     * starts over whenever there is nothing being waited for.
     */
    static long loading_since = -1;
    if (!loading || !items.empty())
        loading_since = -1;
    else if (loading_since < 0)
        loading_since = static_cast<long>(g_view.frames);
    if (items.empty())
    {
        const bool slow = loading && static_cast<long>(g_view.frames) - loading_since > 24;
        if (slow || !loading)
            text::draw(kContentX, kGridTop, loading ? "Loading" : "Nothing here", 26,
                       Weight::regular, gfx::palette::text_dim);
        return;
    }

    const int scroll = static_cast<int>(g_view.grid_scroll.value());
    const int step_x = kCardWidth + kCardGap;
    const int first_row = std::max(0, scroll / kGridRowHeight);
    const int rows_on_screen = (gfx::kHeight - kGridTop) / kGridRowHeight + 1;
    const int rows_here = std::min(rows_on_screen, grid_rows(items.size()) - first_row);
    const int columns_here = std::min(static_cast<int>(items.size()), kGridColumns);
    g_reveal_span = std::max(1, (columns_here - 1) + (rows_here - 1));
    gfx::push_clip(kContentX - 24, kGridClipTop, kContentWidth + 48, gfx::kHeight - kGridClipTop);
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        const int column = static_cast<int>(i) % kGridColumns;
        const int row = static_cast<int>(i) / kGridColumns;
        const int x = kContentX + column * step_x;
        const int y = kGridTop + row * kGridRowHeight - scroll;
        if (y > gfx::kHeight || y + kGridRowHeight < kGridClipTop - 60)
        {
            /* Two rows either side, which at the speed the stick scrolls is
               about a second of warning. */
            if (y < gfx::kHeight + kGridRowHeight * 2 &&
                y + kGridRowHeight > kGridClipTop - kGridRowHeight * 2)
                warm_card(items[i], CardArt::show);
            continue;
        }
        const bool focused = !g_view.sidebar_focused && static_cast<int>(i) == g_view.grid_index;
        draw_card(items[i], x, y, focused, g_view.focus.value(), CardArt::show, kCardWidth,
                  kCardHeight, card_reveal(column, row - first_row));
    }
    gfx::pop_clip();

    /* Only once something is actually off the top: a fade over an empty
       header is a smear for no reason. */
    if (scroll > 0)
        draw_scroll_fade(kGridClipTop);
    draw_scroll_bar(g_view.grid_scroll.value(), grid_scroll_limit(items.size()));
}

/* A framed panel used by every full-screen prompt. */
using ui::panel;

void draw_option(int x, int y, int w, const std::string &label, const std::string &hint,
                 bool focused) noexcept
{
    gfx::rounded_rect(x, y, w, 76, 12, gfx::palette::control);
    if (focused)
        ui::focus_pill(x, y, w, 76);
    else
        gfx::stroke_rounded_rect(x, y, w, 76, 12, 1, gfx::palette::control_border);
    text::draw(x + 24, y + 12, label, 26, Weight::medium, gfx::palette::text);
    if (!hint.empty())
        text::draw_ellipsized(x + 24, y + 44, w - 48, hint, 19, Weight::regular,
                              focused ? gfx::rgba(0xff, 0xff, 0xff, 0xcc) : gfx::palette::text_dim);
}

void prepare_server_draft() noexcept
{
    if (g_view.server_draft_initialized)
        return;
    const auto &settings = config::current();
    g_view.server_draft = settings.host.empty()
                              ? ""
                              : server_address::Address{settings.host, settings.port, {}}.display();
    g_view.server_draft_initialized = true;
}

void draw_server_setup(const std::string &detail) noexcept
{
    prepare_server_draft();
    const int x = 480, y = 230, w = 960, h = 600;
    panel(x, y, w, h);
    text::draw(x + 48, y + 40, "Connect your Jellyfin", 40, Weight::bold, gfx::palette::text);
    text::draw_wrapped(
        x + 48, y + 106, w - 96, 2, 32,
        "Enter the address you use to open Jellyfin. We'll remember it for next time.", 23,
        Weight::regular, gfx::palette::text_dim);
    text::draw(x + 48, y + 186, "Server address", 23, Weight::medium, gfx::palette::text_dim);
    const int field_y = y + 226;
    // A text field, visually distinct from the single Continue action.
    gfx::rounded_rect(x + 48, field_y, w - 96, 80, 10, gfx::palette::background);
    if (g_view.setup_index == 0)
        ui::focus_pill(x + 48, field_y, w - 96, 80);
    text::draw_ellipsized(
        x + 70,
        text::centered_y(field_y, 80,
                         g_view.server_draft.empty() ? "media.example.com or 192.168.1.20"
                                                     : g_view.server_draft,
                         27, Weight::regular),
        w - 140,
        g_view.server_draft.empty() ? "media.example.com or 192.168.1.20" : g_view.server_draft, 27,
        Weight::regular, g_view.server_draft.empty() ? gfx::palette::text_dim : gfx::palette::text);
    text::draw(x + 48, y + 326, "No need to type http:// or https://.", 20, Weight::regular,
               gfx::palette::text_dim);
    const int button_x = x + 48, button_y = y + 388;
    gfx::rounded_rect(button_x, button_y, 240, 68, 14, gfx::palette::control);
    if (g_view.setup_index == 1)
        ui::focus_pill(button_x, button_y, 240, 68);
    text::draw(button_x + (240 - text::measure("Continue", 26, Weight::medium)) / 2,
               text::centered_y(button_y, 68, "Continue", 26, Weight::medium), "Continue", 26,
               Weight::medium, gfx::palette::text);
    if (!detail.empty())
        text::draw_wrapped(x + 48, y + 490, w - 96, 3, 26, detail, 20, Weight::regular,
                           gfx::palette::danger);
}

void draw_sign_in(const std::string &code, const std::string &status, const std::string &detail,
                  const std::string &server_name) noexcept
{
    const int x = 520;
    const int y = 200;
    const int w = 880;
    const int h = 640;
    panel(x, y, w, h);

    text::draw(x + 48, y + 40, "Sign in", 40, Weight::bold, gfx::palette::text);
    if (!server_name.empty())
        text::draw_ellipsized(x + 48, y + 96, w - 96, server_name, 23, Weight::regular,
                              gfx::palette::text_dim);

    if (g_view.credential_form)
    {
        text::draw(x + 48, y + 140, "Your Jellyfin account", 28, Weight::medium,
                   gfx::palette::text);
        draw_option(x + 48, y + 192, w - 96, "Username",
                    g_view.pending_username.empty() ? "Select to enter your username"
                                                    : g_view.pending_username,
                    g_view.setup_index == 0);
        const std::string password = g_view.pending_password.empty()
                                         ? "Select to enter your password"
                                     : g_view.show_password ? g_view.pending_password
                                                            : "Password entered";
        draw_option(x + 48, y + 284, w - 96, "Password", password, g_view.setup_index == 1);
        draw_option(x + 48, y + 376, w - 96,
                    g_view.show_password ? "Hide password" : "Show password",
                    "Control whether your password is visible", g_view.setup_index == 2);
        draw_option(x + 48, y + 468, w - 96, "Sign in", "Connect with this account",
                    g_view.setup_index == 3);
        (void)draw_button_hints(x + 48, y + h - 64,
                                {{icons::Icon::ps_circle, "Other sign-in options"}});
        if (!detail.empty())
            text::draw_ellipsized(x + 48, y + h - 32, w - 96, detail, 20, Weight::regular,
                                  gfx::palette::danger);
        return;
    }

    if (!code.empty())
    {
        gfx::rounded_rect(x + 48, y + 140, w - 96, 150, 14, gfx::palette::surface_high);
        text::draw(x + 72, y + 154, "Quick Connect code", 19, Weight::medium,
                   gfx::palette::text_faint);
        const int size = 68;
        const int width = text::measure(code, size, Weight::bold);
        text::draw(x + (w - width) / 2, y + 190, code, size, Weight::bold,
                   gfx::palette::accent_alt);
    }
    else
    {
        gfx::rounded_rect(x + 48, y + 140, w - 96, 150, 14, gfx::palette::surface_high);
        text::draw_wrapped(x + 72, y + 176, w - 144, 2, 30,
                           "Quick Connect is unavailable. Sign in with your username instead.", 22,
                           Weight::regular, gfx::palette::text_dim);
    }

    draw_option(x + 48, y + 320, w - 96, "Use Quick Connect",
                "Approve the code above in any Jellyfin app", g_view.setup_index == 0);
    draw_option(x + 48, y + 412, w - 96, "Sign in with a username",
                "Enter your account using the PS5 keyboard", g_view.setup_index == 1);
    draw_option(x + 48, y + 504, w - 96, "Change server", config::current().host,
                g_view.setup_index == 2);

    text::draw(x + 48, y + h - 40, status, 21, Weight::medium, gfx::palette::text_dim);
    if (!detail.empty())
        text::draw_ellipsized(x + 300, y + h - 40, w - 348, detail, 20, Weight::regular,
                              gfx::palette::danger);
}

void draw_message(const std::string &status, const std::string &detail) noexcept;

/* "2026-07-15T00:00:00Z" becomes "Jul 15, 2026". */
std::string pretty_date(const std::string &iso) noexcept
{
    if (iso.size() < 10)
        return {};
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const int year = std::atoi(iso.substr(0, 4).c_str());
    const int month = std::atoi(iso.substr(5, 2).c_str());
    const int day = std::atoi(iso.substr(8, 2).c_str());
    if (year <= 0 || month < 1 || month > 12 || day < 1)
        return {};
    return std::string{months[month - 1]} + " " + std::to_string(day) + ", " + std::to_string(year);
}

/* A small outlined chip, as televisions use for format badges. */
/*
 * A small fact about the file: the rating, the resolution, the codec.
 *
 * These were a white tint over the backdrop with dim grey lettering, which
 * works over a dark frame and disappears over a bright one -- and the backdrop
 * behind them is a different picture for every title. A dark backing instead
 * of a light one is legible over both, because it is the one thing on screen
 * that does not depend on what is behind it.
 */
void draw_tomato_icon(int x, int y) noexcept
{
    /* At 20 px a perfect red circle reads as a notification dot, not a tomato.
       Build a tiny lobed fruit with a visible green calyx/stem instead. */
    constexpr gfx::Color dark_red = gfx::rgb(0xc9, 0x22, 0x2d);
    constexpr gfx::Color red = gfx::rgb(0xf2, 0x35, 0x2f);
    constexpr gfx::Color green = gfx::rgb(0x42, 0xb8, 0x4a);

    gfx::rounded_rect(x + 1, y + 6, 24, 17, 8, dark_red);
    gfx::rounded_rect(x, y + 7, 13, 15, 7, red);
    gfx::rounded_rect(x + 12, y + 7, 13, 15, 7, red);
    gfx::rounded_rect(x + 5, y + 5, 15, 18, 8, red);

    /* Stem + four-point calyx. Small rectangles survive the TV scaler much
       better than a detailed vector mark would at this size. */
    gfx::rounded_rect(x + 11, y, 4, 7, 2, green);
    gfx::rounded_rect(x + 5, y + 4, 15, 4, 2, green);
    gfx::rounded_rect(x + 3, y + 6, 7, 3, 1, green);
    gfx::rounded_rect(x + 16, y + 6, 7, 3, 1, green);
    gfx::rounded_rect(x + 5, y + 9, 4, 3, 1, green);
    gfx::rounded_rect(x + 17, y + 9, 4, 3, 1, green);

    gfx::rounded_rect(x + 5, y + 10, 4, 3, 2, gfx::rgba(0xff, 0xff, 0xff, 0x72));
}

int draw_badge(int x, int y, const std::string &label) noexcept
{
    const int text_width = text::measure(label, 18, Weight::medium);
    const int width = text_width + 24;
    gfx::rounded_rect(x, y, width, 30, 8, gfx::rgba(0x00, 0x00, 0x00, 0x9e));
    gfx::stroke_rounded_rect(x, y, width, 30, 8, 1, gfx::rgba(0xff, 0xff, 0xff, 0x30));
    text::draw(x + 12, y + 5, label, 18, Weight::medium, gfx::rgba(0xff, 0xff, 0xff, 0xe0));
    return width + 10;
}

/* A pill used for the action buttons on the detail screen. */
std::string resume_label(const jellyfin::Item &item) noexcept;

/* Size item action buttons to their labels with consistent padding. */
constexpr int kActionHeight = 50;
constexpr int kActionPad = 30; /* either side of the label */
constexpr int kActionGap = 14;

int action_width(const std::string &label) noexcept
{
    return text::measure(label, 22, Weight::medium) + kActionPad * 2;
}

void draw_action(int x, int y, const std::string &label, bool focused) noexcept
{
    const int w = action_width(label);
    gfx::rounded_rect(x, y, w, kActionHeight, 12, gfx::rgba(0x22, 0x27, 0x30, 0xf0));
    if (focused)
        ui::focus_pill(x, y, w, kActionHeight);
    if (!focused)
        gfx::stroke_rounded_rect(x, y, w, kActionHeight, 12, 1, gfx::rgba(0xff, 0xff, 0xff, 0x24));
    text::draw(x + kActionPad, y + 13, label, 22, Weight::medium, gfx::palette::text);
}

/*
 * The actions an item offers, in the order they are shown.
 *
 * Built once and used by both the drawing and the input, because the two used
 * to agree about it through parallel arithmetic -- a `next` counter here, a
 * `version_action` index there -- and adding one action meant getting the same
 * sum right in two places.
 */
enum class DetailAction : std::uint8_t
{
    play,
    restart,
    watched,
    series,
    details, /* the file's own facts; always last, so always the rightmost */
};

struct DetailButton
{
    DetailAction action;
    std::string label;
};

std::vector<DetailButton> detail_actions(const jellyfin::Item &item) noexcept
{
    std::vector<DetailButton> out;
    if (item.type != "Movie" && item.type != "Episode")
        return out;
    out.push_back({DetailAction::play, resume_label(item)});
    /* Only where there is something to go back from. */
    if (item.resume_ticks > 0.0)
        out.push_back({DetailAction::restart, "Start from beginning"});
    out.push_back({DetailAction::watched, item.played ? "Unwatched" : "Watched"});
    if (item.type == "Episode" && !item.series_id.empty())
        out.push_back({DetailAction::series, "Go to series"});
    if (!item.sources.empty())
        out.push_back({DetailAction::details, "Details"});
    return out;
}

/*
 * The Version bar : a title with several files -- a 4K and
 * a 1080p copy, a theatrical and an extended cut -- gets its own row above the
 * buttons naming every version, with the chosen one lit. The badges at the top,
 * the Details sheet and Play all follow the choice. Up from the buttons
 * focuses it; left and right choose.
 */
constexpr int kVersionHeight = 44;
constexpr int kVersionPad = 24;
constexpr int kVersionGap = 10;

bool has_versions(const jellyfin::Item &item) noexcept
{
    return (item.type == "Movie" || item.type == "Episode") && item.sources.size() > 1;
}

std::size_t chosen_source(const jellyfin::Item &item) noexcept
{
    return static_cast<std::size_t>(std::clamp(
        g_view.detail_source, 0, std::max(0, static_cast<int>(item.sources.size()) - 1)));
}

/* What a version is called: its quality, or its own name ("Extended",
   "Director's Cut") when two versions share a quality. */
std::string version_name(const jellyfin::Item &item, std::size_t which) noexcept
{
    const jellyfin::MediaSource &source = item.sources[which];
    std::string name;
    for (char c : source.name)
        if (c != '[' && c != ']')
            name += c;
    while (!name.empty() && name.front() == ' ')
        name.erase(name.begin());
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    const std::string quality = source.quality_label();
    bool shared = false;
    for (std::size_t i = 0; i < item.sources.size(); ++i)
        if (i != which && item.sources[i].quality_label() == quality)
            shared = true;
    if (!quality.empty() && !shared)
        return quality;
    if (!name.empty() && name != item.name)
        return name;
    return quality.empty() ? "Version " + std::to_string(which + 1)
                           : quality + " (" + std::to_string(which + 1) + ")";
}

/* The chosen version in a line: picture, sound and size. */
std::string version_facts(const jellyfin::MediaSource &source) noexcept
{
    const std::string dot = "  \xc2\xb7  ";
    std::string out;
    const auto add = [&](const std::string &part)
    {
        if (!part.empty())
            out += (out.empty() ? "" : dot) + part;
    };
    const std::string codec = source.video_codec == "hevc"   ? "HEVC"
                              : source.video_codec == "h264" ? "H.264"
                              : source.video_codec == "av1"  ? "AV1"
                                                             : source.video_codec;
    std::string picture = codec;
    if (!source.video_dovi.empty())
        picture += " Dolby Vision";
    else if (!source.video_range.empty() && source.video_range != "SDR")
        picture += " " + source.video_range;
    add(picture);
    for (const jellyfin::Track &track : source.audio_tracks)
        if (track.index == source.default_audio)
        {
            /* The profile says more only for DTS and TrueHD ("DTS-HD MA");
               for AAC it is "LC", which says nothing to a viewer. */
            const bool named =
                !track.profile.empty() && (track.codec == "dts" || track.codec == "truehd");
            std::string sound = named                     ? track.profile
                                : track.codec == "aac"    ? "AAC"
                                : track.codec == "eac3"   ? "Dolby Digital+"
                                : track.codec == "ac3"    ? "Dolby Digital"
                                : track.codec == "truehd" ? "TrueHD"
                                : track.codec == "dts"    ? "DTS"
                                                          : track.codec;
            if (track.channels == 8)
                sound += " 7.1";
            else if (track.channels == 6)
                sound += " 5.1";
            else if (track.channels == 2)
                sound += " Stereo";
            add(sound);
        }
    if (source.size_bytes > 0)
        add(details::size_text(source.size_bytes));
    return out;
}

void draw_version_bar(const jellyfin::Item &item, int x, int y, int right) noexcept
{
    const bool focused = g_view.detail_action == kDetailVersion;
    const std::size_t chosen = chosen_source(item);
    const int label_y = y + (kVersionHeight - 24) / 2;
    text::draw(x, label_y, "Version", 20, Weight::medium,
               focused ? gfx::palette::text : gfx::palette::text_dim);
    int cursor = x + text::measure("Version", 20, Weight::medium) + 22;
    for (std::size_t i = 0; i < item.sources.size(); ++i)
    {
        const std::string name = version_name(item, i);
        const int w = text::measure(name, 20, Weight::medium) + kVersionPad * 2;
        if (cursor + w > right)
            break;
        const bool lit = i == chosen;
        gfx::Color fill = gfx::rgba(0xff, 0xff, 0xff, 0x00);
        gfx::Color ink = gfx::rgba(0xff, 0xff, 0xff, 0x99);
        if (lit && focused)
        {
            fill = gfx::palette::focus_surface;
            ink = gfx::palette::text;
        }
        else if (lit)
        {
            fill = gfx::rgba(0xff, 0xff, 0xff, 0x33);
            ink = gfx::palette::text;
        }
        gfx::rounded_rect(cursor, y, w, kVersionHeight, kVersionHeight / 2, fill);
        if (lit && focused)
            gfx::stroke_rounded_rect(cursor, y, w, kVersionHeight, kVersionHeight / 2, 2,
                                     gfx::palette::focus_border);
        else
            gfx::stroke_rounded_rect(cursor, y, w, kVersionHeight, kVersionHeight / 2, 1,
                                     gfx::rgba(0xff, 0xff, 0xff, lit ? 0x40 : 0x24));
        text::draw(cursor + kVersionPad, label_y, name, 20, Weight::medium, ink);
        cursor += w + kVersionGap;
    }
    const std::string facts = version_facts(item.sources[chosen]);
    if (!facts.empty() && cursor + 40 < right)
        text::draw_ellipsized(cursor + 14, label_y + 1, right - cursor - 14, facts, 20,
                              Weight::regular, gfx::palette::text_faint);
}

std::string resume_label(const jellyfin::Item &item) noexcept
{
    if (item.resume_ticks <= 0.0)
        return "Play";
    const int seconds = static_cast<int>(item.resume_ticks / 10000000.0);
    const int minutes = seconds / 60;
    if (minutes >= 60)
        return "Resume from " + std::to_string(minutes / 60) + "h " + std::to_string(minutes % 60) +
               "m";
    return "Resume from " + std::to_string(minutes) + "m";
}

/* Lines the Details sheet shows at once. */
constexpr int kDetailsVisible = 17;

/*
 * What the file on the server is: a properties sheet over the detail screen,
 * following whichever version is selected. Built from data the detail request
 * already carries (details_sheet.hpp), so opening it waits on nothing.
 */
void draw_details_sheet(const jellyfin::Item &item) noexcept
{
    if (item.sources.empty())
        return;
    const std::size_t which = static_cast<std::size_t>(
        std::clamp(g_view.detail_source, 0, static_cast<int>(item.sources.size()) - 1));
    std::vector<details::Line> lines = details::build(item, item.sources[which]);
    lines.push_back({details::Line::Kind::heading, "On this PS5", {}});
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        const bool mine = g_shared.delivery_key == item.id + "|" + item.sources[which].id;
        if (mine && g_shared.delivery_ready)
            lines.insert(lines.end(), g_shared.delivery_lines.begin(),
                         g_shared.delivery_lines.end());
        else
            lines.push_back({details::Line::Kind::pair, " ", "Asking the server\xe2\x80\xa6"});
    }

    gfx::fill_rect(0, 0, gfx::kWidth, gfx::kHeight, gfx::rgba(0x00, 0x00, 0x00, 0xb8));
    constexpr int kW = 1240;
    constexpr int kH = 900;
    constexpr int kLine = 40;
    constexpr int kKeyWidth = 190;
    const int x = (gfx::kWidth - kW) / 2;
    const int y = (gfx::kHeight - kH) / 2;
    panel(x, y, kW, kH);

    text::draw(x + 48, y + 34, "Details", 34, Weight::bold, gfx::palette::text);
    std::string subtitle =
        item.series_name.empty() ? item.name : item.series_name + "  \xe2\x80\x94  " + item.name;
    if (item.sources.size() > 1)
    {
        const std::string version = item.sources[which].quality_label();
        subtitle +=
            "  \xc2\xb7  " + (version.empty() ? "Version " + std::to_string(which + 1) : version);
    }
    text::draw_ellipsized(x + 48, y + 84, kW - 96, subtitle, 22, Weight::regular,
                          gfx::palette::text_dim);

    const int last_first = std::max(0, static_cast<int>(lines.size()) - kDetailsVisible);
    g_view.details_scroll = std::clamp(g_view.details_scroll, 0, last_first);
    int row_y = y + 140;
    for (int i = g_view.details_scroll;
         i < static_cast<int>(lines.size()) && i < g_view.details_scroll + kDetailsVisible; ++i)
    {
        const details::Line &line = lines[static_cast<std::size_t>(i)];
        if (line.kind == details::Line::Kind::heading)
            text::draw(x + 48, row_y + 10, line.key, 21, Weight::medium, gfx::palette::accent);
        else
        {
            text::draw_ellipsized(x + 48, row_y + 6, kKeyWidth - 20, line.key, 22, Weight::regular,
                                  gfx::palette::text_faint);
            text::draw_ellipsized(x + 48 + kKeyWidth, row_y + 6, kW - 96 - kKeyWidth, line.value,
                                  22, Weight::regular, gfx::palette::text);
        }
        row_y += kLine;
    }

    const std::string hint =
        last_first > 0 ? "Up and down to scroll    \xc2\xb7    Circle to close" : "Circle to close";
    text::draw(x + 48, y + kH - 50, hint, 20, Weight::regular, gfx::palette::text_faint);
}

/*
 * A series' Next Up : a large still in the top right of
 * the page, which episode it is, and how far in the viewer got. Chosen, it
 * opens that episode. It began as a small card squeezed under the synopsis;
 * Use a readable action target at television viewing distance.
 */
void draw_next_up_feature(const jellyfin::Item &next, int x, int y) noexcept
{
    text::draw(x, y, "Next Up", 28, Weight::medium, gfx::palette::text);
    const int card_y = y + 44;
    const bool borrowed =
        next.image_tag.empty() && !next.series_id.empty() && !next.series_primary_tag.empty();
    const std::string still_id = borrowed ? next.series_id : next.id;
    const std::string still_tag = borrowed ? next.series_primary_tag : next.image_tag;
    const gfx::Bitmap *still =
        images::acquire(still_id, still_tag, images::Kind::primary, kNextUpCardHeight);
    const std::string key = still_id + "|p";
    if (still == nullptr)
        g_awaited.insert(key);
    const std::uint8_t fade = still != nullptr ? artwork_fade(key) : 0;
    if (fade < 255)
        draw_card_placeholder(x, card_y, kNextUpCardWidth, kNextUpCardHeight, next);
    if (still != nullptr)
        gfx::blit_cover(*still, x, card_y, kNextUpCardWidth, kNextUpCardHeight, 12, fade);
    if (next.played_percentage > 0.5)
    {
        const int track = kNextUpCardWidth - 24;
        const int filled = static_cast<int>(track * next.played_percentage / 100.0);
        gfx::rounded_rect(x + 12, card_y + kNextUpCardHeight - 18, track, 6, 3,
                          gfx::rgba(0xff, 0xff, 0xff, 0x40));
        gfx::rounded_rect(x + 12, card_y + kNextUpCardHeight - 18, filled, 6, 3,
                          gfx::palette::accent);
    }

    int ty = card_y + kNextUpCardHeight + 16;
    std::string heading = next.name;
    if (next.parent_index_number > 0)
        heading = "S" + std::to_string(next.parent_index_number) + ":E" +
                  std::to_string(next.index_number) + "  " + next.name;
    text::draw_ellipsized(x, ty, kNextUpCardWidth, heading, 26, Weight::bold, gfx::palette::text);
    ty += 40;
    /* How far in, or else how long and when it aired. */
    std::string meta;
    if (next.resume_ticks > 0.0)
        meta = resume_label(next);
    else
    {
        if (next.runtime_minutes > 0)
            meta = std::to_string(next.runtime_minutes) + "m";
        if (const std::string aired = pretty_date(next.premiere_date); !aired.empty())
            meta += (meta.empty() ? "" : "  \xc2\xb7  ") + aired;
    }
    if (!meta.empty())
    {
        text::draw_ellipsized(x, ty, kNextUpCardWidth, meta, 21, Weight::regular,
                              gfx::palette::text_dim);
        ty += 38;
    }
    /* It takes no focus, so the buttons that reach it are always shown. */
    const bool started = next.resume_ticks > 0.0;
    (void)draw_button_hints(x, ty + 4,
                            {{icons::Icon::ps_triangle, started ? "Resume" : "Play"},
                             {icons::Icon::ps_square, "Episode"}});
}

std::string time_of_day(double ahead) noexcept;

/* Detail hero, metadata and focus-scrolling content rows. */
constexpr int kDetailPosterY = kSafeY + 60;
constexpr int kDetailPosterW = 300;
constexpr int kDetailPosterH = 450;
constexpr int kHeroBottom = kDetailPosterY + kDetailPosterH;
/* let the episode/
   season strip breathe before Cast & Crew instead of squeezing both rows into
   the first viewport. The next row is intentionally reached by scrolling. */
constexpr int kDetailRowGap = 42;
constexpr int kCastPhoto = kCastPhotoHeight;
constexpr int kCastPitch = kCastPhoto + 44;
constexpr int kCastRowHeight = 44 + kCastPhoto + 84;
/* The right-hand column a series' Next Up takes in the hero. */
constexpr int kNextUpX = kContentRight - kNextUpCardWidth;
/* How far down the screen a focused row may reach before the page moves. */
constexpr int kDetailVisibleBottom = gfx::kHeight - 34;

struct DetailLayout
{
    int strip_top = 0; /* heading tops, before the page scrolls */
    int strip_bottom = 0;
    int cast_top = 0;
    int cast_bottom = 0;
    bool strip = false;
    bool cast = false;
};

DetailLayout detail_layout(const jellyfin::Item &item, const std::vector<jellyfin::Item> &episodes,
                           bool loading) noexcept
{
    DetailLayout layout;
    int y = kHeroBottom + 26;
    if (!episodes.empty())
    {
        const StripShape shape = strip_shape(episodes);
        layout.strip = true;
        layout.strip_top = shape.y - shape.heading;
        layout.strip_bottom = shape.y + shape.card_h + (shape.seasons ? 62 : 44);
        y = layout.strip_bottom + kDetailRowGap;
    }
    /* Lower rows publish as one stable layout. A prefetched Item may already
       contain People while its seasons/episodes are still in flight; drawing
       Cast during that gap makes it appear under the hero for a few frames and
       then jump down when the strip arrives. */
    if (!loading && !item.people.empty())
    {
        layout.cast = true;
        /* With nothing between it and the hero, the cast sits in the middle of
           the space that is left, rather than hard under the buttons with the
           bottom of the screen empty. */
        if (!layout.strip)
            y = kHeroBottom + std::max(26, (gfx::kHeight - kHeroBottom - kCastRowHeight) / 2);
        layout.cast_top = y;
        layout.cast_bottom = y + kCastRowHeight;
    }
    return layout;
}

/* Where the page sits for whichever row has the focus. */
float detail_page_target(const DetailLayout &layout) noexcept
{
    int bottom = 0;
    if (g_view.detail_action == -1 && layout.strip)
        bottom = layout.strip_bottom;
    else if (g_view.detail_action == kDetailCast && layout.cast)
        bottom = layout.cast_bottom;
    return static_cast<float>(std::max(0, bottom - kDetailVisibleBottom));
}

std::string join_people(const jellyfin::Item &item, const char *type, std::size_t limit) noexcept
{
    std::string out;
    std::size_t count = 0;
    for (const jellyfin::Person &person : item.people)
        if (person.type == type && count < limit)
        {
            out += (count++ == 0 ? "" : ", ") + person.name;
        }
    return out;
}

void draw_cast_row(const jellyfin::Item &item, int top) noexcept
{
    text::draw(kSafeX, top, "Cast & Crew", 28, Weight::medium, gfx::palette::text);
    const int photo_y = top + 44;
    const bool row_focused = g_view.detail_action == kDetailCast;
    gfx::push_clip(kSafeX - 24, photo_y - 12, kContentRight - kSafeX + 48, kCastPhoto + 96);
    for (std::size_t i = 0; i < item.people.size(); ++i)
    {
        const int x = kSafeX + static_cast<int>(i) * kCastPitch -
                      static_cast<int>(g_view.detail_cast_scroll.value());
        if (x > kContentRight || x + kCastPhoto < kSafeX - 40)
            continue;
        const jellyfin::Person &person = item.people[i];
        const gfx::Bitmap *photo =
            person.image_tag.empty()
                ? nullptr
                : images::acquire(person.id, person.image_tag, images::Kind::primary, kCastPhoto);
        if (photo != nullptr)
            gfx::blit_cover(*photo, x, photo_y, kCastPhoto, kCastPhoto, kCastPhoto / 2, 255);
        else
        {
            gfx::rounded_rect(x, photo_y, kCastPhoto, kCastPhoto, kCastPhoto / 2,
                              gfx::rgba(0xff, 0xff, 0xff, 0x12));
            const std::string initial = person.name.empty() ? "?" : person.name.substr(0, 1);
            text::draw(x + (kCastPhoto - text::measure(initial, 60, Weight::bold)) / 2,
                       photo_y + kCastPhoto / 2 - 40, initial, 60, Weight::bold,
                       gfx::rgba(0xff, 0xff, 0xff, 0x40));
        }
        const bool focused = row_focused && static_cast<int>(i) == g_view.detail_cast_index;
        if (focused)
            gfx::stroke_rounded_rect(x - 5, photo_y - 5, kCastPhoto + 10, kCastPhoto + 10,
                                     kCastPhoto / 2 + 5, 3, gfx::palette::text);
        const int label_w = kCastPitch - 16;
        const int label_x = x + kCastPhoto / 2 - label_w / 2;
        const auto centred =
            [&](const std::string &words, int y, int size, Weight weight, gfx::Color colour)
        {
            const int w = std::min(label_w, text::measure(words, size, weight));
            text::draw_ellipsized(label_x + (label_w - w) / 2, y, label_w, words, size, weight,
                                  colour);
        };
        centred(person.name, photo_y + kCastPhoto + 14, 21, Weight::medium, gfx::palette::text);
        const std::string role =
            person.type == "Actor" || person.type == "GuestStar" ? person.role : person.type;
        if (!role.empty())
            centred(role, photo_y + kCastPhoto + 42, 19, Weight::regular, gfx::palette::text_faint);
    }
    gfx::pop_clip();
}

void draw_detail(const jellyfin::Item &item, const std::vector<jellyfin::Item> &episodes,
                 const jellyfin::Item &next_up, bool loading) noexcept
{
    /* Only an empty page waits: with the card's own record in hand the page
       is drawn at once and the rest of it arrives underneath. */
    if (item.id.empty())
    {
        draw_message(loading ? "Loading" : "Nothing to show", {});
        return;
    }

    /* Backdrop across the whole page, scrimmed for legibility. It stays put
       while the page scrolls over it. */
    const gfx::Bitmap *art = nullptr;
    if (!item.backdrop_tag.empty())
        art = images::acquire(item.id, item.backdrop_tag, images::Kind::backdrop, 1080);
    if (art == nullptr && !item.parent_backdrop_tag.empty())
    {
        const std::string owner =
            item.parent_backdrop_id.empty() ? item.series_id : item.parent_backdrop_id;
        if (!owner.empty())
            art = images::acquire(owner, item.parent_backdrop_tag, images::Kind::backdrop, 1080);
    }
    if (wash_begin(item.id + (art != nullptr ? "+" : "-"), 1, art == nullptr) && art != nullptr)
    {
        /* Fade the backdrop into the page background. */
        gfx::blit_cover(*art, 0, 0, gfx::kWidth, gfx::kHeight, 0, 255);
        gfx::media_scrim(gfx::kHeight, gfx::rgb(0x0d, 0x0d, 0x12), 0x99, 360, 1780, 0xf0);
    }
    wash_end();

    const DetailLayout layout = detail_layout(item, episodes, loading);
    g_view.detail_page.aim(detail_page_target(layout));
    const int shift = static_cast<int>(g_view.detail_page.value() + 0.5f);

    /* Poster beside the text, as the web and TV clients both do. */
    const int poster_x = kSafeX;
    const int poster_y = kDetailPosterY - shift;
    const gfx::Bitmap *poster =
        images::acquire(item.id, item.image_tag, images::Kind::primary, 560);
    if (poster != nullptr)
        gfx::blit_cover(*poster, poster_x, poster_y, kDetailPosterW, kDetailPosterH, 14, 255);
    else
        draw_card_placeholder(poster_x, poster_y, kDetailPosterW, kDetailPosterH, item);

    const int x = poster_x + kDetailPosterW + 48;
    /* A series' Next Up takes the right of the hero; the text stops short of it. */
    const bool show_next_up = !next_up.id.empty();
    const int right = show_next_up ? kNextUpX - 64 : kContentRight;
    int y = poster_y - 6;

    /* television pages
       should lead with the show's name at the same scale a movie leads with
       its title. Episode/season names become the secondary line instead of
       making the series identity look like a small eyebrow. */
    if ((item.type == "Episode" || item.type == "Season") && !item.series_name.empty())
    {
        text::draw_ellipsized(x, y, right - x, item.series_name, 52, Weight::bold,
                              gfx::palette::text);
        y += 64;
        std::string secondary = item.name;
        if (item.type == "Episode" && item.parent_index_number > 0 && item.index_number > 0)
            secondary = "S" + std::to_string(item.parent_index_number) + ":E" +
                        std::to_string(item.index_number) + "  \xe2\x80\x94  " + item.name;
        text::draw_ellipsized(x, y, right - x, secondary, 30, Weight::medium,
                              gfx::palette::accent_alt);
        y += 48;
    }
    else
    {
        text::draw_ellipsized(x, y, right - x, item.name, 52, Weight::bold, gfx::palette::text);
        y += 72;
    }

    /* Rating, season and episode, release date, then the format badges. */
    int cursor = x;
    /* CommunityRating supplies the audience score; CriticRating is a separate metric. */
    if (item.community_rating > 0.0)
    {
        const int percent = static_cast<int>(item.community_rating * 10.0 + 0.5);
        draw_tomato_icon(cursor, y + 2);
        cursor += 34;
        const std::string score = std::to_string(percent) + "%";
        text::draw(cursor, y, score, 22, Weight::medium, gfx::palette::text);
        cursor += text::measure(score, 22, Weight::medium) + 8;
        text::draw(cursor, y + 3, "audience", 18, Weight::regular, gfx::palette::text_faint);
        cursor += text::measure("audience", 18, Weight::regular) + 24;
    }
    if (item.type == "Episode" && item.parent_index_number > 0)
    {
        const std::string code = "S" + std::to_string(item.parent_index_number) + ":E" +
                                 std::to_string(item.index_number);
        text::draw(cursor, y, code, 22, Weight::medium, gfx::palette::text);
        cursor += text::measure(code, 22, Weight::medium) + 24;
    }
    const std::string released = pretty_date(item.premiere_date);
    if (!released.empty() && item.type != "Series")
    {
        text::draw(cursor, y, released, 22, Weight::medium, gfx::palette::text);
        cursor += text::measure(released, 22, Weight::medium) + 24;
    }
    if (!item.official_rating.empty())
        cursor += draw_badge(cursor, y - 3, item.official_rating);

    if (!item.sources.empty())
    {
        const std::size_t which = static_cast<std::size_t>(
            std::clamp(g_view.detail_source, 0, static_cast<int>(item.sources.size()) - 1));
        for (const std::string &badge : item.sources[which].badges())
        {
            if (cursor > right - 120)
                break;
            cursor += draw_badge(cursor, y - 3, badge);
        }
    }
    y += 54;

    /* The facts: how long and when it would end, or for a show its years and
       size; then the genres. */
    {
        const std::string dot = "  \xc2\xb7  ";
        std::string facts;
        const auto add = [&facts, &dot](const std::string &part)
        {
            if (!part.empty())
                facts += (facts.empty() ? "" : dot) + part;
        };
        if (item.type == "Series")
        {
            const int from = item.production_year;
            const std::string until = item.status == "Continuing" ? std::string{"Present"}
                                      : item.end_date.size() >= 4 ? item.end_date.substr(0, 4)
                                                                  : std::string{};
            if (from > 0)
                add(until.empty() || until == std::to_string(from)
                        ? std::to_string(from)
                        : std::to_string(from) + " \xe2\x80\x93 " + until);
            if (item.child_count > 0)
                add(std::to_string(item.child_count) +
                    (item.child_count == 1 ? " season" : " seasons"));
            if (item.recursive_item_count > 0)
                add(std::to_string(item.recursive_item_count) + " episodes");
        }
        else if (item.type == "Season")
        {
            if (!episodes.empty())
                add(std::to_string(episodes.size()) +
                    (episodes.size() == 1 ? " episode" : " episodes"));
        }
        else if (item.runtime_minutes > 0)
        {
            const int hours = item.runtime_minutes / 60;
            const int minutes = item.runtime_minutes % 60;
            add(hours > 0 ? std::to_string(hours) + "h " + std::to_string(minutes) + "m"
                          : std::to_string(minutes) + "m");
            const double left = item.runtime_minutes * 60.0 - item.resume_ticks / 10000000.0;
            if (const std::string ends = time_of_day(std::max(0.0, left)); !ends.empty())
                add("Ends at " + ends);
        }
        add(item.genres);
        if (item.type == "Series" && !next_up.id.empty())
            add(item.studios);
        if (!facts.empty())
        {
            text::draw_ellipsized(x, y, right - x, facts, 22, Weight::regular,
                                  gfx::rgb(0xc4, 0xc4, 0xcc));
            y += 40;
        }
    }

    /* Who made it. */
    {
        std::string credits;
        const std::string directors = join_people(item, "Director", 2);
        const std::string writers = join_people(item, "Writer", 2);
        if (!directors.empty())
            credits = "Directed by " + directors;
        if (!writers.empty())
            credits += (credits.empty() ? "Written by " : "  \xc2\xb7  Written by ") + writers;
        /* A show with a Next Up needs the room for its synopsis; its studios
           then ride on the line above. */
        if (!item.studios.empty() && next_up.id.empty())
            credits += (credits.empty() ? "" : "  \xc2\xb7  ") + item.studios;
        if (!credits.empty())
        {
            text::draw_ellipsized(x, y, right - x, credits, 20, Weight::regular,
                                  gfx::palette::text_dim);
            y += 36;
        }
    }

    if (!item.tagline.empty())
    {
        text::draw_ellipsized(x, y + 2, right - x, item.tagline, 22, Weight::medium,
                              gfx::palette::accent_alt);
        y += 36;
    }

    const std::vector<DetailButton> actions = detail_actions(item);
    const int buttons_y = poster_y + kDetailPosterH - 50;
    const bool versions = has_versions(item);
    const int version_y = buttons_y - kVersionHeight - 18;
    const int text_floor = versions           ? version_y - 22
                           : !actions.empty() ? buttons_y - 22
                                              : poster_y + kDetailPosterH;
    if (!item.overview.empty())
    {
        y += 8;
        const int room = (text_floor - y) / 37;
        (void)text::draw_wrapped(x, y, right - x - 20, std::clamp(room, 1, 6), 37, item.overview,
                                 23, Weight::regular, gfx::rgb(0xcf, 0xcf, 0xd6));
    }
    if (show_next_up)
        draw_next_up_feature(next_up, kNextUpX, poster_y - 6);
    if (versions)
        draw_version_bar(item, x, version_y, right);

    {
        int action_x = x;
        for (std::size_t i = 0; i < actions.size(); ++i)
        {
            draw_action(action_x, buttons_y, actions[i].label,
                        g_view.detail_action == static_cast<int>(i));
            action_x += action_width(actions[i].label) + kActionGap;
        }
    }

    if (layout.cast)
        draw_cast_row(item, layout.cast_top - shift);

    if (episodes.empty())
        return;

    /* Display seasons, episodes or sibling episodes according to item type. */
    const StripShape shape = strip_shape(episodes);
    const int strip_y = shape.y - shift;
    const std::string heading = shape.seasons ? std::string{"Seasons"}
                                : item.type == "Episode" && item.parent_index_number > 0
                                    ? "More from Season " + std::to_string(item.parent_index_number)
                                    : std::string{"Episodes"};
    text::draw(kSafeX, strip_y - shape.heading, heading, 28, Weight::medium, gfx::palette::text);
    gfx::push_clip(kSafeX - 24, strip_y - 20, kContentRight - kSafeX + 48, shape.card_h + 110);
    for (std::size_t i = 0; i < episodes.size(); ++i)
    {
        const int ex = kSafeX + static_cast<int>(i) * shape.pitch -
                       static_cast<int>(g_view.detail_scroll.value());
        if (ex > kContentRight || ex + shape.card_w < kSafeX - 40)
        {
            /* Warm the ones just off each end, as the grids do. */
            if (ex < kContentRight + shape.pitch * 3 &&
                ex + shape.card_w > kSafeX - shape.pitch * 3)
                images::prefetch(episodes[i].id, episodes[i].image_tag, images::Kind::primary,
                                 shape.card_h);
            continue;
        }
        const jellyfin::Item &entry = episodes[i];
        const bool focused =
            static_cast<int>(i) == g_view.detail_episode && g_view.detail_action == -1;

        /* Same fallback as a card: the show's picture beats a blank one. */
        const bool borrowed = entry.image_tag.empty() && !entry.series_id.empty() &&
                              !entry.series_primary_tag.empty();
        const std::string thumb_id = borrowed ? entry.series_id : entry.id;
        const std::string thumb_tag = borrowed ? entry.series_primary_tag : entry.image_tag;
        const gfx::Bitmap *thumb =
            images::acquire(thumb_id, thumb_tag, images::Kind::primary, shape.card_h);
        const std::string thumb_key = thumb_id + "|p";
        if (thumb == nullptr)
            g_awaited.insert(thumb_key);
        const std::uint8_t thumb_fade = thumb != nullptr ? artwork_fade(thumb_key) : 0;
        if (thumb_fade < 255)
            draw_card_placeholder(ex, strip_y, shape.card_w, shape.card_h, entry);
        if (thumb != nullptr)
            gfx::blit_cover(*thumb, ex, strip_y, shape.card_w, shape.card_h, 10, thumb_fade);
        if (entry.played_percentage > 0.5)
        {
            const int track = shape.card_w - 20;
            const int filled = static_cast<int>(track * entry.played_percentage / 100.0);
            gfx::rounded_rect(ex + 10, strip_y + shape.card_h - 16, track, 5, 3,
                              gfx::rgba(0xff, 0xff, 0xff, 0x40));
            gfx::rounded_rect(ex + 10, strip_y + shape.card_h - 16, filled, 5, 3,
                              gfx::palette::accent);
        }
        /* The episode this page is about, in the rest of its season. */
        if (entry.id == item.id)
            (void)draw_badge(ex + 10, strip_y + 10, "Viewing");
        if (focused)
            gfx::stroke_rounded_rect(ex - 3, strip_y - 3, shape.card_w + 6, shape.card_h + 6, 13, 3,
                                     gfx::palette::text);

        const std::string label =
            shape.seasons ? entry.name : std::to_string(entry.index_number) + ". " + entry.name;
        text::draw_ellipsized(ex, strip_y + shape.card_h + 12, shape.card_w, label, 22,
                              focused ? Weight::medium : Weight::regular,
                              focused ? gfx::palette::text : gfx::palette::text_dim);
        if (shape.seasons && entry.child_count > 0)
            text::draw(ex, strip_y + shape.card_h + 36,
                       std::to_string(entry.child_count) +
                           (entry.child_count == 1 ? " episode" : " episodes"),
                       19, Weight::regular, gfx::palette::text_faint);
    }
    gfx::pop_clip();
}

/* Seconds as h:mm:ss, or m:ss for anything under an hour. */
std::string clock_text(double seconds) noexcept
{
    if (seconds < 0.0)
        seconds = 0.0;
    const int total = static_cast<int>(seconds);
    const int hours = total / 3600;
    const int minutes = (total / 60) % 60;
    const int rest = total % 60;
    std::string out;
    if (hours > 0)
    {
        out += std::to_string(hours) + ":";
        if (minutes < 10)
            out += "0";
    }
    out += std::to_string(minutes) + ":";
    if (rest < 10)
        out += "0";
    out += std::to_string(rest);
    return out;
}

/* ------------------------------------------------- player overlay */

enum class PlayerAction : std::uint8_t
{
    subtitles = 0,
    audio,
    quality,
    autoplay,
    debug,
};

struct PlayerButton
{
    icons::Icon icon;
    PlayerAction action;
};

constexpr long long kQualityBitrates[] = {0, 40000000, 20000000, 12000000, 8000000, 4000000};
constexpr const char *kQualityLabels[] = {"Automatic", "40 Mb/s", "20 Mb/s",
                                          "12 Mb/s",   "8 Mb/s",  "4 Mb/s"};
constexpr const char *kVideoCodecLabels[] = {"Automatic", "H.264", "HEVC"};
constexpr const char *kVideoCodecValues[] = {"", "h264", "hevc"};

/* Group video quality, audio and subtitle controls. */
struct QualityRow
{
    const char *label;
    bool header;
    int group; /* 0 bitrate, 1 video codec */
    int value;
};

std::vector<QualityRow> quality_rows() noexcept
{
    std::vector<QualityRow> rows;
    rows.push_back({"Bitrate", true, 0, 0});
    for (int i = 0; i < static_cast<int>(std::size(kQualityBitrates)); ++i)
        rows.push_back({kQualityLabels[i], false, 0, i});
    rows.push_back({"Video codec", true, 1, 0});
    for (int i = 0; i < static_cast<int>(std::size(kVideoCodecLabels)); ++i)
        rows.push_back({kVideoCodecLabels[i], false, 1, i});
    return rows;
}

/* Show the selected audio track and effective output format. */
struct AudioRow
{
    std::string label;
    bool header;
    int group;         /* 0 track, 1 format, 2 channels */
    int value;         /* track position, or channel count */
    std::string codec; /* format rows: "" for the original */
};

/* The track playing now, or the version's default. */
const jellyfin::Track *playing_audio_track(const player::Status &status) noexcept
{
    const auto &source = g_view.playing_source;
    const int wanted = status.audio_index >= 0 ? status.audio_index : source.default_audio;
    for (const jellyfin::Track &track : source.audio_tracks)
        if (track.index == wanted)
            return &track;
    return source.audio_tracks.empty() ? nullptr : &source.audio_tracks.front();
}

std::string format_name(const std::string &codec, const std::string &profile) noexcept
{
    if (codec == "truehd")
        return profile.find("Atmos") != std::string::npos ? "TrueHD Atmos" : "TrueHD";
    if (codec == "dts" || codec == "dca")
        return profile.empty() ? "DTS" : profile;
    if (codec == "eac3")
        return profile.find("Atmos") != std::string::npos ? "Dolby Digital+ Atmos"
                                                          : "Dolby Digital+";
    if (codec == "ac3")
        return "Dolby Digital";
    if (codec == "aac")
        return "AAC";
    if (codec == "flac")
        return "FLAC";
    if (codec.rfind("pcm", 0) == 0)
        return "PCM";
    return codec;
}

/* Rank of a format, for "only below the original". */
int format_rank(const jellyfin::Track &track) noexcept
{
    const std::string &codec = track.codec;
    if (codec == "truehd" || codec == "flac" || codec.rfind("pcm", 0) == 0)
        return 4;
    if (codec == "dts" || codec == "dca")
        return track.profile.find("MA") != std::string::npos ||
                       track.profile.find("HRA") != std::string::npos
                   ? 4
                   : 3;
    if (codec == "eac3")
        return 3;
    if (codec == "ac3")
        return 2;
    return 1;
}

std::vector<AudioRow> audio_rows(const player::Status &status) noexcept
{
    std::vector<AudioRow> rows;
    const auto &source = g_view.playing_source;
    if (source.audio_tracks.size() > 1)
    {
        rows.push_back({"Track", true, 0, 0, {}});
        for (std::size_t i = 0; i < source.audio_tracks.size(); ++i)
        {
            const auto &track = source.audio_tracks[i];
            std::string label = track.label;
            if (label.empty())
            {
                label = track.language.empty() ? format_name(track.codec, track.profile)
                                               : track.language + "  \xc2\xb7  " +
                                                     format_name(track.codec, track.profile);
            }
            rows.push_back({label, false, 0, static_cast<int>(i), {}});
        }
    }
    const jellyfin::Track *track = playing_audio_track(status);
    const int rank = track != nullptr ? format_rank(*track) : 1;
    const int source_channels = track != nullptr && track->channels > 0 ? track->channels : 8;
    rows.push_back({"Format", true, 1, 0, {}});
    rows.push_back({track != nullptr
                        ? "Original  \xc2\xb7  " + format_name(track->codec, track->profile)
                        : std::string{"Original"},
                    false,
                    1,
                    0,
                    {}});
    if (rank > 3)
        rows.push_back({"Dolby Digital+", false, 1, 0, "eac3"});
    if (rank > 2)
        rows.push_back({"Dolby Digital", false, 1, 0, "ac3"});
    if (rank > 1)
        rows.push_back({"AAC", false, 1, 0, "aac"});

    const std::string format = player::current_request().audio_codec;
    const int ceiling =
        format == "ac3" || format == "eac3" ? std::min(source_channels, 6) : source_channels;
    rows.push_back({"Channels", true, 2, 0, {}});
    rows.push_back({"Automatic", false, 2, 0, {}});
    if (source_channels > 2)
        rows.push_back({"Stereo", false, 2, 2, {}});
    if (ceiling >= 6 && source_channels > 6)
        rows.push_back({"5.1", false, 2, 6, {}});
    return rows;
}

/* Only offer what this version actually has. */
std::vector<PlayerButton> player_buttons() noexcept
{
    std::vector<PlayerButton> out;
    if (!g_view.playing_source.subtitle_tracks.empty())
        out.push_back({icons::Icon::subtitles, PlayerAction::subtitles});
    if (!g_view.playing_source.audio_tracks.empty())
        out.push_back({icons::Icon::audio, PlayerAction::audio});
    out.push_back({icons::Icon::quality, PlayerAction::quality});
    if (g_view.playing_item.type == "Episode" && !g_view.playing_item.series_id.empty())
        out.push_back({icons::Icon::skip_forward, PlayerAction::autoplay});
    out.push_back({icons::Icon::info, PlayerAction::debug});
    return out;
}

/* Subtitle timing controls share the track-selection panel. */
enum class TimingRow : std::uint8_t
{
    none,
    heading,
    late,
    early,
    reset,
};

/* Refreshed once a frame from the status both the input and the drawing
   already read, rather than copying the whole status for every row. */
bool g_subtitle_text_showing = false;

void note_subtitle_state(const player::Status &status) noexcept
{
    g_subtitle_text_showing = status.subtitle_index >= 0 && !status.subtitle_burned;
}

bool subtitle_timing_offered() noexcept
{
    return g_subtitle_text_showing;
}

TimingRow timing_row(int row) noexcept
{
    if (g_view.panel_kind != 0 || !subtitle_timing_offered())
        return TimingRow::none;
    const int first = 1 + static_cast<int>(g_view.playing_source.subtitle_tracks.size());
    switch (row - first)
    {
    case 0:
        return TimingRow::heading;
    case 1:
        return TimingRow::late;
    case 2:
        return TimingRow::early;
    case 3:
        return TimingRow::reset;
    default:
        return TimingRow::none;
    }
}

/* Writes a changed offset for the title it belongs to. */
void commit_subtitle_offset() noexcept
{
    if (!g_view.subtitle_offset_dirty)
        return;
    config::set_subtitle_offset(g_view.subtitle_offset_item, g_view.subtitle_offset);
    config::save();
    g_view.subtitle_offset_dirty = false;
}

int panel_rows() noexcept
{
    if (g_view.panel_kind == 0)
        return 1 + static_cast<int>(g_view.playing_source.subtitle_tracks.size()) +
               (subtitle_timing_offered() ? 4 : 0);
    if (g_view.panel_kind == 1)
        return static_cast<int>(audio_rows(player::status()).size());
    return static_cast<int>(quality_rows().size());
}

std::string panel_row_label(int row) noexcept
{
    const auto &source = g_view.playing_source;
    if (g_view.panel_kind == 0)
    {
        switch (timing_row(row))
        {
        case TimingRow::heading:
            return "Timing  \xc2\xb7  " + subtitle_timing::describe(g_view.subtitle_offset);
        case TimingRow::late:
            return "Subtitles late?  Show them sooner";
        case TimingRow::early:
            return "Subtitles early?  Show them later";
        case TimingRow::reset:
            return "Back to original timing";
        case TimingRow::none:
            break;
        }
        if (row == 0)
            return "Off";
        const auto &track = source.subtitle_tracks[static_cast<std::size_t>(row - 1)];
        return track.label.empty() ? track.language : track.label;
    }
    if (g_view.panel_kind == 1)
    {
        const auto rows = audio_rows(player::status());
        return row >= 0 && row < static_cast<int>(rows.size())
                   ? rows[static_cast<std::size_t>(row)].label
                   : std::string{};
    }
    return quality_rows()[static_cast<std::size_t>(row)].label;
}

/* Whether a row is the setting in force, asked per row because the quality
   list holds three independent choices at once. */
bool panel_row_is_current(int row, const player::Status &status) noexcept
{
    const auto &source = g_view.playing_source;
    if (g_view.panel_kind == 0)
    {
        if (timing_row(row) != TimingRow::none)
            return false; /* actions, not choices */
        const int chosen = row - 1;
        if (chosen < 0)
            return status.subtitle_index < 0;
        return source.subtitle_tracks[static_cast<std::size_t>(chosen)].index ==
               status.subtitle_index;
    }
    if (g_view.panel_kind == 1)
    {
        const auto rows = audio_rows(status);
        if (row < 0 || row >= static_cast<int>(rows.size()) ||
            rows[static_cast<std::size_t>(row)].header)
            return false;
        const AudioRow &entry = rows[static_cast<std::size_t>(row)];
        const jellyfin::PlaybackRequest request = player::current_request();
        if (entry.group == 0)
        {
            const jellyfin::Track *track = playing_audio_track(status);
            return track != nullptr &&
                   source.audio_tracks[static_cast<std::size_t>(entry.value)].index == track->index;
        }
        if (entry.group == 1)
            return entry.codec == request.audio_codec;
        return entry.value == request.max_audio_channels;
    }

    const QualityRow entry = quality_rows()[static_cast<std::size_t>(row)];
    if (entry.header)
        return false;
    const jellyfin::PlaybackRequest request = player::current_request();
    if (entry.group == 0)
        return kQualityBitrates[entry.value] == request.max_bitrate;
    if (entry.group == 1)
        return request.video_codec == kVideoCodecValues[entry.value];
    return false;
}

void play_adjacent_episode(int direction) noexcept;
void begin_playback(const jellyfin::Item &item, int source_index, double start_seconds) noexcept;
int starting_subtitle(const jellyfin::MediaSource &source) noexcept;

/* Headings are drawn but never landed on. */
bool panel_row_is_heading(int row) noexcept
{
    if (g_view.panel_kind == 2)
        return quality_rows()[static_cast<std::size_t>(row)].header;
    if (g_view.panel_kind == 1)
    {
        const auto rows = audio_rows(player::status());
        return row >= 0 && row < static_cast<int>(rows.size()) &&
               rows[static_cast<std::size_t>(row)].header;
    }
    return timing_row(row) == TimingRow::heading;
}

/* Which row is in force now, so the panel can mark it. */
int panel_current_row(const player::Status &status) noexcept
{
    const auto &source = g_view.playing_source;
    if (g_view.panel_kind == 0)
    {
        for (std::size_t i = 0; i < source.subtitle_tracks.size(); ++i)
            if (source.subtitle_tracks[i].index == status.subtitle_index)
                return static_cast<int>(i) + 1;
        return 0;
    }
    if (g_view.panel_kind == 1)
    {
        const auto rows = audio_rows(status);
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (!rows[i].header && panel_row_is_current(static_cast<int>(i), status))
                return static_cast<int>(i);
        return 1;
    }
    const auto rows = quality_rows();
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (!rows[i].header && panel_row_is_current(static_cast<int>(i), status))
            return static_cast<int>(i);
    return 1;
}

/* Increase timing adjustment steps for larger pending offsets. */

/* Everything except a text subtitle change needs the stream started again. */
void restart_here(const jellyfin::PlaybackRequest &changed) noexcept
{
    jellyfin::PlaybackRequest request = changed;
    request.start_seconds = player::status().position_seconds;
    (void)player::restart(request);
}

void apply_panel_choice() noexcept
{
    const player::Status status = player::status();
    const auto &source = g_view.playing_source;
    const int row = g_view.panel_index;

    if (g_view.panel_kind == 0)
    {
        const int chosen = row - 1;
        const bool text_now = !status.subtitle_burned;
        if (chosen < 0)
        {
            if (text_now)
                player::show_text_subtitle(-1);
            else
            {
                jellyfin::PlaybackRequest request = player::current_request();
                request.subtitle_index = -1;
                restart_here(request);
            }
            return;
        }
        const auto &track = source.subtitle_tracks[static_cast<std::size_t>(chosen)];
        /* A text subtitle is drawn by the app, so it can be swapped without
           interrupting the picture. An image one is burned in by the server. */
        if (track.is_text && text_now)
        {
            player::show_text_subtitle(track.index);
            return;
        }
        jellyfin::PlaybackRequest request = player::current_request();
        request.subtitle_index = track.index;
        restart_here(request);
        return;
    }
    if (g_view.panel_kind == 1)
    {
        const auto rows = audio_rows(status);
        if (row < 0 || row >= static_cast<int>(rows.size()) ||
            rows[static_cast<std::size_t>(row)].header || panel_row_is_current(row, status))
            return;
        const AudioRow &entry = rows[static_cast<std::size_t>(row)];
        jellyfin::PlaybackRequest request = player::current_request();
        if (entry.group == 0)
        {
            request.audio_index = source.audio_tracks[static_cast<std::size_t>(entry.value)].index;
            /* A new track starts from its own format and channels. */
            request.audio_codec.clear();
            request.max_audio_channels = 0;
        }
        else if (entry.group == 1)
            request.audio_codec = entry.codec;
        else
            request.max_audio_channels = entry.value;
        restart_here(request);
        return;
    }
    const QualityRow entry = quality_rows()[static_cast<std::size_t>(row)];
    if (entry.header)
        return;
    jellyfin::PlaybackRequest request = player::current_request();
    if (entry.group == 0)
        request.max_bitrate = kQualityBitrates[entry.value];
    else if (entry.group == 1)
        request.video_codec = kVideoCodecValues[entry.value];
    restart_here(request);
}

/* White on an unknown picture needs a shadow to stay readable. */
/* Keep subtitle placement stable when playback controls open. */
/* The face a style asks for, in the text renderer's terms. */
Weight subtitle_weight(const subtitle_style::Style &style) noexcept
{
    if (style.font == subtitle_style::Font::readable)
        return style.bold ? Weight::readable_bold : Weight::readable;
    return style.bold ? Weight::bold : Weight::medium;
}

/* Center subtitle lines relative to the configured floor offset. */
void draw_subtitle_block(const std::string &text, const subtitle_style::Style &style, int center_x,
                         int floor) noexcept
{
    const int size = style.pixel_size;
    const Weight weight = subtitle_weight(style);
    std::vector<std::string> lines;
    for (std::size_t at = 0; at <= text.size();)
    {
        const std::size_t stop = text.find('\n', at);
        lines.push_back(text.substr(at, stop == std::string::npos ? std::string::npos : stop - at));
        if (stop == std::string::npos)
            break;
        at = stop + 1;
    }
    const int step = style.line_step;
    const int top = (floor - style.bottom_margin) - static_cast<int>(lines.size()) * step;

    /* Draw all subtitle backgrounds before text to avoid overlapping boxes dimming earlier lines.
     */
    if (style.box_alpha > 0)
    {
        const gfx::Color fill = gfx::rgba(0x0c, 0x0c, 0x10, style.box_alpha);
        if (style.box == subtitle_style::Box::band)
        {
            int widest = 0;
            int drawn = 0;
            for (const std::string &line : lines)
                if (!line.empty())
                {
                    widest = std::max(widest, text::measure(line, size, weight));
                    ++drawn;
                }
            if (widest > 0)
            {
                const int height = drawn * step;
                gfx::rounded_rect(center_x - widest / 2 - style.box_pad_x,
                                  top + static_cast<int>(lines.size()) * step - height -
                                      style.box_rise,
                                  widest + style.box_pad_x * 2, height, size / 8, fill);
            }
        }
        else
        {
            int y = top;
            for (const std::string &line : lines)
            {
                if (line.empty())
                {
                    y += step;
                    continue;
                }
                const int width = text::measure(line, size, weight);
                gfx::rounded_rect(center_x - width / 2 - style.box_pad_x, y - style.box_rise,
                                  width + style.box_pad_x * 2, style.box_height, size / 6, fill);
                y += step;
            }
        }
    }

    const gfx::Color edge_colour = gfx::rgba(0x00, 0x00, 0x00, 0xcc);
    int y = top;
    for (const std::string &line : lines)
    {
        if (line.empty())
        {
            y += step;
            continue;
        }
        const int width = text::measure(line, size, weight);
        const int x = center_x - width / 2;
        const int e = style.edge_px;
        if (style.edge == subtitle_style::Edge::outline ||
            style.edge == subtitle_style::Edge::heavy)
        {
            const int offsets[8][2] = {{-e, 0},  {e, 0},  {0, -e}, {0, e},
                                       {-e, -e}, {e, -e}, {-e, e}, {e, e}};
            /* The thick outline fills its corners too, or the letters keep a
               bright notch at each diagonal on a light picture. */
            const int count = style.edge == subtitle_style::Edge::heavy ? 8 : 4;
            for (int i = 0; i < count; ++i)
                text::draw(x + offsets[i][0], y + offsets[i][1], line, size, weight, edge_colour);
        }
        else if (style.edge == subtitle_style::Edge::shadow)
        {
            text::draw(x + e + 1, y + e + 1, line, size, weight, edge_colour);
        }
        text::draw(x, y, line, size, weight, style.colour);
        y += step;
    }
}

void draw_subtitle_lines(const std::string &text) noexcept
{
    draw_subtitle_block(text, subtitle_style::resolve(config::current().subtitle_look),
                        gfx::kWidth / 2, gfx::kHeight);
}

/* Anchor the control popover to its triggering button. */
void draw_track_popover(const player::Status &status, int buttons_count, int first_button_x,
                        int button_size, int button_gap, int row_y) noexcept
{
    static constexpr const char *kTitles[3] = {"Subtitles", "Audio", "Quality"};
    constexpr int kPopW = 560;
    constexpr int kRow = 52;
    constexpr int kHeader = 52;

    const int rows = panel_rows();
    const int visible = std::min(rows, 7);
    const int height = kHeader + visible * kRow + 18;
    const int anchor = std::clamp(g_view.panel_anchor, 0, std::max(0, buttons_count - 1));
    const int anchor_right = first_button_x + anchor * (button_size + button_gap) + button_size;
    const int px = std::clamp(anchor_right - kPopW, kSafeX, gfx::kWidth - kSafeX - kPopW);
    const int py = row_y - 18 - height;

    gfx::rounded_rect(px, py, kPopW, height, 24, gfx::rgba(0x16, 0x16, 0x1b, 0xf4));
    text::draw(px + 28, py + 14, kTitles[g_view.panel_kind], 24, Weight::medium,
               gfx::palette::text_dim);

    const int first = std::max(0, std::min(g_view.panel_index - visible + 2, rows - visible));
    int y = py + kHeader;
    for (int row = first; row < rows && row < first + visible; ++row)
    {
        const bool header = panel_row_is_heading(row);
        if (header && timing_row(row) == TimingRow::heading)
        {
            /* The state is what is being watched while adjusting, so it reads
               brighter than a heading, and in the accent once it has moved. */
            text::draw(px + 28, y + 10, "Timing", 20, Weight::medium, gfx::palette::text_faint);
            const bool original = subtitle_timing::is_original(g_view.subtitle_offset);
            text::draw(px + 28 + text::measure("Timing", 20, Weight::medium) + 16, y + 8,
                       subtitle_timing::describe(g_view.subtitle_offset), 23, Weight::medium,
                       original ? gfx::palette::text_dim : gfx::palette::accent);
            y += kRow - 10;
            continue;
        }
        if (header)
        {
            text::draw(px + 28, y + 10, panel_row_label(row), 20, Weight::medium,
                       gfx::palette::text_faint);
            y += kRow - 10;
            continue;
        }
        const bool focused = row == g_view.panel_index;
        if (focused)
            gfx::rounded_rect(px + 14, y - 4, kPopW - 28, kRow - 6, 12,
                              gfx::rgba(0xff, 0xff, 0xff, 0x18));
        if (panel_row_is_current(row, status))
            icons::draw(icons::Icon::check, px + 26, y + 6, 22, gfx::palette::accent);
        text::draw_ellipsized(px + 62, y + 4, kPopW - 96, panel_row_label(row), 25,
                              focused ? Weight::medium : Weight::regular,
                              focused ? gfx::palette::text : gfx::palette::text_dim);
        y += kRow;
    }
}

/*
 * What the player's title block should say about an item: the show or film, the
 * episode line under it, and whichever item owns the show's lettering.
 */
void set_playing_title(const jellyfin::Item &item) noexcept
{
    g_view.playing_series = item.series_name.empty() ? item.name : item.series_name;
    g_view.playing_episode.clear();
    if (item.type == "Episode" && (item.parent_index_number > 0 || item.index_number > 0))
    {
        g_view.playing_episode = "S" + std::to_string(item.parent_index_number) + ":E" +
                                 std::to_string(item.index_number);
        if (!item.name.empty())
            g_view.playing_episode += "  \xe2\x80\x94  " + item.name;
    }
    /* A series carries the lettering more often than an episode does. */
    g_view.playing_logo_id.clear();
    g_view.playing_logo_tag.clear();
    if (!item.parent_logo_tag.empty())
    {
        g_view.playing_logo_id = item.parent_logo_id.empty() ? item.series_id : item.parent_logo_id;
        g_view.playing_logo_tag = item.parent_logo_tag;
    }
    if (g_view.playing_logo_id.empty() && !item.logo_tag.empty())
    {
        g_view.playing_logo_id = item.id;
        g_view.playing_logo_tag = item.logo_tag;
    }
}

/* Indicate playback buffering without replacing the last picture. */
void draw_player_loading(const player::Status &status) noexcept
{
    const int cx = gfx::kWidth / 2;
    const int cy = gfx::kHeight / 2;

    if (status.state == player::State::failed)
    {
        /* A ring with a gap in it, which reads as stopped rather than busy. */
        gfx::arc(cx, cy - 40, 30, 4, 0.0f, 1.0f, gfx::rgba(0xcc, 0x44, 0x44, 0xc0));
        gfx::rounded_rect(cx - 2, cy - 58, 4, 22, 2, gfx::rgb(0xcc, 0x44, 0x44));
        gfx::rounded_rect(cx - 2, cy - 28, 4, 4, 2, gfx::rgb(0xcc, 0x44, 0x44));
        const std::string &line =
            status.message.empty() ? std::string{"Playback failed"} : status.message;
        const int width = text::measure(line, 26, Weight::regular);
        text::draw(cx - width / 2, cy + 16, line, 26, Weight::regular, gfx::rgb(0xd4, 0xd4, 0xdc));
        return;
    }

    /* Just under a turn a second, which is slow enough to look deliberate. */
    const float head = static_cast<float>(g_view.frames % 54) / 54.0f;
    gfx::arc(cx, cy, 30, 4, 0.0f, 1.0f, gfx::rgba(0xff, 0xff, 0xff, 0x24));
    gfx::arc(cx, cy, 30, 4, head, 0.3f, gfx::rgba(0xff, 0xff, 0xff, 0xe6));

    /* A seek is a restart, and saying so is noise: the scrubber has it. */
    if (g_view.seek_restart || status.message.empty())
        return;
    const int width = text::measure(status.message, 24, Weight::regular);
    text::draw(cx - width / 2, cy + 58, status.message, 24, Weight::regular,
               gfx::rgb(0x9a, 0x9a, 0xa4));
}

/* The console's own clock, as hours and minutes, `ahead` seconds from now. */
std::string time_of_day(double ahead) noexcept
{
    struct
    {
        std::int64_t seconds;
        std::int64_t microseconds;
    } stamp{};
    if (sceKernelGettimeofday(&stamp) != 0)
        return {};
    struct
    {
        int minutes_west;
        int dst;
    } zone{};
    if (sceKernelGettimezone(&zone) != 0)
        zone.minutes_west = 0;

    std::int64_t local = stamp.seconds - static_cast<std::int64_t>(zone.minutes_west) * 60 +
                         static_cast<std::int64_t>(ahead);
    local %= 86400;
    if (local < 0)
        local += 86400;
    int hour = static_cast<int>(local / 3600);
    const int minute = static_cast<int>((local / 60) % 60);
    /* Twelve-hour, because that is what the console's own clock shows. */
    hour %= 12;
    if (hour == 0)
        hour = 12;
    char out[16];
    std::snprintf(out, sizeof(out), "%d:%02d", hour, minute);
    return std::string{out};
}

/* Show the series and episode title separately. */
void draw_player_title(const player::Status &status) noexcept
{
    const gfx::Bitmap *logo = nullptr;
    if (!g_view.playing_logo_id.empty())
        logo = images::acquire(g_view.playing_logo_id, g_view.playing_logo_tag, images::Kind::logo,
                               110);

    /* A wash at the top, so lettering and clock hold up over a bright scene. */
    gfx::vertical_gradient(0, 0, gfx::kWidth, 300, gfx::rgba(0x00, 0x00, 0x00, 0xb4),
                           gfx::rgba(0x00, 0x00, 0x00, 0x00));

    int y = kSafeY + 14;
    if (logo != nullptr && logo->valid())
    {
        /* Its own shape, inside a box: wide lettering is limited by width and
           tall lettering by height, and neither is ever stretched. */
        constexpr int kMaxH = 110;
        constexpr int kMaxW = 760;
        int h = kMaxH;
        int w = (h * logo->width) / std::max(1, logo->height);
        if (w > kMaxW)
        {
            w = kMaxW;
            h = (w * logo->height) / std::max(1, logo->width);
        }
        gfx::blit_scaled(*logo, kSafeX, y, w, h);
        y += h + 10;
    }
    else if (!g_view.playing_series.empty())
    {
        text::draw_ellipsized(kSafeX, y, 1100, g_view.playing_series, 56, Weight::bold,
                              gfx::palette::text);
        y += 74;
    }

    if (!g_view.playing_episode.empty())
        text::draw_ellipsized(kSafeX, y, 1100, g_view.playing_episode, 30, Weight::medium,
                              gfx::rgb(0xd0, 0xd0, 0xd8));

    if (const std::string now = time_of_day(0.0); !now.empty())
    {
        const int width = text::measure(now, 30, Weight::medium);
        text::draw(gfx::kWidth - kSafeX - width, kSafeY + 16, now, 30, Weight::medium,
                   gfx::rgb(0xd0, 0xd0, 0xd8));
    }
    (void)status;
}

/* Draw playback controls independently from transient loading overlays. */
/* Offer sibling navigation only when a previous or next episode exists. */
constexpr int kEpisodeButtonW = 268;
constexpr int kEpisodeButtonH = 60;
constexpr int kEpisodeGap = 20;

void draw_episode_buttons(int y, bool focused) noexcept
{
    struct Choice
    {
        bool offered;
        icons::Icon icon;
        const char *label;
    };
    const Choice choices[2] = {
        {g_view.has_previous, icons::Icon::chevron_left, "Previous episode"},
        {g_view.has_next, icons::Icon::chevron_right, "Next episode"},
    };
    if (!choices[0].offered && !choices[1].offered)
        return;

    /* Pack available transport buttons without empty slots. */
    int x = kSafeX;
    for (int i = 0; i < 2; ++i)
    {
        if (!choices[i].offered)
            continue;
        const bool on = focused && g_view.episode_choice == i;

        if (on)
        {

            gfx::rounded_rect(x, y, kEpisodeButtonW, kEpisodeButtonH, kEpisodeButtonH / 2,
                              gfx::palette::text);
        }
        else
        {
            gfx::rounded_rect(x, y, kEpisodeButtonW, kEpisodeButtonH, kEpisodeButtonH / 2,
                              gfx::rgba(0xff, 0xff, 0xff, 0x1c));
        }

        const gfx::Color ink = on ? gfx::rgb(0x10, 0x10, 0x14) : gfx::palette::text;
        const int label_w = text::measure(choices[i].label, 23, Weight::medium);
        const int content = 26 + 10 + label_w;
        const int left = x + (kEpisodeButtonW - content) / 2;
        icons::draw(choices[i].icon, i == 0 ? left : left + label_w + 10, y + 17, 26, ink);
        text::draw(i == 0 ? left + 36 : left, y + 17, choices[i].label, 23, Weight::medium, ink);
        x += kEpisodeButtonW + kEpisodeGap;
    }

    if (g_view.changing_episode)
        text::draw(x + 8, y + 18, "Loading", 22, Weight::regular, gfx::palette::text_dim);
}

void draw_player_controls(const player::Status &status) noexcept
{
    gfx::vertical_gradient(0, gfx::kHeight - 560, gfx::kWidth, 220,
                           gfx::rgba(0x00, 0x00, 0x00, 0x00), gfx::rgba(0x00, 0x00, 0x00, 0xbb));
    gfx::fill_rect(0, gfx::kHeight - 340, gfx::kWidth, 340, gfx::rgba(0x00, 0x00, 0x00, 0xbb));

    const std::vector<PlayerButton> buttons = player_buttons();
    constexpr int kButton = 72;
    constexpr int kButtonGap = 16;
    const int row_y = kTransportRow;
    /* The badge draws at kSafeX, so the title starts clear of it. */
    constexpr int kTitleX = kSafeX + kTransportBadge + 26;
    const bool on_modules = g_view.osd_focus == View::OsdFocus::modules;
    const int buttons_w = static_cast<int>(buttons.size()) * (kButton + kButtonGap) - kButtonGap;
    int bx = gfx::kWidth - kSafeX - buttons_w;
    const int first_button_x = bx;

    /* Indicate the module row and its directional-input ownership. */
    {
        const int hint = 26;
        const int hint_x = first_button_x - hint - 30;
        const int hint_y = row_y + (kButton - hint) / 2;
        if (on_modules)
        {
            /* A disc the size of the buttons it belongs to, so the lit state
               joins the row rather than sitting beside it. */
            const int disc = kButton - 12;
            gfx::rounded_rect(hint_x - (disc - hint) / 2, row_y + (kButton - disc) / 2, disc, disc,
                              disc / 2, gfx::with_alpha(icons::button::triangle, 0x33));
            icons::draw(icons::Icon::ps_triangle, hint_x, hint_y, hint, icons::button::triangle);
        }
        else
        {
            icons::draw(icons::Icon::ps_triangle, hint_x, hint_y, hint,
                        gfx::with_alpha(icons::button::triangle, 0xaa));
        }
    }

    for (std::size_t i = 0; i < buttons.size(); ++i)
    {
        /* The button that owns an open popover stays lit, so the two read as
           one control rather than a box that appeared from nowhere. */
        const bool owns_popover = g_view.panel_open && g_view.panel_anchor == static_cast<int>(i);
        const bool focused =
            (on_modules && g_view.osd_button == static_cast<int>(i) && !g_view.panel_open) ||
            owns_popover;
        const bool lit = (buttons[i].action == PlayerAction::debug && g_view.show_debug) ||
                         (buttons[i].action == PlayerAction::autoplay &&
                          config::autoplay_enabled(g_view.playing_item.series_id));
        gfx::rounded_rect(bx, row_y, kButton, kButton, kButton / 2,
                          focused ? gfx::palette::focus_surface
                          : lit   ? gfx::palette::accent
                                  : gfx::rgba(0xff, 0xff, 0xff, 0x22));
        if (focused)
            gfx::stroke_rounded_rect(bx, row_y, kButton, kButton, kButton / 2, 2,
                                     gfx::palette::focus_border);
        icons::draw(buttons[i].icon, bx + 20, row_y + 20, 32, gfx::palette::text);
        bx += kButton + kButtonGap;
    }
    if (on_modules && !g_view.panel_open && !buttons.empty())
    {
        const PlayerButton &selected = buttons[static_cast<std::size_t>(
            std::clamp(g_view.osd_button, 0, static_cast<int>(buttons.size()) - 1))];
        if (selected.action == PlayerAction::autoplay)
        {
            const std::string label = config::autoplay_enabled(g_view.playing_item.series_id)
                                          ? "Autoplay  On for this series"
                                          : "Autoplay  Off for this series";
            const int width = text::measure(label, 21, Weight::medium);
            text::draw(gfx::kWidth - kSafeX - width, row_y - 36, label, 21, Weight::medium,
                       gfx::rgba(0xff, 0xff, 0xff, 0xcc));
        }
    }

    /* Distinguish the scrub target from the current playback position. */
    const bool scrubbing = g_view.seek_target >= 0.0;
    const double aimed = scrubbing ? g_view.seek_target : status.position_seconds;
    const double shown = status.position_seconds;
    const int track_x = kSafeX;
    const int track_w = gfx::kWidth - kSafeX * 2;
    const int track_y = gfx::kHeight - 212;
    const bool scrub_focus = g_view.osd_focus == View::OsdFocus::bar && !g_view.panel_open;
    const int thickness = scrub_focus ? 8 : 6;
    gfx::rounded_rect(track_x, track_y, track_w, thickness, thickness / 2,
                      gfx::rgba(0xff, 0xff, 0xff, 0x30));
    if (status.duration_seconds > 0.0)
    {
        const double fraction = std::clamp(aimed / status.duration_seconds, 0.0, 1.0);
        const int filled = static_cast<int>(track_w * fraction);
        gfx::rounded_rect(track_x, track_y, filled, thickness, thickness / 2, gfx::palette::text);
        if (scrub_focus || scrubbing)
        {
            /* A circle on the line, not a rounded slab standing on it. */
            const int radius = scrubbing ? 11 : 9;
            gfx::rounded_rect(track_x + filled - radius, track_y + thickness / 2 - radius,
                              radius * 2, radius * 2, radius, gfx::palette::text);
        }
    }

    if (scrubbing && status.duration_seconds > 0.0)
    {
        /* The pending jump, so a run of presses can be seen building up. */
        const double delta = g_view.seek_pending;
        const std::string label =
            (delta < 0.0 ? "-" : "+") + clock_text(delta < 0.0 ? -delta : delta);
        const int width = text::measure(label, 26, Weight::medium) + 32;
        const double fraction = std::clamp(aimed / status.duration_seconds, 0.0, 1.0);
        const int centre = track_x + static_cast<int>(track_w * fraction);
        const int bubble_x = std::clamp(centre - width / 2, track_x, track_x + track_w - width);
        gfx::rounded_rect(bubble_x, track_y - 62, width, 44, 22, gfx::palette::focus_surface);
        gfx::stroke_rounded_rect(bubble_x, track_y - 62, width, 44, 22, 2,
                                 gfx::palette::focus_border);
        text::draw(bubble_x + 16, track_y - 52, label, 26, Weight::medium, gfx::palette::text);
    }

    {
        const std::string now = clock_text(shown);
        int at = track_x;
        text::draw(at, track_y + 26, now, 24, Weight::medium, gfx::rgba(0xff, 0xff, 0xff, 0xee));
        at += text::measure(now, 24, Weight::medium);
        if (status.duration_seconds > 0.0)
        {
            /* The separator and the total a shade back, so the eye lands on
               the number that changes. */
            text::draw(at + 9, track_y + 26, "/", 24, Weight::regular,
                       gfx::rgba(0xff, 0xff, 0xff, 0x6e));
            text::draw(at + 24, track_y + 26, clock_text(status.duration_seconds), 24,
                       Weight::regular, gfx::rgba(0xff, 0xff, 0xff, 0x9e));
        }
    }
    if (status.duration_seconds > 0.0)
    {
        /* When it lands, which is the one a viewer deciding whether to start
           it actually wants. The count of what is left went to the left-hand
           pair, where it is the same fact stated more usefully. */
        const std::string ends = time_of_day(std::max(0.0, status.duration_seconds - shown));
        if (!ends.empty())
        {
            const std::string label = "Ends at " + ends;
            text::draw(track_x + track_w - text::measure(label, 24, Weight::medium), track_y + 26,
                       label, 24, Weight::medium, gfx::rgba(0xff, 0xff, 0xff, 0xbb));
        }
    }

    /* Under the timeline, where "down" now goes. */
    draw_episode_buttons(gfx::kHeight - 132,
                         g_view.osd_focus == View::OsdFocus::episodes && !g_view.panel_open);

    if (status.paused && !scrubbing)
        text::draw(kTitleX, row_y + 26, "Paused", 24, Weight::medium,
                   gfx::rgba(0xff, 0xff, 0xff, 0xdd));

    if (g_view.panel_open)
        draw_track_popover(status, static_cast<int>(buttons.size()), first_button_x, kButton,
                           kButtonGap, row_y);
}

/* Display transport state beside the title. */
void draw_transport_badge(const player::Status &status, int y) noexcept
{
    const bool with_controls = g_view.osd_visible || g_view.panel_open;
    const int fade = with_controls ? 255 : std::min(255, g_view.flash * 10);
    if (fade <= 0)
        return;

    if (g_view.flash > 0)
    {
        /* 0 at the press, 1 once the ring has finished travelling. */
        const float t = 1.0f - static_cast<float>(g_view.flash) / 40.0f;
        const int grow = static_cast<int>(t * 30.0f);
        const int ring_size = kTransportBadge + grow * 2;
        const auto ring =
            static_cast<std::uint8_t>((1.0f - t) * 170.0f * static_cast<float>(fade) / 255.0f);
        gfx::stroke_rounded_rect(kSafeX - grow, y - grow, ring_size, ring_size, ring_size / 2, 3,
                                 gfx::rgba(0xff, 0xff, 0xff, ring));
    }

    gfx::rounded_rect(kSafeX, y, kTransportBadge, kTransportBadge, kTransportBadge / 2,
                      gfx::rgba(0x00, 0x00, 0x00, static_cast<std::uint8_t>(fade * 5 / 9)));
    gfx::rounded_rect(kSafeX, y, kTransportBadge, kTransportBadge, kTransportBadge / 2,
                      gfx::rgba(0xff, 0xff, 0xff, static_cast<std::uint8_t>(fade / 8)));
    /* The glyph is the action, as on every other player: a triangle while
       paused means cross will resume. */
    icons::draw(status.paused ? icons::Icon::play : icons::Icon::pause, kSafeX + 25, y + 25, 26,
                gfx::rgba(0xff, 0xff, 0xff, static_cast<std::uint8_t>(fade)));
}

/* Defined with the player's input, which is where the machine is driven. */
void advance_autoplay(const player::Status &status) noexcept;

/* A deliberately small playback affordance: it should be noticed, not become
   a second OSD. Cross performs the jump; Circle can quietly dismiss it. */
void draw_skip_intro() noexcept
{
    const float t = std::clamp(g_view.intro_in.value(), 0.0f, 1.0f);
    constexpr int kW = 300;
    constexpr int kH = 76;
    constexpr int kRadius = 20;
    const int x = kContentRight - kW;
    const int base_y = g_view.osd_visible ? kTransportRow - kH - 76 : gfx::kHeight - 154;
    const int y = base_y + static_cast<int>((1.0f - t) * 18.0f);
    const auto fade = [t](std::uint8_t a) { return static_cast<std::uint8_t>(a * t); };

    gfx::drop_shadow(x, y, kW, kH, kRadius, 18, fade(0x78));
    gfx::rounded_rect(x, y, kW, kH, kRadius, gfx::rgba(0x0c, 0x0d, 0x12, fade(0xea)));
    gfx::horizontal_gradient(x, y, kW, kH, gfx::rgba(0x00, 0xa4, 0xdc, fade(0x1e)),
                             gfx::rgba(0x0c, 0x0d, 0x12, fade(0x00)));
    gfx::stroke_rounded_rect(x, y, kW, kH, kRadius, 1, gfx::rgba(0xff, 0xff, 0xff, fade(0x24)));
    gfx::stroke_rounded_rect(x, y, kW, kH, kRadius, 2, gfx::rgba(0x62, 0xd5, 0xf4, fade(0xd8)));
    const auto button = g_view.osd_visible ? icons::Icon::ps_square : icons::Icon::ps_cross;
    icons::draw(button, x + 20, y + 25, 26, gfx::with_alpha(ui::hint_colour(button), fade(0xff)));
    text::draw(x + 62, y + 12, "Skip intro", 27, Weight::bold,
               gfx::rgba(0xff, 0xff, 0xff, fade(0xf2)));
    const int seconds =
        std::max(1, static_cast<int>(std::ceil(g_view.playing_item.intro_end_seconds -
                                               player::status().position_seconds)));
    text::draw(x + 62, y + 46,
               g_view.intro_seek_failed ? "Could not skip. Try again."
                                        : std::to_string(seconds) + " seconds remaining",
               18, Weight::regular, gfx::rgba(0xc2, 0xd8, 0xe0, fade(0xff)));
}

/* Offer the next episode over closing credits. */
void draw_up_next_card() noexcept
{
    const jellyfin::Item &next = g_view.up_next;
    constexpr int kCardW = 620;
    constexpr int kStill = 200;
    constexpr int kCardH = 192;
    const int x = kContentRight - kCardW;
    const int y = gfx::kHeight - 280;

    /* It arrives from below rather than appearing. */
    const float t = std::clamp(g_view.up_next_in.value(), 0.0f, 1.0f);
    const int lift = static_cast<int>((1.0f - t) * 40.0f);
    const auto fade = [t](std::uint8_t a) { return static_cast<std::uint8_t>(a * t); };
    const int top = y + lift;

    gfx::drop_shadow(x, top, kCardW, kCardH, 20, 24, fade(0x88));
    gfx::rounded_rect(x, top, kCardW, kCardH, 20, gfx::rgba(0x12, 0x12, 0x16, fade(0xf2)));

    const bool borrowed =
        next.image_tag.empty() && !next.series_id.empty() && !next.series_primary_tag.empty();
    const std::string still_id = borrowed ? next.series_id : next.id;
    const std::string still_tag = borrowed ? next.series_primary_tag : next.image_tag;
    const gfx::Bitmap *still =
        still_id.empty() ? nullptr
                         : images::acquire(still_id, still_tag, images::Kind::primary, kStill);
    const int picture_x = x + 16;
    const int picture_y = top + 16;
    const int picture_h = kCardH - 32;
    if (still != nullptr)
        gfx::blit_cover(*still, picture_x, picture_y, kStill, picture_h, 12, fade(0xff));
    else
        gfx::rounded_rect(picture_x, picture_y, kStill, picture_h, 12,
                          gfx::rgba(0xff, 0xff, 0xff, fade(0x14)));

    const int text_x = picture_x + kStill + 20;
    const int text_w = kCardW - (text_x - x) - 24;
    const bool counting = g_view.autoplay.counting();
    const int seconds = static_cast<int>(std::ceil(g_view.autoplay.seconds_left()));
    const std::string heading = counting ? "Next episode in " + std::to_string(std::max(1, seconds))
                                         : std::string{"Up next"};
    text::draw(text_x, top + 20, heading, 22, Weight::medium,
               gfx::rgba(0xff, 0xff, 0xff, fade(0xb0)));
    std::string title = next.name;
    if (next.parent_index_number > 0)
        title = "S" + std::to_string(next.parent_index_number) + ":E" +
                std::to_string(next.index_number) + "  " + next.name;
    text::draw_ellipsized(text_x, top + 52, text_w, title, 27, Weight::bold,
                          gfx::rgba(0xff, 0xff, 0xff, fade(0xff)));

    /* The countdown, as a bar that fills. */
    const int bar_y = top + 96;
    gfx::rounded_rect(text_x, bar_y, text_w, 6, 3, gfx::rgba(0xff, 0xff, 0xff, fade(0x24)));
    if (counting)
    {
        const float left =
            static_cast<float>(g_view.autoplay.seconds_left() / autoplay::kCountdown);
        const int filled = static_cast<int>(text_w * std::clamp(1.0f - left, 0.0f, 1.0f));
        gfx::rounded_rect(text_x, bar_y, filled, 6, 3, gfx::palette::accent);
    }
    (void)draw_button_hints(text_x, top + kCardH - 58,
                            {{icons::Icon::ps_cross, counting ? "Play now" : "Play"},
                             {icons::Icon::ps_circle, "Not now"}});
}

/* Pause autoplay until the viewer confirms continued watching. */
void draw_still_watching() noexcept
{
    /* The artwork of whatever is playing, dimmed almost to black. */
    const jellyfin::Item &item = g_view.playing_item;
    const gfx::Bitmap *art = nullptr;
    if (!item.backdrop_tag.empty())
        art = images::acquire(item.id, item.backdrop_tag, images::Kind::backdrop, 1080);
    if (art == nullptr && !item.parent_backdrop_tag.empty())
    {
        const std::string owner =
            item.parent_backdrop_id.empty() ? item.series_id : item.parent_backdrop_id;
        if (!owner.empty())
            art = images::acquire(owner, item.parent_backdrop_tag, images::Kind::backdrop, 1080);
    }
    if (art != nullptr)
        gfx::blit_cover(*art, 0, 0, gfx::kWidth, gfx::kHeight, 0, 255);
    else
        gfx::fill_rect(0, 0, gfx::kWidth, gfx::kHeight, gfx::rgb(0x08, 0x08, 0x0b));
    gfx::fill_rect(0, 0, gfx::kWidth, gfx::kHeight,
                   gfx::rgba(0x00, 0x00, 0x00, art != nullptr ? 0xdc : 0xa0));

    const int centre = gfx::kWidth / 2;
    const std::string show = g_view.playing_series.empty() ? item.name : g_view.playing_series;
    const std::string heading = "Are you still watching?";
    text::draw(centre - text::measure(heading, 64, Weight::bold) / 2, 300, heading, 64,
               Weight::bold, gfx::palette::text);
    if (!show.empty())
    {
        const int w = text::measure(show, 34, Weight::regular);
        text::draw(centre - w / 2, 396, show, 34, Weight::regular,
                   gfx::rgba(0xff, 0xff, 0xff, 0xa8));
    }

    /* One filled button, and a quieter way out beside it. */
    static constexpr const char *kLabels[2] = {"Continue watching", "Stop"};
    int widths[2] = {0, 0};
    int total = 0;
    for (int i = 0; i < 2; ++i)
    {
        widths[i] = text::measure(kLabels[i], 30, Weight::medium) + 96;
        total += widths[i];
    }
    total += 24;
    int bx = centre - total / 2;
    const int by = 520;
    constexpr int kH = 76;
    for (int i = 0; i < 2; ++i)
    {
        const bool lit = i == g_view.ask_choice;
        if (lit)
            gfx::drop_shadow(bx, by, widths[i], kH, kH / 2, 18, 0x80);
        gfx::rounded_rect(bx, by, widths[i], kH, kH / 2,
                          lit ? gfx::palette::focus_surface : gfx::palette::control);
        gfx::stroke_rounded_rect(bx, by, widths[i], kH, kH / 2, lit ? 2 : 1,
                                 lit ? gfx::palette::focus_border : gfx::palette::control_border);
        text::draw(bx + (widths[i] - text::measure(kLabels[i], 30, Weight::medium)) / 2, by + 22,
                   kLabels[i], 30, Weight::medium, gfx::palette::text);
        /* The countdown runs under the button that is waiting to be pressed. */
        if (lit && i == 0)
        {
            const float left = static_cast<float>(g_view.autoplay.ask_seconds_left() /
                                                  autoplay::State::kAskTimeout);
            const int track = widths[i];
            gfx::rounded_rect(bx, by + kH + 14, track, 4, 2, gfx::rgba(0xff, 0xff, 0xff, 0x1c));
            gfx::rounded_rect(bx, by + kH + 14,
                              static_cast<int>(track * std::clamp(left, 0.0f, 1.0f)), 4, 2,
                              gfx::rgba(0xff, 0xff, 0xff, 0x66));
        }
        bx += widths[i] + 24;
    }
    const std::string note =
        "Playback stops in " +
        std::to_string(static_cast<int>(std::ceil(g_view.autoplay.ask_seconds_left()))) +
        " seconds";
    text::draw(centre - text::measure(note, 22, Weight::regular) / 2, by + kH + 40, note, 22,
               Weight::regular, gfx::palette::text_faint);
}

/* Retain artwork and available actions after playback ends. */
struct EndChoice
{
    enum class Kind : unsigned char
    {
        play_next,
        again,
        leave
    } kind;
    const char *label;
};

std::vector<EndChoice> end_choices() noexcept
{
    std::vector<EndChoice> out;
    if (g_view.has_next && !g_view.up_next.id.empty())
        out.push_back({EndChoice::Kind::play_next, "Play next episode"});
    out.push_back({EndChoice::Kind::again, "Watch again"});
    out.push_back({EndChoice::Kind::leave, "Leave"});
    return out;
}

void draw_end_card() noexcept
{
    const bool episode = g_view.has_next && !g_view.up_next.id.empty();
    const jellyfin::Item &subject = episode ? g_view.up_next : g_view.playing_item;

    /* The artwork fills the screen, the way the services end on the next
       thing rather than on nothing. */
    const gfx::Bitmap *art = nullptr;
    if (!subject.backdrop_tag.empty())
        art = images::acquire(subject.id, subject.backdrop_tag, images::Kind::backdrop, 1080);
    if (art == nullptr && !subject.parent_backdrop_tag.empty())
    {
        const std::string owner =
            subject.parent_backdrop_id.empty() ? subject.series_id : subject.parent_backdrop_id;
        if (!owner.empty())
            art = images::acquire(owner, subject.parent_backdrop_tag, images::Kind::backdrop, 1080);
    }
    if (art == nullptr && !g_view.playing_item.backdrop_tag.empty())
        art = images::acquire(g_view.playing_item.id, g_view.playing_item.backdrop_tag,
                              images::Kind::backdrop, 1080);
    if (art != nullptr)
        gfx::blit_cover(*art, 0, 0, gfx::kWidth, gfx::kHeight, 0, 255);
    else
        gfx::fill_rect(0, 0, gfx::kWidth, gfx::kHeight, gfx::rgb(0x0b, 0x0b, 0x0f));
    /* Dark at the left where the words go, clearer at the right. */
    gfx::horizontal_gradient(0, 0, gfx::kWidth, gfx::kHeight, gfx::rgba(0x08, 0x08, 0x0c, 0xf4),
                             gfx::rgba(0x08, 0x08, 0x0c, 0x66));
    gfx::vertical_gradient(0, gfx::kHeight - 420, gfx::kWidth, 420,
                           gfx::rgba(0x08, 0x08, 0x0c, 0x00), gfx::rgba(0x08, 0x08, 0x0c, 0xe0));

    int y = 300;
    const std::string label = episode ? "Next episode" : "Finished";
    text::draw(kSafeX, y, label, 26, Weight::medium, gfx::palette::accent_alt);
    y += 46;
    std::string title = subject.name;
    if (episode && subject.parent_index_number > 0)
        title = "S" + std::to_string(subject.parent_index_number) + ":E" +
                std::to_string(subject.index_number) + "  " + subject.name;
    text::draw_ellipsized(kSafeX, y, 1100, episode ? title : g_view.playing_item.name, 56,
                          Weight::bold, gfx::palette::text);
    y += 86;
    if (episode && !subject.overview.empty())
    {
        (void)text::draw_wrapped(kSafeX, y, 980, 3, 36, subject.overview, 23, Weight::regular,
                                 gfx::rgba(0xff, 0xff, 0xff, 0xc0));
        y += 120;
    }
    else if (!episode)
    {
        /* A film has no series to name, and repeating its own title under
           itself is what the first cut of this did. Say something useful
           instead: how long it was, and what year it is from. */
        const jellyfin::Item &film = g_view.playing_item;
        std::string words;
        if (film.runtime_minutes > 0)
        {
            const int hours = film.runtime_minutes / 60;
            const int minutes = film.runtime_minutes % 60;
            words = hours > 0 ? std::to_string(hours) + "h " + std::to_string(minutes) + "m"
                              : std::to_string(minutes) + "m";
        }
        if (film.production_year > 0)
            words += (words.empty() ? "" : "  \xc2\xb7  ") + std::to_string(film.production_year);
        if (!words.empty())
        {
            text::draw(kSafeX, y, words, 26, Weight::regular, gfx::rgba(0xff, 0xff, 0xff, 0xa8));
            y += 60;
        }
        else
            y += 20;
    }

    const std::vector<EndChoice> choices = end_choices();
    int bx = kSafeX;
    const int by = y + 20;
    constexpr int kH = 72;
    for (std::size_t i = 0; i < choices.size(); ++i)
    {
        const bool lit = static_cast<int>(i) == g_view.end_choice;
        const int w = text::measure(choices[i].label, 28, Weight::medium) + 84;
        if (lit)
            gfx::drop_shadow(bx, by, w, kH, kH / 2, 18, 0x80);
        gfx::rounded_rect(bx, by, w, kH, kH / 2,
                          lit ? gfx::palette::focus_surface : gfx::palette::control);
        gfx::stroke_rounded_rect(bx, by, w, kH, kH / 2, lit ? 2 : 1,
                                 lit ? gfx::palette::focus_border : gfx::palette::control_border);
        text::draw(bx + (w - text::measure(choices[i].label, 28, Weight::medium)) / 2, by + 20,
                   choices[i].label, 28, Weight::medium, gfx::palette::text);
        bx += w + 18;
    }
    /* No "Cross to choose": a lit button says that already. The way back is
       the one thing worth spelling out, and it sits bottom right, where
       nothing else is.  */
    const int hint = 22 + 9 + text::measure("Back", 20, Weight::regular);
    (void)draw_button_hints(kContentRight - hint, gfx::kHeight - 84,
                            {{icons::Icon::ps_circle, "Back"}});
}

void draw_player() noexcept
{
    const gfx::Bitmap *picture = player::frame();
    const player::Status status = player::status();
    note_subtitle_state(status);
    /* Driven from the drawing, not the input: prompts must keep their timing
       even while no controls are visible. */
    advance_autoplay(status);
    const bool intro_range =
        g_view.playing_item.intro_start_seconds >= 0.0 &&
        g_view.playing_item.intro_end_seconds > g_view.playing_item.intro_start_seconds;
    g_view.intro_visible = intro_range && !g_view.intro_dismissed &&
                           status.state == player::State::playing &&
                           status.position_seconds >= g_view.playing_item.intro_start_seconds &&
                           status.position_seconds < g_view.playing_item.intro_end_seconds;
    /* The title's own timing follows it; a new title brings its own. */
    if (const std::string item = player::current_request().item_id;
        item != g_view.subtitle_offset_item)
    {
        commit_subtitle_offset();
        g_view.subtitle_offset_item = item;
        g_view.subtitle_offset = config::subtitle_offset(item);
    }
    const std::string cue = subtitles::current(
        subtitle_timing::cue_clock(status.position_seconds, g_view.subtitle_offset));
    static long last_shown = -1;
    static bool last_clean = false;
    /* A frame is only worth reusing when nothing is drawn over the picture --
       the up-next card and the still-watching question included, or they
       would never appear at all.  */
    const bool clean = picture && status.state == player::State::playing && !status.paused &&
                       !g_view.osd_visible && !g_view.panel_open && !g_view.show_debug &&
                       cue.empty() && g_view.flash == 0 && !gfx::hdr_request_pending() &&
                       g_view.up_next_in.value() <= 0.004f && g_view.intro_in.value() <= 0.004f &&
                       !g_view.autoplay.asking();
    const bool reuse = clean && last_clean && status.shown == last_shown;
    last_clean = clean;
    last_shown = status.shown;
    if (reuse)
    {
        gfx::reuse_frame();
        return;
    }
    if (picture != nullptr)
    {
        /* Letterbox: fit inside the screen without cropping or stretching. */
        const int by_width = (gfx::kWidth * picture->height) / picture->width;
        int draw_w = gfx::kWidth;
        int draw_h = by_width;
        if (draw_h > gfx::kHeight)
        {
            draw_h = gfx::kHeight;
            draw_w = (gfx::kHeight * picture->width) / picture->height;
        }
        const int left = (gfx::kWidth - draw_w) / 2;
        const int top = (gfx::kHeight - draw_h) / 2;
        const auto black = gfx::rgb(0, 0, 0);
        gfx::fill_rect(0, 0, gfx::kWidth, top, black);
        gfx::fill_rect(0, top + draw_h, gfx::kWidth, gfx::kHeight - top - draw_h, black);
        gfx::fill_rect(0, top, left, draw_h, black);
        gfx::fill_rect(left + draw_w, top, gfx::kWidth - left - draw_w, draw_h, black);
        gfx::blit_video(*picture, left, top, draw_w, draw_h);
    }
    else
    {
        gfx::clear(gfx::rgb(0, 0, 0));
        draw_player_loading(status);
    }

    if (g_view.show_debug)
    {
        /* Separate source, delivery and presentation diagnostics. */
        const auto upper = [](std::string value)
        {
            for (char &c : value)
                c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
            return value;
        };
        const auto channels = [](int count)
        {
            switch (count)
            {
            case 0:
                return std::string{};
            case 1:
                return std::string{" Mono"};
            case 2:
                return std::string{" Stereo"};
            case 6:
                return std::string{" 5.1"};
            case 8:
                return std::string{" 7.1"};
            default:
                return " " + std::to_string(count) + "ch";
            }
        };
        const auto megabits = [](long long bits)
        {
            char out[32];
            std::snprintf(out, sizeof(out), "%.1f Mb/s", static_cast<double>(bits) / 1000000.0);
            return std::string{out};
        };

        const jellyfin::MediaSource &source = g_view.playing_source;

        /* Describe the selected audio stream rather than the file default. */
        const jellyfin::Track *audio_track = nullptr;
        const int wanted_audio =
            status.audio_index >= 0 ? status.audio_index : source.default_audio;
        for (const jellyfin::Track &track : source.audio_tracks)
            if (track.index == wanted_audio)
            {
                audio_track = &track;
                break;
            }
        std::string audio_format = upper(audio_track ? audio_track->codec : source.audio_codec);
        if (audio_track != nullptr)
        {
            /* The codec alone reads DTS for both DTS-HD MA and core DTS. */
            if (audio_track->codec == "dts" && !audio_track->profile.empty())
                audio_format = upper(audio_track->profile);
            else if (audio_track->profile.find("Atmos") != std::string::npos)
                audio_format += " ATMOS";
        }
        const int audio_channels = audio_track ? audio_track->channels : source.audio_channels;

        std::string first = "Source";
        if (source.width > 0)
            first += "    " + std::to_string(source.width) + "x" + std::to_string(source.height);
        if (!source.video_codec.empty())
            first += "    " + upper(source.video_codec);
        if (!audio_format.empty())
            first += "    " + audio_format + channels(audio_channels);
        if (source.bitrate > 0)
            first += "    " + megabits(source.bitrate);

        std::string second = "Playing";
        if (status.width > 0)
            second += "   " + std::to_string(status.width) + "x" + std::to_string(status.height);
        /* delivery_info already reads "Video x copy | Audio y | Server: reasons". */
        std::string delivery = status.delivery_info;
        std::string reasons;
        if (const std::size_t at = delivery.find("| Server: "); at != std::string::npos)
        {
            reasons = delivery.substr(at + 10);
            delivery = delivery.substr(0, at);
        }
        /* "Video hevc copy | Audio eac3 -> aac transcode (client AAC decoder)"
           says the same thing in half the words. */
        for (const char *noise : {"Video ", "Audio ", " (client AAC decoder)"})
        {
            for (std::size_t at = delivery.find(noise); at != std::string::npos;
                 at = delivery.find(noise))
                delivery.erase(at, std::char_traits<char>::length(noise));
        }
        if (!delivery.empty())
            second += "   " + delivery;

        char pipeline[160];
        std::snprintf(pipeline, sizeof(pipeline),
                      "Pipeline   %.2f fps    decode %llu ms    convert %llu ms    a/v %d ms",
                      status.frame_rate, static_cast<unsigned long long>(status.decode_us / 1000),
                      static_cast<unsigned long long>(status.convert_us / 1000),
                      status.audio_lead_ms);

        /* Report the effective subtitle method, including server burn-in. */
        std::string subtitle_line = "Subtitles  ";
        if (status.subtitle_burned)
            subtitle_line += "burned in by the server";
        else if (status.subtitle_index < 0)
            subtitle_line += "off";
        else
            subtitle_line += "track " + std::to_string(status.subtitle_index) + ", text";
        if (const std::size_t held = subtitles::count(); held > 0)
            subtitle_line += "   " + std::to_string(held) + " cues held";
        if (!subtitle_timing::is_original(g_view.subtitle_offset))
            subtitle_line += "   " + subtitle_timing::describe(g_view.subtitle_offset);

        /* Report source cadence and display cadence separately. */
        std::string display_line = "Display    ";
        int out_w = 0;
        int out_h = 0;
        double out_hz = 0.0;
        if (gfx::output_mode(out_w, out_h, out_hz))
        {
            char shape[64] = {};
            (void)std::snprintf(shape, sizeof(shape), "%dx%d  %.2f Hz", out_w, out_h, out_hz);
            display_line += shape;
        }
        else
        {
            display_line += "unavailable";
        }
        /* Compare repeated-frame cadence with the expected source/display ratio. */
        std::string cadence_line = "Cadence    ";
        {
            unsigned total = 0;
            for (const unsigned count : status.holds)
                total += count;
            if (total == 0)
            {
                cadence_line += "measuring...";
            }
            else
            {
                for (std::size_t hold = 1; hold < 6; ++hold)
                {
                    if (status.holds[hold] == 0)
                        continue;
                    char part[48] = {};
                    (void)std::snprintf(part, sizeof(part), "%s%u held %u%%   ",
                                        hold == 5 ? ">" : "",
                                        hold == 5 ? 4u : static_cast<unsigned>(hold),
                                        status.holds[hold] * 100u / total);
                    cadence_line += part;
                }
                char tail[64] = {};
                (void)std::snprintf(tail, sizeof(tail), "  %ld dropped of %ld shown",
                                    status.dropped, status.shown);
                cadence_line += tail;
            }
        }

        if (status.source_fps > 0.0)
        {
            char source[96] = {};
            const double ratio = out_hz > 0.0 ? out_hz / status.source_fps : 0.0;
            const double off = ratio - static_cast<double>(static_cast<int>(ratio + 0.5));
            (void)std::snprintf(source, sizeof(source), "   source %.3f fps   %.2f per frame, %s",
                                status.source_fps, ratio,
                                ratio > 0.0 && off < 0.01 && off > -0.01 ? "even" : "uneven");
            display_line += source;
        }

        std::string rates =
            "Bitrate    Limit " + (status.max_bitrate > 0 ? megabits(status.max_bitrate)
                                                          : std::string{"Auto (120 Mb/s ceiling)"});
        rates += " | Media " + (status.measured_bitrate > 0
                                    ? megabits(static_cast<long long>(status.measured_bitrate))
                                    : std::string{"measuring..."});
        rates += " | Download " + (status.download_bitrate > 0
                                       ? megabits(static_cast<long long>(status.download_bitrate))
                                       : std::string{"measuring..."});
        std::vector<std::string> lines{first, second, rates};
        if (!reasons.empty())
            lines.push_back("Reason     " + reasons);
        lines.emplace_back(pipeline);
        lines.push_back(subtitle_line);
        lines.push_back(display_line);
        lines.push_back(cadence_line);
        if (!status.colour_info.empty())
            lines.push_back("Colour     " + status.colour_info);

        /* Its own panel at the top, clear of the controls and of the picture's
           own subtitles, and legible rather than merely present. */
        constexpr int kStep = 30;
        const int width = gfx::kWidth - kSafeX * 2;
        const int height = static_cast<int>(lines.size()) * kStep + 28;
        gfx::rounded_rect(kSafeX - 24, kSafeY - 14, width + 48, height, 18,
                          gfx::rgba(0x00, 0x00, 0x00, 0xc4));
        int line_y = kSafeY + 2;
        for (const std::string &line : lines)
        {
            text::draw_ellipsized(kSafeX, line_y, width, line, 21, Weight::regular,
                                  gfx::rgb(0xd2, 0xd2, 0xda));
            line_y += kStep;
        }
    }

    /* The subtitles hold one height whatever the controls are doing, and the
       controls are drawn over the top of them. */
    if (!cue.empty())
        draw_subtitle_lines(cue);

    /* Use next-episode artwork, falling back to the current title at EOF. */
    if (status.state == player::State::ended && !g_view.autoplay.counting() &&
        !g_view.autoplay.asking() && !g_view.changing_episode && !g_view.finished)
    {
        g_view.finished = true;
        g_view.end_choice = 0;
        /* Everything the stream held goes back now, not when the viewer
           eventually presses something. */
        player::stop();
        subtitles::clear();
        pad::set_trigger_feel(pad::TriggerFeel::none);
        trace::mark("player end: finished, stream released");
    }
    const bool finished = g_view.finished && !g_view.changing_episode;
    {
        /* One line per change of state at the end of a title, so what the
           screen did is recoverable from the trace. */
        static int s_state = -1;
        const int now = static_cast<int>(status.state) * 10 + (finished ? 1 : 0);
        if (now != s_state)
        {
            s_state = now;
            trace::mark("player end: state " + std::to_string(static_cast<int>(status.state)) +
                        (finished ? " showing the end card" : "") + ", next " +
                        (g_view.has_next ? "yes" : "no") + ", changing " +
                        (g_view.changing_episode ? "yes" : "no"));
        }
    }
    if (finished)
        draw_end_card();
    /* The next episode, and the question, over everything but the controls. */
    else if (g_view.up_next_in.value() > 0.004f && !g_view.up_next.id.empty())
        draw_up_next_card();

    if (g_view.osd_visible || g_view.panel_open)
    {
        if (!g_view.show_debug)
            draw_player_title(status);
        draw_player_controls(status);
    }
    // Draw above the OSD scrim, clear of its module labels and timeline.
    if (g_view.intro_in.value() > 0.004f && !g_view.panel_open)
        draw_skip_intro();

    draw_transport_badge(status, kTransportRow);
    if (g_view.autoplay.asking())
        draw_still_watching();
}

/* Subtitle appearance controls and a live preview. */
struct SubtitleRow
{
    const char *label;   /* empty for a heading */
    const char *heading; /* the heading's text, when label is empty */
    int subtitle_style::Choices::*field;
    int count;
    const char *const *names;
};

const std::vector<SubtitleRow> &subtitle_rows() noexcept
{
    namespace ss = subtitle_style;
    static const std::vector<SubtitleRow> rows = {
        {"", "Text subtitles", nullptr, 0, nullptr},
        {"Size", nullptr, &ss::Choices::size, 5, ss::kSizeNames},
        {"Font", nullptr, &ss::Choices::font, 2, ss::kFontNames},
        {"Weight", nullptr, &ss::Choices::bold, 2, ss::kBoldNames},
        {"Colour", nullptr, &ss::Choices::colour, 6, ss::kColourNames},
        {"Edge", nullptr, &ss::Choices::edge, 4, ss::kEdgeNames},
        {"Background", nullptr, &ss::Choices::background, 3, ss::kBackgroundNames},
        {"Background shape", nullptr, &ss::Choices::box, 2, ss::kBoxNames},
        {"Line spacing", nullptr, &ss::Choices::spacing, 3, ss::kSpacingNames},
        {"Position", nullptr, &ss::Choices::position, 4, ss::kPositionNames},
        {"", "Picture subtitles (Blu-ray, DVD)", nullptr, 0, nullptr},
        {"When one is the default", nullptr, &ss::Choices::picture, 2, ss::kPictureNames},
        {"Back to the original look", nullptr, nullptr, 0, nullptr},
    };
    return rows;
}

void draw_subtitle_settings() noexcept
{
    text::draw(kContentX, kSafeY + 96, "Subtitles", 38, Weight::bold, gfx::palette::text);
    config::Settings &settings = config::current();
    const auto &rows = subtitle_rows();

    constexpr int kListW = 700;
    /* Nine choices, two headings and an action have to finish above the hint
       line at the foot of the screen. */
    constexpr int kRowH = 52;
    int focus_y = kGridTop - 20;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
    {
        if (rows[static_cast<std::size_t>(i)].label[0] == '\0')
        {
            focus_y += (i == 0 ? 0 : 14) + 44;
            continue;
        }
        if (i == g_view.subtitle_row)
            ui::focus_pill(kContentX - 16, focus_y, kListW, kRowH - 8, ui::FocusGroup::settings, 1);
        focus_y += kRowH;
    }
    int y = kGridTop - 20;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
    {
        const SubtitleRow &row = rows[static_cast<std::size_t>(i)];
        if (row.label[0] == '\0')
        {
            y += i == 0 ? 0 : 14;
            text::draw(kContentX, y + 14, row.heading, 21, Weight::medium, gfx::palette::accent);
            y += 44;
            continue;
        }
        const bool focused = i == g_view.subtitle_row;
        ui::control_label(kContentX + 8, y, kListW - 300, kRowH - 8, row.label, 24, Weight::medium,
                          focused ? gfx::palette::text : gfx::palette::text_dim);
        if (row.field != nullptr)
        {
            const int value = subtitle_style::wrap(settings.subtitle_look.*row.field, row.count);
            std::string shown = row.names[value];
            if (focused)
                shown = "\xe2\x80\xb9  " + shown + "  \xe2\x80\xba";
            const int width = text::measure(shown, 24, Weight::regular);
            text::draw(kContentX + kListW - 44 - width,
                       text::centered_y(y, kRowH - 8, row.label, 24, Weight::medium), shown, 24,
                       Weight::regular, focused ? gfx::palette::text : gfx::palette::text_dim);
        }
        y += kRowH;
    }

    /* The preview: bright below, dark above, so the edge and box choices can be
       judged against both kinds of picture. */
    constexpr int kPreviewX = kContentX + 760;
    const int preview_w = kContentRight - kPreviewX;
    constexpr int kPreviewY = kGridTop - 20;
    constexpr int kPreviewH = 560;
    gfx::push_clip(kPreviewX, kPreviewY, preview_w, kPreviewH);
    gfx::rounded_rect(kPreviewX, kPreviewY, preview_w, kPreviewH, 18, gfx::rgb(0x1a, 0x1f, 0x2b));
    gfx::fill_rect(kPreviewX, kPreviewY + kPreviewH / 2, preview_w, kPreviewH / 2,
                   gfx::rgb(0xc9, 0xcf, 0xd6));
    text::draw(kPreviewX + 24, kPreviewY + 18, "Preview", 20, Weight::medium,
               gfx::palette::text_faint);
    subtitle_style::Style style = subtitle_style::resolve(settings.subtitle_look);
    /* Position is shown to scale: the preview is about half the screen's height. */
    style.bottom_margin = style.bottom_margin * kPreviewH / gfx::kHeight + 24;
    draw_subtitle_block("This is how your subtitles\nwill look while you watch.", style,
                        kPreviewX + preview_w / 2, kPreviewY + kPreviewH);
    gfx::pop_clip();

    text::draw(kContentX, gfx::kHeight - 90,
               "Left and right to change    \xc2\xb7    Circle to go back", 20, Weight::regular,
               gfx::palette::text_faint);
}

/* Settings category navigation. */
/* The idle-time choices, in the order the Playback page steps through them. */
constexpr int kIdleMinutes[] = {0, 30, 60, 90, 120, 180};
int idle_index(int minutes) noexcept
{
    for (int i = 0; i < 6; ++i)
        if (kIdleMinutes[i] == minutes)
            return i;
    return 3; /* whatever was stored, ninety is the one it means */
}

constexpr const char *kSettingsMenu[] = {"Subtitles", "Playback", "Controller",
                                         "Server",    "Account",  "Diagnostics"};
constexpr int kSettingsMenuCount = 6;

/* A screen's single action row, focused, with the menu's look. */
void draw_settings_action(int y, const std::string &label, const std::string &value) noexcept
{
    constexpr int kW = 900;
    ui::focus_pill(kContentX - 16, y, kW, 64, ui::FocusGroup::settings, g_view.settings_page);
    ui::control_label(kContentX + 8, y, kW - 260, 64, label, 26, Weight::medium,
                      gfx::palette::text);
    if (!value.empty())
    {
        const std::string shown = "\xe2\x80\xb9  " + value + "  \xe2\x80\xba";
        text::draw(kContentX + kW - 44 - text::measure(shown, 26, Weight::regular),
                   text::centered_y(y, 64, label, 26, Weight::medium), shown, 26, Weight::regular,
                   gfx::palette::text);
    }
}

enum class AccountRow : unsigned char
{
    profile,
    dashboard,
    sign_out,
};

std::vector<AccountRow> account_rows() noexcept
{
    std::vector<AccountRow> rows{AccountRow::profile};
    if (account::is_admin())
        rows.push_back(AccountRow::dashboard);
    rows.push_back(AccountRow::sign_out);
    return rows;
}

const char *account_row_label(AccountRow row) noexcept
{
    switch (row)
    {
    case AccountRow::profile:
        return "Profile";
    case AccountRow::dashboard:
        return "Dashboard";
    case AccountRow::sign_out:
        return "Sign out";
    }
    return "";
}

const char *account_row_hint(AccountRow row) noexcept
{
    switch (row)
    {
    case AccountRow::profile:
        return "Your picture and password.";
    case AccountRow::dashboard:
        return "Manage the server, as the Jellyfin dashboard does.";
    case AccountRow::sign_out:
        return "Forgets this console's sign-in and starts again.";
    }
    return "";
}

void draw_settings(const std::string &server_name) noexcept
{
    const config::Settings &settings = config::current();
    const std::string address = server_address::origin(settings.host, settings.port);
    switch (g_view.settings_page)
    {
    case 1:
        draw_subtitle_settings();
        return;
    case 2:
    {
        /* Autoplay, the way every television app offers it. */
        text::draw(kContentX, kSafeY + 96, "Playback", 38, Weight::bold, gfx::palette::text);
        const autoplay::Choices &look = settings.autoplay_look;
        static constexpr const char *kAskNames[] = {"Never", "After 2 episodes", "After 3 episodes",
                                                    "After 4 episodes", "After 5 episodes"};
        static constexpr const char *kIdleNames[] = {
            "Never",         "After 30 minutes", "After 60 minutes", "After 90 minutes",
            "After 2 hours", "After 3 hours"};
        const char *const rows[3] = {"Play the next episode automatically",
                                     "Ask if you are still watching",
                                     "Ask when nothing has been pressed"};
        const std::string values[3] = {
            look.enabled ? "On" : "Off", kAskNames[std::clamp(look.ask_after, 0, 4)],
            kIdleNames[std::clamp(idle_index(look.ask_idle_minutes), 0, 5)]};
        ui::focus_pill(kContentX - 28, kGridTop + g_view.playback_row * 84 - 6, 900, 66,
                       ui::FocusGroup::settings, 2);
        for (int i = 0; i < 3; ++i)
        {
            const int y = kGridTop + i * 84;
            const bool focused = i == g_view.playback_row;
            ui::control_label(kContentX, y - 6, 620, 66, rows[i], 30, Weight::medium,
                              focused ? gfx::palette::text : gfx::palette::text_dim);
            const std::string shown =
                focused ? "\xe2\x80\xb9  " + values[i] + "  \xe2\x80\xba" : values[i];
            text::draw(kContentX + 860 - text::measure(shown, 28, Weight::regular),
                       text::centered_y(y - 6, 66, rows[i], 30, Weight::medium), shown, 28,
                       Weight::regular, focused ? gfx::palette::text : gfx::palette::text_dim);
        }
        text::draw_wrapped(
            kContentX, kGridTop + 280, 900, 3, 34,
            g_view.playback_row == 0
                ? "A card offers the next episode over the closing credits and counts "
                  "itself down. Cross takes it early, Circle sends it away."
            : g_view.playback_row == 1
                ? "After this many episodes played one after another with nothing "
                  "pressed, playback stops and asks. It gives up after a minute, so a "
                  "room nobody is in does not hold the server open."
                : "And the same question for one long thing left running: a film, or "
                  "an episode nobody stopped.",
            22, Weight::regular, gfx::palette::text_dim);
        break;
    }
    case 3:
    {
        text::draw(kContentX, kSafeY + 96, "Controller", 38, Weight::bold, gfx::palette::text);
        const bool available = pad::trigger_effects_available();
        draw_settings_action(kGridTop, "Trigger feedback",
                             available ? (settings.trigger_feedback ? "On" : "Off")
                                       : "Not available");
        text::draw(kContentX, kGridTop + 84,
                   !available ? "This controller has no adaptive triggers."
                   : settings.trigger_feedback
                       ? "The triggers resist while you seek, until you push through."
                       : "The triggers stay free.",
                   22, Weight::regular, gfx::palette::text_dim);
        break;
    }
    case 4:
        text::draw(kContentX, kSafeY + 96, "Server", 38, Weight::bold, gfx::palette::text);
        text::draw(kContentX, kGridTop, server_name.empty() ? address : server_name, 30,
                   Weight::medium, gfx::palette::text);
        if (!server_name.empty())
            text::draw(kContentX, kGridTop + 46, address, 22, Weight::regular,
                       gfx::palette::text_dim);
        draw_settings_action(kGridTop + 110, "Change server", "");
        break;
    case 6:
    {
        /* Reports can be sent from here as well as from the offer that
           follows a crash, and this is also where they are thrown away. */
        text::draw(kContentX, kSafeY + 96, "Diagnostics", 38, Weight::bold, gfx::palette::text);
        const bool available = crash::report_available();
        text::draw(kContentX, kGridTop,
                   available ? "This console is holding a report"
                             : "Nothing has gone wrong since the last report",
                   30, Weight::medium, gfx::palette::text);
        text::draw_ellipsized(kContentX, kGridTop + 46, kContentRight - kContentX,
                              available
                                  ? crash::report_summary()
                                  : "Reports are written whenever SlopFin stops unexpectedly.",
                              22, Weight::regular, gfx::palette::text_dim);
        const bool can_send = diagnostics::destination(settings.report_server).valid();
        const int row_count = can_send ? 2 : 1;
        const char *rows[2] = {can_send ? "Send to report receiver" : "Delete the reports",
                               "Delete the reports"};
        g_view.diagnostics_row = std::clamp(g_view.diagnostics_row, 0, row_count - 1);
        ui::focus_pill(kContentX - 28, kGridTop + 120 + g_view.diagnostics_row * 84 - 6, 620, 66,
                       ui::FocusGroup::settings, g_view.settings_page);
        for (int i = 0; i < row_count; ++i)
        {
            const int y = kGridTop + 120 + i * 84;
            const bool focused = i == g_view.diagnostics_row;
            ui::control_label(kContentX, y - 6, 560, 66, rows[i], 30, Weight::medium,
                              focused ? gfx::palette::text : gfx::palette::text_dim);
            if (i == row_count - 1 && focused)
            {
                text::draw(kContentX + 360, y + 12, "Hold Cross", 22, Weight::regular,
                           gfx::palette::text_faint);
                gfx::rounded_rect(kContentX + 360, y + 46, 200, 6, 3,
                                  gfx::rgba(0xff, 0xff, 0xff, 0x22));
                const int filled = 200 * std::clamp(g_view.diagnostics_hold, 0, 60) / 60;
                if (filled > 0)
                    gfx::rounded_rect(kContentX + 360, y + 46, filled, 6, 3, gfx::palette::danger);
            }
        }
        text::draw_wrapped(
            kContentX, kGridTop + 320, 900, 2, 30,
            diagnostics::destination(settings.report_server).valid()
                ? "Reports are sent only when you choose Send, to your configured report receiver."
                : "Reports stay on this console. A separate report receiver is optional.",
            21, Weight::regular, gfx::palette::text_faint);
        break;
    }
    case 5:
    {
        text::draw(kContentX, kSafeY + 96, "Account", 38, Weight::bold, gfx::palette::text);
        text::draw(kContentX, kGridTop,
                   "Signed in to " + (server_name.empty() ? address : server_name), 30,
                   Weight::medium, gfx::palette::text);
        /* The same places the profile menu leads . */
        const std::vector<AccountRow> rows = account_rows();
        ui::focus_pill(kContentX - 28, kGridTop + 84 + g_view.account_row * 84 - 6, 620, 66,
                       ui::FocusGroup::settings, g_view.settings_page);
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            const int y = kGridTop + 84 + static_cast<int>(i) * 84;
            const bool focused = static_cast<int>(i) == g_view.account_row;
            ui::control_label(kContentX, y - 6, 560, 66, account_row_label(rows[i]), 32,
                              Weight::medium,
                              focused ? gfx::palette::text : gfx::palette::text_dim);
        }
        text::draw(kContentX, kGridTop + 84 + static_cast<int>(rows.size()) * 84 + 24,
                   account_row_hint(rows[static_cast<std::size_t>(
                       std::clamp(g_view.account_row, 0, static_cast<int>(rows.size()) - 1))]),
                   22, Weight::regular, gfx::palette::text_dim);
        break;
    }
    default:
    {
        text::draw(kContentX, kSafeY + 96, "Settings", 38, Weight::bold, gfx::palette::text);
        if (!g_view.sidebar_focused)
            ui::focus_pill(kContentX - 28, kGridTop + g_view.settings_index * 84 - 6, 620, 68,
                           ui::FocusGroup::settings, g_view.settings_page);
        for (int i = 0; i < kSettingsMenuCount; ++i)
        {
            const bool focused = i == g_view.settings_index && !g_view.sidebar_focused;
            const int y = kGridTop + i * 84;
            ui::control_label(kContentX, y - 6, 560, 68, kSettingsMenu[i], 34, Weight::medium,
                              focused ? gfx::palette::text : gfx::palette::text_dim);
        }
        text::draw(kContentX, kGridTop + kSettingsMenuCount * 84 + 40,
                   "SlopFin 0.1   unofficial Jellyfin client", 20, Weight::regular,
                   gfx::palette::text_faint);
        return;
    }
    }
    text::draw(kContentX, gfx::kHeight - 90, "Circle to go back", 20, Weight::regular,
               gfx::palette::text_faint);
}

/* Upload diagnostics only after explicit confirmation. */
void send_crash_report() noexcept
{
    g_view.report_prompt = View::ReportPrompt::sending;
    const config::Settings settings = config::current();
    background::run(
        [settings]
        {
            const auto finish = [](bool sent, std::string message)
            {
                std::lock_guard<std::mutex> guard(g_shared.mutex);
                g_shared.report_sent = sent;
                g_shared.report_message = std::move(message);
                g_shared.report_ready = true;
            };
            const auto destination = diagnostics::destination(settings.report_server);
            if (!destination.valid())
            {
                finish(false, "No report receiver is configured. Reports stay on this console.");
                return;
            }
            const std::string body = crash::report_body();
            const std::vector<std::string> headers{
                "X-SlopFin-Kind: " + crash::report_kind(),
                "X-SlopFin-Build: " __DATE__,
                "X-SlopFin-Device: PS5-" + settings.device_id.substr(0, 8),
            };
            const http::Response answer = http::post(destination.host, destination.port, "/report",
                                                     headers, body, "text/plain; charset=utf-8");
            if (answer.ok())
            {
                crash::report_sent();
                finish(true, "Report sent to " + destination.display());
            }
            else
                finish(false, answer.status > 0
                                  ? "The receiver answered " + std::to_string(answer.status)
                                  : "Could not reach the configured report receiver.");
        });
}

/* True while the offer is holding the input. */
bool report_prompt_open() noexcept
{
    return g_view.report_prompt != View::ReportPrompt::none;
}

void handle_report_prompt() noexcept
{
    if (g_view.report_prompt == View::ReportPrompt::offer)
    {
        if (pad::pressed(pad::Button::left) || pad::pressed(pad::Button::right))
            g_view.report_choice = g_view.report_choice == 0 ? 1 : 0;
        else if (pad::pressed(pad::Button::circle))
            g_view.report_prompt = View::ReportPrompt::none;
        else if (pad::pressed(pad::Button::cross))
        {
            if (g_view.report_choice == 0)
                send_crash_report();
            else
                g_view.report_prompt = View::ReportPrompt::none;
        }
        return;
    }
    if (g_view.report_prompt == View::ReportPrompt::sending)
        return; /* nothing to press while it is on the wire */
    if (g_view.report_leaves > 0 && --g_view.report_leaves == 0)
        g_view.report_prompt = View::ReportPrompt::none;
    if (pad::pressed(pad::Button::cross) || pad::pressed(pad::Button::circle))
        g_view.report_prompt = View::ReportPrompt::none;
}

void draw_report_prompt() noexcept
{
    if (g_view.report_prompt == View::ReportPrompt::none)
        return;
    constexpr int kCardW = 860;
    constexpr int kCardH = 300;
    const int x = (gfx::kWidth - kCardW) / 2;
    const int y = (gfx::kHeight - kCardH) / 2;
    gfx::fill_rect(0, 0, gfx::kWidth, gfx::kHeight, gfx::rgba(0x00, 0x00, 0x00, 0xb4));
    gfx::drop_shadow(x, y, kCardW, kCardH, 26, 26, 0x90);
    gfx::rounded_rect(x, y, kCardW, kCardH, 26, gfx::rgba(0x16, 0x16, 0x1b, 0xfa));
    const bool offering = g_view.report_prompt == View::ReportPrompt::offer;
    const std::string title =
        offering                                              ? "SlopFin stopped unexpectedly"
        : g_view.report_prompt == View::ReportPrompt::sending ? "Sending the report"
        : g_view.report_prompt == View::ReportPrompt::sent    ? "Thank you"
                                                              : "That did not send";
    text::draw(x + 44, y + 40, title, 34, Weight::bold, gfx::palette::text);
    const std::string line = offering ? crash::report_summary() : g_view.report_line;
    text::draw_ellipsized(x + 44, y + 92, kCardW - 88, line, 23, Weight::regular,
                          gfx::palette::text_dim);
    if (offering)
    {
        text::draw_ellipsized(x + 44, y + 126, kCardW - 88,
                              "The report says what the app was doing and what it had just done. "
                              "Nothing else leaves the console.",
                              21, Weight::regular, gfx::palette::text_faint);
        const char *labels[2] = {"Send report", "Not now"};
        int button_x = x + 44;
        for (int i = 0; i < 2; ++i)
        {
            draw_action(button_x, y + kCardH - 92, labels[i], i == g_view.report_choice);
            button_x += action_width(labels[i]) + kActionGap;
        }
    }
    else if (g_view.report_prompt == View::ReportPrompt::sending)
    {
        const float head = static_cast<float>(g_view.frames % 54) / 54.0f;
        gfx::arc(x + 60, y + kCardH - 66, 18, 3, 0.0f, 1.0f, gfx::rgba(0xff, 0xff, 0xff, 0x24));
        gfx::arc(x + 60, y + kCardH - 66, 18, 3, head, 0.3f, gfx::rgba(0xff, 0xff, 0xff, 0xe6));
    }
    else
    {
        draw_action(x + 44, y + kCardH - 92, "Close", true);
    }
}

void draw_message(const std::string &status, const std::string &detail) noexcept
{
    draw_mark(gfx::kWidth / 2 - 40, 380, 80, gfx::palette::accent);
    const int width = text::measure(status, 34, Weight::medium);
    text::draw((gfx::kWidth - width) / 2, 500, status, 34, Weight::medium, gfx::palette::text);
    if (!detail.empty())
    {
        const int detail_width = text::measure(detail, 22, Weight::regular);
        text::draw((gfx::kWidth - detail_width) / 2, 552, detail, 22, Weight::regular,
                   gfx::palette::danger);
    }
    const int phase = (g_view.frames / 3) % 60;
    gfx::rounded_rect(gfx::kWidth / 2 - 60 + phase * 2, 610, 40, 6, 3, gfx::palette::accent);
}

/* ------------------------------------------------------------ navigation */

void clamp_home(const std::vector<Row> &rows) noexcept
{
    if (rows.empty())
        return;
    g_view.column.resize(rows.size(), 0);
    g_view.row_offset.resize(rows.size());
    g_view.row_index = std::clamp(g_view.row_index, 0, static_cast<int>(rows.size()) - 1);
    for (std::size_t r = 0; r < rows.size(); ++r)
    {
        const int last = std::max(0, static_cast<int>(rows[r].items.size()) - 1);
        g_view.column[r] = std::clamp(g_view.column[r], 0, last);
    }
}

void update_home_scroll(const std::vector<Row> &rows) noexcept
{
    if (rows.empty())
        return;
    g_view.scroll.aim(
        static_cast<float>(row_top(rows, static_cast<std::size_t>(g_view.row_index)) - kRowsTop));

    /* Scroll only enough to keep the selected card visible. */
    const auto r = static_cast<std::size_t>(g_view.row_index);
    const auto column = static_cast<std::size_t>(g_view.column[r]);
    const int card_left = card_offset(rows[r], column);
    const int focused_w = column < rows[r].items.size() ? card_w_for(rows[r].items[column], rows[r])
                                                        : row_card_w(rows[r]);
    const int visible = kContentWidth - focused_w;
    float target = g_view.row_offset[r].target();
    if (static_cast<float>(card_left) - target < 0.0f)
        target = static_cast<float>(card_left);
    else if (static_cast<float>(card_left) - target > static_cast<float>(visible))
        target = static_cast<float>(card_left - visible);
    g_view.row_offset[r].aim(std::max(0.0f, target));
}

void ask_keyboard(View::Prompt prompt) noexcept
{
    if (!ime::available())
    {
        if (prompt == View::Prompt::search)
        {
            g_view.search.keyboard_notice =
                "The PlayStation keyboard could not start. Restart the app and try again.";
            return;
        }
        set_status(prompt == View::Prompt::host ? Screen::server_setup : Screen::sign_in,
                   "Keyboard unavailable",
                   "The PS5 keyboard could not start. Restart the app or use Quick Connect.");
        return;
    }
    g_view.search.keyboard_notice.clear();
    g_view.prompt = prompt;
    switch (prompt)
    {
    case View::Prompt::host:
        prepare_server_draft();
        ime::request(g_view.server_draft, "Jellyfin server address",
                     "Server name or IP, e.g. media.example.com", ime::Mode::text);
        break;
    case View::Prompt::username:
        ime::request(g_view.pending_username, "Username", "Your Jellyfin username",
                     ime::Mode::text);
        break;
    case View::Prompt::password:
        ime::request(g_view.pending_password, "Password", "Your Jellyfin password",
                     g_view.show_password ? ime::Mode::text : ime::Mode::password);
        break;
    case View::Prompt::search:
        ime::request(g_view.search.committed, "Search Jellyfin", "Find a movie, series or episode",
                     ime::Mode::search);
        break;
    case View::Prompt::none:
        break;
    }
}

void connect_server_draft() noexcept
{
    prepare_server_draft();
    const auto address = server_address::parse(g_view.server_draft);
    if (!address.valid())
    {
        g_view.setup_index = 0;
        set_status(Screen::server_setup, "Check your server address", address.error);
        return;
    }
    g_view.server_draft = address.display();
    set_status(Screen::connecting, "Contacting " + address.display());
    submit(Request::probe_server, address.host, {}, address.port);
}

void handle_keyboard_result() noexcept
{
    /* A keyboard the Profile or Dashboard screen asked for is theirs to read. */
    if (g_view.prompt == View::Prompt::none)
        return;
    std::string entered;
    if (!ime::take_result(entered))
    {
        if (ime::take_cancelled())
        {
            g_view.ime_input_guard = 2;
            if (g_view.prompt == View::Prompt::search && g_view.search.committed.empty())
                g_view.search.open = false;
            g_view.prompt = View::Prompt::none;
        }
        return;
    }
    g_view.ime_input_guard = 2;
    const View::Prompt prompt = g_view.prompt;
    g_view.prompt = View::Prompt::none;

    switch (prompt)
    {
    case View::Prompt::host:
        g_view.server_draft = entered;
        g_view.server_draft_initialized = true;
        g_view.setup_index = 1;
        set_status(Screen::server_setup, "Choose your server");
        break;
    case View::Prompt::username:
        g_view.pending_username = entered;
        g_view.setup_index = 1;
        break;
    case View::Prompt::password:
        g_view.pending_password = entered;
        g_view.setup_index = 3;
        break;
    case View::Prompt::search:
        g_view.search.draft = entered;
        g_view.search.refresh_requested = true;
        g_view.search.pending = 1;
        break;
    case View::Prompt::none:
        break;
    }
}

/* Restore the prior page and its viewport when navigating back. */
struct GridPosition
{
    int index = 0;
    float scroll = 0.0f;
};

struct DetailPosition
{
    int episode = 0;
    int action = 0;
    int source = 0;
    int cast_index = 0;
    int details_scroll = 0;
    float strip_scroll = 0.0f;
    float page_scroll = 0.0f;
    float cast_scroll = 0.0f;
    bool details_open = false;
};

/* Render-thread navigation memory. A category/detail is a viewport, not just
   an ID: returning to it should restore the exact place the viewer left. */
std::unordered_map<std::string, GridPosition> g_grid_positions;
std::unordered_map<std::string, DetailPosition> g_detail_positions;

struct Back
{
    Screen screen = Screen::home;
    std::string id;    /* library id for a grid, item id for a detail */
    std::string title; /* what the grid was called */
    int index = 0;     /* grid_index, or the focused episode on a detail */
    int action = 0;    /* which detail button had the focus */
    int source = 0;
    int cast_index = 0;
    int details_scroll = 0;
    float scroll = 0.0f;      /* grid or detail strip */
    float page_scroll = 0.0f; /* vertical detail page */
    float cast_scroll = 0.0f;
    bool details_open = false;
};
std::vector<Back> g_back;
void push_back_entry(Screen from) noexcept
{
    if (g_back.size() > 16)
        g_back.erase(g_back.begin());
    Back entry;
    entry.screen = from;
    switch (from)
    {
    case Screen::grid:
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        entry.id = g_shared.grid_library_id;
        entry.title = g_shared.grid_title;
        entry.index = g_view.grid_index;
        entry.scroll = g_view.grid_scroll.value();
        if (!entry.id.empty())
            g_grid_positions[entry.id] = GridPosition{entry.index, entry.scroll};
        break;
    }
    case Screen::detail:
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        entry.id = g_shared.detail_item.id;
        entry.index = g_view.detail_episode;
        entry.action = g_view.detail_action;
        entry.source = g_view.detail_source;
        entry.cast_index = g_view.detail_cast_index;
        entry.details_scroll = g_view.details_scroll;
        entry.scroll = g_view.detail_scroll.value();
        entry.page_scroll = g_view.detail_page.value();
        entry.cast_scroll = g_view.detail_cast_scroll.value();
        entry.details_open = g_view.details_open;
        if (!entry.id.empty())
            g_detail_positions[entry.id] =
                DetailPosition{entry.index,       entry.action,         entry.source,
                               entry.cast_index,  entry.details_scroll, entry.scroll,
                               entry.page_scroll, entry.cast_scroll,    entry.details_open};
        break;
    }
    default:
        /* Home and the rest keep their own state, so there is nothing to
           record beyond which one it was. */
        break;
    }
    if (entry.screen == Screen::detail && entry.id.empty())
        return; /* nothing to come back to */
    g_back.push_back(std::move(entry));
}

Screen current_screen() noexcept
{
    std::lock_guard<std::mutex> guard(g_shared.mutex);
    return g_shared.screen;
}

/* Opens a library's grid. `remember` is false when the step is itself a
   return, which must not push the screen it is leaving. */
void open_grid(const std::string &library_id, const std::string &title, bool remember,
               int restore_index) noexcept
{
    if (remember)
        push_back_entry(current_screen());

    GridPosition destination{};
    if (const auto found = g_grid_positions.find(library_id); found != g_grid_positions.end())
        destination = found->second;
    if (restore_index >= 0)
        destination.index = restore_index;

    /* The sidebar stays pointed at whatever is on screen. */
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        /* Switching from one category to another: keep the cards that were
           showing, so they can leave rather than vanish. */
        if (g_shared.screen == Screen::grid && !g_shared.grid_items.empty() &&
            g_shared.grid_library_id != library_id)
        {
            g_view.leaving_items = g_shared.grid_items;
            g_view.leaving_scroll = g_view.grid_scroll.value();
            g_view.leaving_frame = static_cast<long>(g_view.frames);
        }
        g_shared.screen = Screen::grid;
        g_shared.grid_items.clear();
        ++g_shared.grid_epoch;
        g_shared.grid_loading = true;
        g_shared.grid_title = title;
        g_shared.grid_library_id = library_id;
        g_shared.grid_total = 0;
    }
    /* A library keeps its own viewport. Switching Movies -> Shows no longer
       destroys Movies' focus/scroll, and returning to either category begins
       at the exact card/offset that was left. */
    g_view.grid_index = destination.index;
    g_view.grid_scroll.jump(destination.scroll);
    /* Shown once as the category opens, then retired: how long the list is is
       worth knowing before anyone has touched the stick. */
    note_scrolling();
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        /* Applied again when the async list lands so an empty loading frame
           can never clamp a remembered index to zero. */
        g_shared.grid_restore = destination.index;
    }
    submit(Request::open_library, library_id);
}

void open_grid(const std::string &library_id, const std::string &title) noexcept
{
    open_grid(library_id, title, true, -1);
}

/* Use cached item metadata immediately while detail rows load. */
void open_detail(const std::string &item_id, bool remember, const jellyfin::Item *known) noexcept
{
    if (remember)
        push_back_entry(current_screen());
    /* Whatever is already known goes up at once: the full record, if the
       cursor sat on this title long enough to fetch it, or else the card. */
    jellyfin::Item opening;
    bool item_complete = false;
    if (const std::optional<jellyfin::Item> cached = cached_detail(item_id); cached.has_value())
    {
        opening = *cached;
        item_complete = true;
    }
    else if (known != nullptr && known->id == item_id)
        opening = *known;

    DetailPosition destination{};
    bool restore_position = false;
    if (const auto found = g_detail_positions.find(item_id); found != g_detail_positions.end())
    {
        destination = found->second;
        restore_position = true;
    }

    std::vector<jellyfin::Item> opening_rows;
    jellyfin::Item opening_next;
    bool rows_complete = false;
    if (const std::optional<CachedDetailRows> cached = cached_detail_rows(item_id);
        cached.has_value())
    {
        opening_rows = cached->entries;
        opening_next = cached->next_up;
        rows_complete = true;
    }

    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        g_shared.screen = Screen::detail;
        g_shared.detail_target_id = item_id;
        g_shared.detail_item = opening;
        g_shared.detail_episodes = std::move(opening_rows);
        g_shared.detail_next_up = std::move(opening_next);
        ++g_shared.detail_epoch;
        /* Cast is allowed into the layout only when both halves describe the
           same complete title. That removes the "cast flashes under hero,
           then drops below Seasons" first-open state entirely. */
        g_shared.detail_loading = !(item_complete && rows_complete);
    }
    if (restore_position)
    {
        g_view.detail_action = destination.action;
        g_view.detail_source = destination.source;
        g_view.detail_episode = destination.episode;
        g_view.detail_scroll.jump(destination.strip_scroll);
        g_view.detail_page.jump(destination.page_scroll);
        g_view.detail_cast_scroll.jump(destination.cast_scroll);
        g_view.detail_cast_index = destination.cast_index;
        g_view.details_scroll = destination.details_scroll;
        g_view.details_open = destination.details_open;
        /* The page itself may transition, but the restored focus should not
           independently pop in from zero as though it were a new selection. */
        g_view.focus.jump(1.0f);
        g_view.focus.aim(1.0f);
        g_view.detail_strip_placed_for = item_id;
    }
    else
    {
        g_view.detail_action = 0;
        g_view.detail_source = 0;
        g_view.detail_episode = 0;
        g_view.detail_scroll.jump(0.0f);
        g_view.detail_page.jump(0.0f);
        g_view.detail_cast_scroll.jump(0.0f);
        g_view.detail_cast_index = 0;
        g_view.details_scroll = 0;
        g_view.details_open = false;
        g_view.detail_strip_placed_for.clear();
        restart_focus();
    }
    submit(Request::open_detail, item_id);
}

void open_detail(const std::string &item_id, bool remember) noexcept
{
    open_detail(item_id, remember, nullptr);
}

void open_detail(const std::string &item_id) noexcept
{
    open_detail(item_id, true, nullptr);
}

/* Opened from a card, which already knows most of what the page shows. */
void open_detail(const jellyfin::Item &card) noexcept
{
    open_detail(card.id, true, &card);
}

/*
 * One step back. Returns false when there is nowhere recorded to go, which is
 * the caller's cue to fall back on the home screen.
 */
bool go_back() noexcept
{
    while (!g_back.empty())
    {
        const Back entry = g_back.back();
        g_back.pop_back();
        switch (entry.screen)
        {
        case Screen::grid:
            if (entry.id.empty())
                continue;
            open_grid(entry.id, entry.title, false, entry.index);
            g_view.grid_scroll.jump(entry.scroll);
            return true;
        case Screen::detail:
            if (entry.id.empty())
                continue;
            open_detail(entry.id, false);
            /* Restore the viewport, not merely the item ID. This includes the
               vertical page position and the independent Cast/season strips,
               so Back is visually the inverse of opening the child page. */
            g_view.detail_episode = entry.index;
            g_view.detail_action = entry.action;
            g_view.detail_source = entry.source;
            g_view.detail_cast_index = entry.cast_index;
            g_view.details_scroll = entry.details_scroll;
            g_view.detail_scroll.jump(entry.scroll);
            g_view.detail_page.jump(entry.page_scroll);
            g_view.detail_cast_scroll.jump(entry.cast_scroll);
            g_view.details_open = entry.details_open;
            g_view.focus.jump(1.0f);
            g_view.focus.aim(1.0f);
            return true;
        case Screen::home:
        {
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            g_shared.screen = Screen::home;
            return true;
        }
        default:
            continue;
        }
    }
    return false;
}

void open_section(const std::vector<SidebarEntry> &entries, int index, bool keep_focus) noexcept
{
    if (index < 0 || index >= static_cast<int>(entries.size()))
        return;
    const SidebarEntry &entry = entries[static_cast<std::size_t>(index)];

    switch (entry.kind)
    {
    case SidebarEntry::Kind::home:
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        g_shared.screen = Screen::home;
        break;
    }
    case SidebarEntry::Kind::search:
        /* No longer built into the sidebar; triangle opens the search bar in
           whichever section is on screen. */
        return;
    case SidebarEntry::Kind::library:
        open_grid(entry.library_id, entry.label);
        break;
    case SidebarEntry::Kind::settings:
    {
        g_view.settings_page = 0; /* always arrive at the menu */
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        g_shared.screen = Screen::settings;
        break;
    }
    }
    g_view.active_section = index;
    /* Picking a section is a fresh start, so circle in it goes to the sidebar
       rather than unwinding wherever the viewer happened to have been. */
    g_back.clear();
    if (!keep_focus)
        g_view.sidebar_focused = false;
    restart_focus();
}

void handle_sidebar_input(const std::vector<SidebarEntry> &entries) noexcept
{
    const int count = static_cast<int>(entries.size());
    /* Selecting a sidebar entry opens that category. */
    const int before = g_view.sidebar_index;
    if (pad::pressed(pad::Button::down) && g_view.sidebar_index + 1 < count)
        ++g_view.sidebar_index;
    else if (pad::pressed(pad::Button::up) && g_view.sidebar_index > 0)
        --g_view.sidebar_index;
    else if (pad::pressed(pad::Button::cross) || pad::pressed(pad::Button::right) ||
             pad::pressed(pad::Button::circle))
    {
        /* Nothing left to confirm, so every one of these means "into it". */
        g_view.sidebar_focused = false;
        restart_focus();
    }

    if (g_view.sidebar_index != before)
        open_section(entries, g_view.sidebar_index, true);
}

void handle_home_input(const std::vector<Row> &rows) noexcept
{
    if (rows.empty())
    {
        if (pad::pressed(pad::Button::left) || pad::pressed(pad::Button::circle))
            g_view.sidebar_focused = true;
        return;
    }
    bool moved = false;
    const auto r = static_cast<std::size_t>(g_view.row_index);

    if (pad::pressed(pad::Button::down) && g_view.row_index + 1 < static_cast<int>(rows.size()))
    {
        ++g_view.row_index;
        moved = true;
    }
    else if (pad::pressed(pad::Button::up) && g_view.row_index > 0)
    {
        --g_view.row_index;
        moved = true;
    }
    else if (pad::pressed(pad::Button::right) &&
             g_view.column[r] + 1 < static_cast<int>(rows[r].items.size()))
    {
        ++g_view.column[r];
        moved = true;
    }
    else if (pad::pressed(pad::Button::left))
    {
        if (g_view.column[r] > 0)
        {
            --g_view.column[r];
            moved = true;
        }
        else
        {
            g_view.sidebar_focused = true;
        }
    }
    else if (pad::pressed(pad::Button::circle))
    {
        g_view.sidebar_focused = true;
    }
    else if (pad::pressed(pad::Button::cross))
    {
        const jellyfin::Item &item = rows[r].items[static_cast<std::size_t>(g_view.column[r])];
        open_detail(item);
    }
    else if (pad::pressed(pad::Button::triangle))
    {
        open_search();
    }
    else if (pad::pressed(pad::Button::options) && rows[r].title == "Continue Watching")
    {
        /* Options dismisses a title from Continue Watching by marking it watched. */
        const jellyfin::Item &item = rows[r].items[static_cast<std::size_t>(g_view.column[r])];
        submit(Request::mark_played, item.id);
    }

    if (moved)
        restart_focus();
}

void handle_grid_input(const std::vector<jellyfin::Item> &items) noexcept
{
    if (items.empty())
    {
        if (pad::pressed(pad::Button::left) || pad::pressed(pad::Button::circle))
            g_view.sidebar_focused = true;
        return;
    }
    /* A grid reached from somewhere goes back there; one reached from the
       sidebar hands the focus back to the sidebar. */
    if (pad::pressed(pad::Button::circle))
    {
        if (!go_back())
            g_view.sidebar_focused = true;
        return;
    }
    const int last = static_cast<int>(items.size()) - 1;
    bool moved = false;

    if (pad::pressed(pad::Button::right) && g_view.grid_index < last)
    {
        ++g_view.grid_index;
        moved = true;
    }
    else if (pad::pressed(pad::Button::left))
    {
        if (g_view.grid_index % kGridColumns != 0)
        {
            --g_view.grid_index;
            moved = true;
        }
        else
        {
            g_view.sidebar_focused = true;
        }
    }
    else if (pad::pressed(pad::Button::down))
    {
        g_view.grid_index = std::min(last, g_view.grid_index + kGridColumns);
        moved = true;
    }
    else if (pad::pressed(pad::Button::up))
    {
        if (g_view.grid_index >= kGridColumns)
        {
            g_view.grid_index -= kGridColumns;
            moved = true;
        }
    }
    else if (pad::pressed(pad::Button::cross))
    {
        open_detail(items[static_cast<std::size_t>(g_view.grid_index)]);
        return;
    }
    else if (pad::pressed(pad::Button::circle))
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        g_shared.screen = Screen::home;
    }
    else if (pad::pressed(pad::Button::triangle))
    {
        open_search();
    }

    if (moved)
        restart_focus();

    /* Continuous analogue scrolling keeps the focused item inside the viewport. */
    const float limit = grid_scroll_limit(items.size());
    const float stick = pad::scroll_axis();
    if (stick != 0.0f && limit > 0.0f)
    {
        constexpr float kRowsPerSecond = 2.6f;
        const float step = stick * kRowsPerSecond * kGridRowHeight * frame_seconds();
        const float at = std::clamp(g_view.grid_scroll.value() + step, 0.0f, limit);
        g_view.grid_scroll.jump(at);
        note_scrolling();

        const int rows = grid_rows(items.size());
        const int first_visible = static_cast<int>(at) / kGridRowHeight;
        const int last_visible =
            std::min(rows - 1, (static_cast<int>(at) + kGridViewHeight - 1) / kGridRowHeight);
        const int column = g_view.grid_index % kGridColumns;
        const int wanted = std::clamp(g_view.grid_index / kGridColumns, first_visible,
                                      std::max(first_visible, last_visible));
        g_view.grid_index = std::min(last, wanted * kGridColumns + column);
        return;
    }

    /* Keep the focused row on screen, by asking the spring rather than by
       moving the list: the cursor arrives, the page follows it. */
    const int row = g_view.grid_index / kGridColumns;
    const float top = static_cast<float>(row * kGridRowHeight);
    const float bottom = top + static_cast<float>(kGridRowHeight);
    const float view_height = static_cast<float>(kGridViewHeight);
    if (top < g_view.grid_scroll.target())
    {
        g_view.grid_scroll.aim(top);
        note_scrolling();
    }
    else if (bottom > g_view.grid_scroll.target() + view_height)
    {
        g_view.grid_scroll.aim(std::min(limit, bottom - view_height));
        note_scrolling();
    }
}

/* Search owns input while its overlay is open. */
void open_search() noexcept
{
    auto &search = g_view.search;
    search.open = true;
    search.keyboard_notice.clear();
    search.draft = search.committed;
    search.reveal.jump(0.0f);
    search.reveal.aim(1.0f);
    g_view.sidebar_focused = false;
    ask_keyboard(View::Prompt::search);
    restart_focus();
}

void close_search() noexcept
{
    View::Search &search = g_view.search;
    search.open = false;
    search.reveal.jump(0.0f);
    restart_focus();
}

/* Submit the text returned by the system keyboard in the current library. */
void run_live_search(const SearchScope &scope) noexcept
{
    View::Search &search = g_view.search;
    search.pending = 0;
    if (search.draft == search.committed && search.committed_scope == scope.library_id &&
        search.committed_types == scope.types && !search.refresh_requested)
        return;
    search.committed_scope = scope.library_id;
    search.committed_types = scope.types;
    search.refresh_requested = false;
    search.committed = search.draft;
    search.result_index = 0;
    search.scroll.jump(0.0f);
    for (auto &offset : search.rail)
        offset.jump(0.0f);
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        g_shared.search_items.clear();
        g_shared.search_error.clear();
        ++g_shared.search_epoch;
        g_shared.search_loading = !search.committed.empty();
    }
    if (!search.committed.empty())
        submit_search(search.committed, scope.library_id, scope.types);
}

/* Returns true when the search consumed the frame's input. */
bool handle_search_input(const SearchScope &scope, const std::vector<jellyfin::Item> &results,
                         const std::vector<search_sections::Section> &sections) noexcept
{
    auto &search = g_view.search;
    if (!search.open)
        return false;
    if (search.pending > 0 && --search.pending == 0)
        run_live_search(scope);
    if (pad::pressed(pad::Button::circle))
    {
        close_search();
        return true;
    }
    if (pad::pressed(pad::Button::triangle))
    {
        ask_keyboard(View::Prompt::search);
        return true;
    }
    if (sections.empty())
    {
        if (pad::pressed(pad::Button::up) || pad::pressed(pad::Button::cross))
            ask_keyboard(View::Prompt::search);
        return true;
    }
    auto [row, column] = search_sections::locate(sections, search.result_index);
    (void)column;
    const int old = search.result_index;
    if (pad::pressed(pad::Button::right))
        search.result_index = search_sections::move(sections, old, 1, 0);
    else if (pad::pressed(pad::Button::left))
        search.result_index = search_sections::move(sections, old, -1, 0);
    else if (pad::pressed(pad::Button::down))
        search.result_index = search_sections::move(sections, old, 0, 1);
    else if (pad::pressed(pad::Button::up))
    {
        if (row == 0)
        {
            ask_keyboard(View::Prompt::search);
            return true;
        }
        search.result_index = search_sections::move(sections, old, 0, -1);
    }
    else if (pad::pressed(pad::Button::cross))
    {
        open_detail(results[static_cast<std::size_t>(search.result_index)]);
        close_search();
        return true;
    }
    if (old != search.result_index)
        restart_focus();
    const auto position = search_sections::locate(sections, search.result_index);
    const auto &section = sections[static_cast<std::size_t>(position.first)];
    auto &rail = search.rail[static_cast<std::size_t>(section.kind)];
    const int x = position.second * (section.width() + 28);
    if (x < rail.target())
        rail.aim(x);
    else if (x + section.width() > rail.target() + kContentWidth - 24)
        rail.aim(x + section.width() - kContentWidth + 24);
    const int height = gfx::kHeight - search_results_top() - 80;
    const auto &last = sections.back();
    const float limit = static_cast<float>(std::max(0, last.top + last.extent() - height));
    if (section.top < search.scroll.target())
        search.scroll.aim(section.top);
    else if (section.top + section.extent() > search.scroll.target() + height)
        search.scroll.aim(
            std::min(limit, static_cast<float>(section.top + section.extent() - height)));
    return true;
}

void handle_setup_input(Screen screen) noexcept
{
    const int options = (screen == Screen::server_setup) ? 2 : g_view.credential_form ? 4 : 3;
    if (screen == Screen::sign_in && g_view.credential_form)
    {
        if (pad::pressed(pad::Button::circle))
        {
            g_view.credential_form = false;
            g_view.pending_password.clear();
            g_view.show_password = false;
            g_view.setup_index = 1;
            return;
        }
        if (pad::pressed(pad::Button::cross))
        {
            if (g_view.setup_index == 0)
                ask_keyboard(View::Prompt::username);
            else if (g_view.setup_index == 1)
                ask_keyboard(View::Prompt::password);
            else if (g_view.setup_index == 2)
                g_view.show_password = !g_view.show_password;
            else if (g_view.pending_username.empty())
            {
                g_view.setup_index = 0;
                set_status(Screen::sign_in, "Sign in", "Enter your username first.");
            }
            else
            {
                set_status(Screen::connecting, "Signing in");
                submit(Request::password_login, g_view.pending_username, g_view.pending_password);
                g_view.show_password = false;
            }
            return;
        }
    }
    if (pad::pressed(pad::Button::down) && g_view.setup_index + 1 < options)
        ++g_view.setup_index;
    else if (pad::pressed(pad::Button::up) && g_view.setup_index > 0)
        --g_view.setup_index;
    else if (pad::pressed(pad::Button::cross))
    {
        if (screen == Screen::server_setup)
        {
            if (g_view.setup_index == 0)
                ask_keyboard(View::Prompt::host);
            else
            {
                connect_server_draft();
            }
        }
        else
        {
            if (g_view.setup_index == 0)
            {
                set_status(Screen::connecting, "Starting Quick Connect");
                submit(Request::quick_connect);
            }
            else if (g_view.setup_index == 1)
            {
                g_view.credential_form = true;
                g_view.setup_index = 0;
                g_view.show_password = false;
            }
            else
                set_status(Screen::server_setup, "Choose your server");
        }
    }
}

void handle_detail_input(const jellyfin::Item &item, const std::vector<jellyfin::Item> &episodes,
                         const jellyfin::Item &next_up) noexcept
{
    /* The same list the buttons are drawn from, so the two cannot disagree. */
    const std::vector<DetailButton> buttons = detail_actions(item);
    const int actions = static_cast<int>(buttons.size());
    const bool playable = actions > 0;
    const auto action_at = [&buttons](int index)
    {
        return index >= 0 && index < static_cast<int>(buttons.size())
                   ? buttons[static_cast<std::size_t>(index)].action
                   : DetailAction::play;
    };

    /* The sheet holds the buttons while it is open: scroll, or close. */
    if (g_view.details_open)
    {
        if (pad::pressed(pad::Button::circle) || pad::pressed(pad::Button::cross) ||
            action_at(g_view.detail_action) != DetailAction::details)
            g_view.details_open = false;
        else if (pad::pressed(pad::Button::down))
            ++g_view.details_scroll; /* clamped where it is drawn */
        else if (pad::pressed(pad::Button::up) && g_view.details_scroll > 0)
            --g_view.details_scroll;
        return;
    }

    /* Do not focus cast while series or season rows are still loading. */
    const bool has_next_up = !next_up.id.empty();
    const bool has_cast = !item.people.empty();
    if (!playable && g_view.detail_action >= 0 && !episodes.empty())
        g_view.detail_action = -1;
    if (g_view.detail_action == kDetailNextUp) /* left over from an earlier layout */
        g_view.detail_action = !episodes.empty() ? -1 : 0;

    if (pad::pressed(pad::Button::circle))
    {
        if (!go_back())
        {
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            g_shared.screen = Screen::home;
        }
        return;
    }
    if (has_next_up && pad::pressed(pad::Button::triangle))
    {
        /* Straight into the episode, from where it was left. The full record
           with its media sources is fetched first, as the next-episode button
           does. */
        {
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            g_shared.pending_play_ready = false;
            g_shared.pending_play_resume = true;
        }
        submit(Request::play_sibling, next_up.id);
        return;
    }
    if (has_next_up && pad::pressed(pad::Button::square))
    {
        open_detail(next_up.id);
        return;
    }

    /* An episode's page opens its season strip at the episode itself. */
    if (item.type == "Episode" && g_view.detail_strip_placed_for != item.id && !episodes.empty())
    {
        g_view.detail_strip_placed_for = item.id;
        for (std::size_t i = 0; i < episodes.size(); ++i)
            if (episodes[i].id == item.id)
            {
                g_view.detail_episode = static_cast<int>(i);
                const StripShape shape = strip_shape(episodes);
                g_view.detail_scroll.jump(
                    static_cast<float>(std::max(0, static_cast<int>(i) - 1) * shape.pitch));
            }
    }

    if (g_view.detail_action == kDetailCast)
    {
        const int last = static_cast<int>(item.people.size()) - 1;
        if (!has_cast)
            g_view.detail_action = !episodes.empty() ? -1 : 0;
        else if (pad::pressed(pad::Button::right) && g_view.detail_cast_index < last)
            ++g_view.detail_cast_index;
        else if (pad::pressed(pad::Button::left) && g_view.detail_cast_index > 0)
            --g_view.detail_cast_index;
        else if (pad::pressed(pad::Button::up))
        {
            g_view.detail_action = !episodes.empty() ? -1 : playable ? 0 : kDetailCast;
            restart_focus();
        }
        const float card_left = static_cast<float>(g_view.detail_cast_index * kCastPitch);
        const float visible = static_cast<float>(kContentRight - kSafeX - kCastPhoto);
        if (card_left - g_view.detail_cast_scroll.target() < 0.0f)
            g_view.detail_cast_scroll.aim(card_left);
        else if (card_left - g_view.detail_cast_scroll.target() > visible)
            g_view.detail_cast_scroll.aim(card_left - visible);
        return;
    }

    if (g_view.detail_action == kDetailVersion)
    {
        const int last = static_cast<int>(item.sources.size()) - 1;
        if (!has_versions(item))
            g_view.detail_action = 0;
        else if (pad::pressed(pad::Button::right) && g_view.detail_source < last)
            ++g_view.detail_source;
        else if (pad::pressed(pad::Button::left) && g_view.detail_source > 0)
            --g_view.detail_source;
        else if (pad::pressed(pad::Button::down) || pad::pressed(pad::Button::cross))
            g_view.detail_action = 0; /* back to Play, on the version chosen */
        return;
    }

    if (g_view.detail_action >= 0 && pad::pressed(pad::Button::down) &&
        (!episodes.empty() || has_cast))
    {
        g_view.detail_action = !episodes.empty() ? -1 : kDetailCast; /* focus moves to the rows */
        restart_focus();
        return;
    }
    if (g_view.detail_action < 0)
    {
        const int last = static_cast<int>(episodes.size()) - 1;
        if (pad::pressed(pad::Button::right) && g_view.detail_episode < last)
            ++g_view.detail_episode;
        else if (pad::pressed(pad::Button::left) && g_view.detail_episode > 0)
            --g_view.detail_episode;
        else if (pad::pressed(pad::Button::up) && playable)
            g_view.detail_action = 0;
        else if (pad::pressed(pad::Button::down) && has_cast)
        {
            g_view.detail_action = kDetailCast;
            restart_focus();
        }
        else if (pad::pressed(pad::Button::cross) && !episodes.empty())
            open_detail(episodes[static_cast<std::size_t>(g_view.detail_episode)]);

        const StripShape shape = strip_shape(episodes);
        const float card_left = static_cast<float>(g_view.detail_episode * shape.pitch);
        const float visible = static_cast<float>(kContentWidth - shape.card_w);
        if (card_left - g_view.detail_scroll.target() < 0.0f)
            g_view.detail_scroll.aim(card_left);
        else if (card_left - g_view.detail_scroll.target() > visible)
            g_view.detail_scroll.aim(card_left - visible);
        return;
    }

    if (pad::pressed(pad::Button::up) && has_versions(item))
    {
        g_view.detail_action = kDetailVersion;
        return;
    }
    if (pad::pressed(pad::Button::right) && g_view.detail_action + 1 < actions)
        ++g_view.detail_action;
    else if (pad::pressed(pad::Button::left) && g_view.detail_action > 0)
        --g_view.detail_action;
    else if (pad::pressed(pad::Button::cross) && playable)
    {
        switch (action_at(g_view.detail_action))
        {
        case DetailAction::play:
            /* The version picked on the detail screen, and the tracks the
               server chose from the user's preferences, go through together. */
            begin_playback(item, g_view.detail_source, item.resume_ticks / 10000000.0);
            break;
        case DetailAction::restart:
            begin_playback(item, g_view.detail_source, 0.0);
            break;
        case DetailAction::watched:
            submit(Request::mark_played, item.id);
            break;
        case DetailAction::details:
        {
            g_view.details_open = true;
            g_view.details_scroll = 0;
            /* Ask the way Play asks: the selected version, its default tracks. */
            const auto &source = item.sources[static_cast<std::size_t>(
                std::clamp(g_view.detail_source, 0, static_cast<int>(item.sources.size()) - 1))];
            {
                std::lock_guard<std::mutex> guard(g_shared.mutex);
                g_shared.delivery_request = jellyfin::PlaybackRequest{};
                g_shared.delivery_request.item_id = item.id;
                g_shared.delivery_request.media_source_id = source.id;
                g_shared.delivery_request.audio_index = source.default_audio;
                g_shared.delivery_request.subtitle_index = starting_subtitle(source);
                g_shared.delivery_key = item.id + "|" + source.id;
                g_shared.delivery_ready = false;
                g_shared.delivery_lines.clear();
                g_shared.request = Request::probe_delivery;
            }
            g_request_wake.notify_one();
            break;
        }
        case DetailAction::series:
            open_detail(item.series_id);
            break;
        }
    }
}

/* Shared playback entry point for detail pages and autoplay. */
/* Use the server default subtitle track, subject to picture-subtitle preferences. */
int starting_subtitle(const jellyfin::MediaSource &source) noexcept
{
    const int chosen = source.default_subtitle;
    if (chosen < 0 || subtitle_style::wrap(config::current().subtitle_look.picture, 2) != 1)
        return chosen;
    const jellyfin::Track *current = nullptr;
    for (const jellyfin::Track &track : source.subtitle_tracks)
        if (track.index == chosen)
            current = &track;
    if (current == nullptr || current->is_text)
        return chosen;
    for (const jellyfin::Track &track : source.subtitle_tracks)
        if (track.is_text && track.language == current->language &&
            track.is_forced == current->is_forced)
            return track.index;
    return chosen;
}

void begin_playback(const jellyfin::Item &item, int source_index, double start_seconds) noexcept
{
    jellyfin::PlaybackRequest request;
    request.item_id = item.id;
    request.start_seconds = start_seconds;
    if (!item.sources.empty())
    {
        const auto &source = item.sources[static_cast<std::size_t>(
            std::clamp(source_index, 0, static_cast<int>(item.sources.size()) - 1))];
        request.media_source_id = source.id;
        request.audio_index = source.default_audio;
        request.subtitle_index = starting_subtitle(source);
    }
    /* Room for the stream first, before it allocates anything. */
    images::set_playback(true);
    if (!player::start(request, item.name, item.runtime_minutes * 60.0))
    {
        images::set_playback(false);
        return;
    }

    /* The panel offers the tracks of the version actually chosen. */
    g_view.playing_source = item.sources.empty()
                                ? jellyfin::MediaSource{}
                                : item.sources[static_cast<std::size_t>(std::clamp(
                                      source_index, 0, static_cast<int>(item.sources.size()) - 1))];
    set_playing_title(item);
    g_view.playing_item = item;
    if (item.type == "Episode")
    {
        trace::mark(item.credits_start_seconds >= 0.0
                        ? "autoplay: credits marker at " +
                              std::to_string(static_cast<int>(item.credits_start_seconds)) + " s"
                        : "autoplay: no credits marker; fallback lead " +
                              std::to_string(static_cast<int>(autoplay::kCardLead)) + " s");
        trace::mark(
            item.intro_start_seconds >= 0.0 && item.intro_end_seconds > item.intro_start_seconds
                ? "intro: marker " + std::to_string(static_cast<int>(item.intro_start_seconds)) +
                      "-" + std::to_string(static_cast<int>(item.intro_end_seconds)) + " s"
                : "intro: no usable marker");
    }
    g_view.intro_visible = false;
    g_view.intro_dismissed = false;
    g_view.intro_seek_failed = false;
    g_view.intro_in.jump(0.0f);
    g_view.end_choice = 0;
    g_view.finished = false;
    /* A title chosen by hand starts the count again; the autoplay path marks
       itself instead (advance_autoplay). */
    if (!g_view.changing_episode)
        g_view.autoplay.chosen_by_hand();
    else
        g_view.autoplay.reset();
    g_view.up_next = jellyfin::Item{};
    g_view.up_next_in.jump(0.0f);
    g_view.osd_visible = true;
    g_view.osd_dismissed = false;
    g_view.osd_idle = slopfin::app::now_us();
    g_view.osd_focus = View::OsdFocus::bar;
    g_view.osd_button = 0;
    g_view.episode_choice = 1;
    g_view.panel_open = false;
    g_view.seek_target = -1.0;
    g_view.seek_commit = 0;
    g_view.changing_episode = false;
    if (config::current().trigger_feedback)
        pad::set_trigger_feel(pad::TriggerFeel::seek);
    g_view.has_previous = false;
    g_view.has_next = false;
    g_view.previous_id.clear();
    g_view.next_id.clear();

    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        g_shared.siblings.clear();
        g_shared.sibling_index = -1;
        g_shared.screen = Screen::playing;
    }
    /* Which episodes sit either side of this one is a question for the server,
       and the answer is only needed once playback is already running. */
    if (item.type == "Episode" && !item.series_id.empty())
        submit_siblings(item.series_id, item.id);
}

/* Fetch sibling playback metadata on the worker before switching episodes. */
void play_adjacent_episode(int direction) noexcept
{
    const std::string &wanted = direction < 0 ? g_view.previous_id : g_view.next_id;
    trace::mark("player: adjacent " + std::to_string(direction) + " wanted " +
                (wanted.empty() ? "(none)" : wanted) + (g_view.changing_episode ? " (busy)" : ""));
    if (wanted.empty() || g_view.changing_episode)
        return;
    g_view.changing_episode = true;
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        g_shared.sibling_loading = true;
        g_shared.pending_play_ready = false;
    }
    submit(Request::play_sibling, wanted);
}

/* Drive autoplay from playback state, even while another control owns input. */
void advance_autoplay(const player::Status &status) noexcept
{
    autoplay::Frame frame;
    frame.playing_episode =
        status.state == player::State::playing || status.state == player::State::ended;
    frame.has_next = g_view.has_next && !g_view.changing_episode;
    frame.ended = status.state == player::State::ended;
    frame.remaining = status.duration_seconds > 1.0
                          ? std::max(0.0, status.duration_seconds - status.position_seconds)
                          : 1e9;
    frame.credits_known = g_view.playing_item.credits_start_seconds >= 0.0;
    frame.credits_started =
        frame.credits_known && status.position_seconds >= g_view.playing_item.credits_start_seconds;
    frame.user_pressed = pad::any_pressed();
    const bool was_asking = g_view.autoplay.asking();
    autoplay::Choices choices = config::current().autoplay_look;
    choices.enabled = config::autoplay_enabled(g_view.playing_item.series_id);
    /* One line when the state actually changes, so what the machine saw is
       recoverable from the trace rather than guessed at. */
    static bool s_card = false;
    if (g_view.autoplay.card_visible() != s_card)
    {
        s_card = g_view.autoplay.card_visible();
        trace::mark(std::string{"autoplay: card "} + (s_card ? "up" : "away") + ", next " +
                    (frame.has_next ? "yes" : "no") + ", remaining " +
                    std::to_string(static_cast<int>(frame.remaining)) + ", ended " +
                    (frame.ended ? "yes" : "no"));
    }
    switch (g_view.autoplay.step(frame_seconds(), frame, choices))
    {
    case autoplay::Action::play_next:
        g_view.autoplay.played_automatically();
        g_view.finished = false;
        play_adjacent_episode(1);
        break;
    case autoplay::Action::ask_still_there:
        /* Nobody answered. Stop, so the server is not left transcoding to an
           empty room, and leave the title's page up. */
        g_view.autoplay.still_there();
        player::stop();
        subtitles::clear();
        pad::set_trigger_feel(pad::TriggerFeel::none);
        {
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            g_shared.screen = Screen::detail;
        }
        break;
    case autoplay::Action::none:
        break;
    }
    /* Nothing plays behind the question: an episode carrying on under it is
       exactly the thing being asked about. */
    if (!was_asking && g_view.autoplay.asking() && !player::paused())
        player::set_paused(true);

    /* The card needs the episode itself, which the worker already has -- and
       so does the screen that follows the title. */
    if (g_view.autoplay.card_visible() || g_view.autoplay.asking() ||
        status.state == player::State::ended)
    {
        if (g_view.up_next.id != g_view.next_id)
        {
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            const int at = g_shared.sibling_index;
            if (at >= 0 && at + 1 < static_cast<int>(g_shared.siblings.size()))
                g_view.up_next = g_shared.siblings[static_cast<std::size_t>(at + 1)];
        }
    }
}

void handle_player_input() noexcept
{
    const player::Status status = player::status();
    note_subtitle_state(status);
    /*
     * The question owns the input while it is up: one button says yes, and
     * anything else leaves it asking.
     */
    if (g_view.autoplay.asking())
    {
        if (pad::pressed(pad::Button::left) || pad::pressed(pad::Button::right))
            g_view.ask_choice = g_view.ask_choice == 0 ? 1 : 0;
        else if (pad::pressed(pad::Button::cross) && g_view.ask_choice == 1)
        {
            g_view.autoplay.still_there();
            player::stop();
            subtitles::clear();
            pad::set_trigger_feel(pad::TriggerFeel::none);
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            g_shared.screen = Screen::detail;
        }
        else if (pad::pressed(pad::Button::cross))
        {
            g_view.autoplay.still_there();
            if (status.state == player::State::ended)
                play_adjacent_episode(1);
            else if (player::paused())
                player::set_paused(false);
            g_view.osd_visible = true;
            g_view.osd_idle = slopfin::app::now_us();
        }
        else if (pad::pressed(pad::Button::circle))
        {
            g_view.autoplay.still_there();
            player::stop();
            subtitles::clear();
            pad::set_trigger_feel(pad::TriggerFeel::none);
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            g_shared.screen = Screen::detail;
        }
        return;
    }
    /*
     * Once the title has finished, the screen belongs to the end card: it is
     * the only thing there, so it takes the input outright.
     */
    if (g_view.finished && !g_view.changing_episode)
    {
        const std::vector<EndChoice> choices = end_choices();
        const int count = static_cast<int>(choices.size());
        g_view.end_choice = std::clamp(g_view.end_choice, 0, count - 1);
        if (pad::pressed(pad::Button::right) && g_view.end_choice + 1 < count)
            ++g_view.end_choice;
        else if (pad::pressed(pad::Button::left) && g_view.end_choice > 0)
            --g_view.end_choice;
        else if (pad::pressed(pad::Button::circle))
        {
            player::stop();
            subtitles::clear();
            pad::set_trigger_feel(pad::TriggerFeel::none);
            g_view.finished = false;
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            g_shared.screen = Screen::detail;
        }
        else if (pad::pressed(pad::Button::cross))
        {
            switch (choices[static_cast<std::size_t>(g_view.end_choice)].kind)
            {
            case EndChoice::Kind::play_next:
                g_view.autoplay.played_automatically();
                g_view.finished = false;
                play_adjacent_episode(1);
                break;
            case EndChoice::Kind::again:
            {
                jellyfin::PlaybackRequest again = player::current_request();
                again.start_seconds = 0.0;
                g_view.autoplay.chosen_by_hand();
                g_view.finished = false;
                (void)player::start(again, g_view.playing_item.name,
                                    g_view.playing_item.runtime_minutes * 60.0);
                break;
            }
            case EndChoice::Kind::leave:
                player::stop();
                subtitles::clear();
                pad::set_trigger_feel(pad::TriggerFeel::none);
                g_view.finished = false;
                std::lock_guard<std::mutex> guard(g_shared.mutex);
                g_shared.screen = Screen::detail;
                break;
            }
        }
        return;
    }

    /* Cross/Circle belong to the intro over a bare picture. With controls
       open, the card advertises Square so Cross keeps its pause/select role. */
    if (g_view.intro_visible && !g_view.panel_open)
    {
        if ((!g_view.osd_visible && pad::pressed(pad::Button::cross)) ||
            (g_view.osd_visible && pad::pressed(pad::Button::square)))
        {
            const double target = g_view.playing_item.intro_end_seconds;
            if (player::seek(target))
            {
                g_view.intro_dismissed = true;
                g_view.intro_visible = false;
                g_view.seek_restart = true;
                g_view.intro_seek_failed = false;
                trace::mark("intro: skipped to " + std::to_string(target) + " s");
            }
            else
            {
                g_view.intro_seek_failed = true;
                trace::mark("intro: seek failed; prompt remains available");
            }
            /* Consume Cross even on failure; it must not also pause playback. */
            return;
        }
        else if (!g_view.osd_visible && pad::pressed(pad::Button::circle))
        {
            g_view.intro_dismissed = true;
            g_view.intro_visible = false;
            trace::mark("intro: dismissed");
            return;
        }
    }

    /* The next-episode card owns Cross/Circle only while the normal controls are hidden. */
    if (g_view.autoplay.card_visible() && !g_view.panel_open)
    {
        /* With the timeline up, Circle belongs to the timeline first. The old
           order dismissed Next Episode while the viewer was only trying to
           hide the controls. Once the OSD is away, Circle dismisses the card. */
        if (pad::pressed(pad::Button::circle) && !g_view.osd_visible)
        {
            g_view.autoplay.dismiss();
            return;
        }
        if (pad::pressed(pad::Button::cross) && !g_view.osd_visible)
        {
            g_view.autoplay.played_automatically();
            play_adjacent_episode(1);
            return;
        }
    }
    if (g_view.osd_idle == 0)
        g_view.osd_idle = slopfin::app::now_us();
    if (g_view.flash > 0)
        --g_view.flash;

    /* Every seek restarts the stream, so a run of presses is gathered and
       committed once the user stops pressing. */
    if (g_view.seek_commit > 0 && --g_view.seek_commit == 0 && g_view.seek_target >= 0.0)
    {
        const double target = g_view.seek_target;
        /* Returning to the scrub origin cancels the seek and avoids restarting the stream. */
        const bool moved = std::fabs(g_view.seek_pending) >= 0.5;
        const bool resume = g_view.seek_was_playing;
        g_view.seek_target = -1.0;
        g_view.seek_origin = -1.0;
        g_view.seek_pending = 0.0;
        g_view.seek_was_playing = false;
        if (moved)
        {
            g_view.seek_restart = true;
            (void)player::seek(target);
        }
        /* Put the picture back the way the skipping found it. */
        if (resume)
            player::set_paused(false);
        g_view.osd_idle = slopfin::app::now_us();
    }

    const auto wake = []()
    {
        g_view.osd_visible = true;
        g_view.osd_dismissed = false;
        g_view.osd_idle = slopfin::app::now_us();
    };
    /* `fixed` is a flat step; otherwise the curve decides from what has already
       gathered. The origin is captured once so the picture running on
       underneath does not drag the target along with it. */
    const auto nudge = [&status](double amount)
    {
        if (g_view.seek_origin < 0.0)
        {
            g_view.seek_origin = status.position_seconds;
            g_view.seek_pending = 0.0;
            /* Pause the picture during scrubbing to keep the origin stable. */
            g_view.seek_was_playing = !status.paused;
            if (g_view.seek_was_playing)
                player::set_paused(true);
        }
        g_view.seek_pending += amount;
        const double ceiling = status.duration_seconds > 5.0 ? status.duration_seconds - 5.0 : 1e9;
        g_view.seek_target = std::clamp(g_view.seek_origin + g_view.seek_pending, 0.0, ceiling);
        /* Commit scrubbing after a short interval without seek input. */
        g_view.seek_commit = 45;
    };

    if (g_view.panel_open)
    {
        const int rows = panel_rows();
        /* Headings are shown but never landed on. */
        const auto skip = [rows](int from, int direction)
        {
            int at = std::clamp(from, 0, rows - 1);
            while (at > 0 && at < rows - 1 && panel_row_is_heading(at))
                at += direction;
            return std::clamp(at, 0, rows - 1);
        };
        /* Timing adjustments remain visible while key-repeat changes the offset. */
        const TimingRow timing = timing_row(g_view.panel_index);
        const bool adjusting = timing == TimingRow::late || timing == TimingRow::early;
        g_view.timing_held =
            adjusting && pad::held(pad::Button::cross) ? g_view.timing_held + 1 : 0;
        const bool repeat = g_view.timing_held > 20 && (g_view.timing_held - 20) % 3 == 0;
        if (adjusting && (pad::pressed(pad::Button::cross) || repeat))
        {
            g_view.subtitle_offset =
                subtitle_timing::nudge(g_view.subtitle_offset, timing == TimingRow::late);
            g_view.subtitle_offset_dirty = true;
        }
        else if (timing == TimingRow::reset && pad::pressed(pad::Button::cross))
        {
            g_view.subtitle_offset = 0.0;
            g_view.subtitle_offset_dirty = true;
        }
        else if (pad::pressed(pad::Button::down))
            g_view.panel_index = skip(g_view.panel_index + 1, 1);
        else if (pad::pressed(pad::Button::up))
            g_view.panel_index = skip(g_view.panel_index - 1, -1);
        else if (pad::pressed(pad::Button::left) || pad::pressed(pad::Button::right))
        {
            /* Sideways leaves the list and goes back to the row of buttons. */
            g_view.panel_open = false;
        }
        else if (pad::pressed(pad::Button::cross))
        {
            apply_panel_choice();
            g_view.panel_open = false;
        }
        else if (pad::pressed(pad::Button::circle) || pad::pressed(pad::Button::triangle) ||
                 pad::pressed(pad::Button::options))
        {
            /* Closing the list leaves the row it belongs to still in control,
               so the next press carries on along it. */
            g_view.panel_open = false;
            g_view.osd_focus = View::OsdFocus::modules;
        }
        if (!g_view.panel_open)
            commit_subtitle_offset();
        wake();
        return;
    }

    const std::vector<PlayerButton> buttons = player_buttons();

    /* Circle first hides controls; a subsequent press leaves playback. */
    if (pad::pressed(pad::Button::circle))
    {
        /* Circle is the only way out of module control, and the only way to
           put the controls away. Each press undoes one thing. */
        if (g_view.osd_focus != View::OsdFocus::bar && g_view.osd_visible)
        {
            g_view.osd_focus = View::OsdFocus::bar;
            wake();
            return;
        }
        if (g_view.osd_visible || g_view.panel_open)
        {
            g_view.panel_open = false;
            g_view.osd_visible = false;
            g_view.osd_dismissed = true;
            return;
        }
        player::stop();
        subtitles::clear();
        pad::set_trigger_feel(pad::TriggerFeel::none);
        g_view.finished = false;
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        trace::mark("player: left playback for the title page");
        g_shared.screen = Screen::detail;
        return;
    }

    if (pad::pressed(pad::Button::cross))
    {
        if (g_view.osd_visible && g_view.osd_focus == View::OsdFocus::episodes)
        {
            play_adjacent_episode(g_view.episode_choice == 0 ? -1 : 1);
            wake();
            return;
        }
        if (g_view.osd_visible && g_view.osd_focus == View::OsdFocus::modules && !buttons.empty())
        {
            const PlayerButton &button = buttons[static_cast<std::size_t>(
                std::clamp(g_view.osd_button, 0, static_cast<int>(buttons.size()) - 1))];
            switch (button.action)
            {
            case PlayerAction::subtitles:
            case PlayerAction::audio:
            case PlayerAction::quality:
                g_view.panel_kind = button.action == PlayerAction::subtitles ? 0
                                    : button.action == PlayerAction::audio   ? 1
                                                                             : 2;
                g_view.panel_anchor = g_view.osd_button;
                g_view.panel_index = panel_current_row(status);
                g_view.panel_open = true;
                break;
            case PlayerAction::autoplay:
            {
                const std::string &series = g_view.playing_item.series_id;
                const bool enabled = !config::autoplay_enabled(series);
                config::set_autoplay_enabled(series, enabled);
                config::save();
                trace::mark(std::string{"autoplay: series override "} + (enabled ? "on " : "off ") +
                            series);
                break;
            }
            case PlayerAction::debug:
                g_view.show_debug = !g_view.show_debug;
                break;
            }
            wake();
            return;
        }
        player::set_paused(!status.paused);
        g_view.flash = 40;
        wake();
        return;
    }

    /* Keep directional input inside the selected module until it is dismissed. */
    if (g_view.osd_focus == View::OsdFocus::modules &&
        (pad::pressed(pad::Button::up) || pad::pressed(pad::Button::down)))
    {
        wake();
        return;
    }

    if (pad::pressed(pad::Button::up))
    {
        /* Up always means the timeline, from wherever else it is pressed. */
        if (g_view.osd_visible)
            g_view.osd_focus = View::OsdFocus::bar;
        wake();
        return;
    }
    if (pad::pressed(pad::Button::down))
    {
        /* Down moves focus into controls below the timeline. */
        if (g_view.osd_visible && (g_view.has_previous || g_view.has_next))
        {
            g_view.osd_focus = View::OsdFocus::episodes;
            /* Land on one that is actually offered. */
            if (g_view.episode_choice == 0 && !g_view.has_previous)
                g_view.episode_choice = 1;
            else if (g_view.episode_choice == 1 && !g_view.has_next)
                g_view.episode_choice = 0;
        }
        wake();
        return;
    }

    /* Continuous seeking uses trigger travel or directional hold duration. */
    {
        const float left_travel = pad::trigger_left();
        const float right_travel = pad::trigger_right();
        constexpr float kTriggerFloor = 0.10f;
        const bool trigger_back = left_travel > kTriggerFloor;
        const bool trigger_on = right_travel > kTriggerFloor || trigger_back;
        /* A held direction scrubs too, but only once the auto-repeat would
           have started, so a tap stays a tap. */
        const bool held_back = pad::held(pad::Button::left);
        const bool held_on = pad::held(pad::Button::right) || held_back;
        const bool steering = g_view.osd_focus == View::OsdFocus::bar || !g_view.osd_visible;

        if (steering && (trigger_on || held_on))
        {
            ++g_view.seek_hold;
            constexpr int kHoldBeforeScrub = 20;
            const bool backward =
                trigger_on ? trigger_back && left_travel >= right_travel : held_back;
            /* Tapped triggers seek one second; tapped directions seek ten. */
            if (trigger_on && g_view.seek_hold == 1)
            {
                nudge(backward ? -seek::kTriggerTap : seek::kTriggerTap);
                wake();
                return;
            }
            if (g_view.seek_hold > kHoldBeforeScrub)
            {
                const double travel =
                    trigger_on ? std::max(left_travel, right_travel) - kTriggerFloor : 0.62;
                const double scaled = travel / (1.0 - kTriggerFloor);
                const double rate = seek::seek_rate(scaled, g_view.seek_hold, g_view.seek_pending);
                nudge((backward ? -rate : rate) * frame_seconds());
                wake();
                return;
            }
        }
        else
        {
            g_view.seek_hold = 0;
        }
    }

    if (pad::pressed(pad::Button::left) || pad::pressed(pad::Button::right))
    {
        const bool forward = pad::pressed(pad::Button::right);
        if (g_view.osd_visible && g_view.osd_focus == View::OsdFocus::modules && !buttons.empty())
        {
            g_view.osd_button = std::clamp(g_view.osd_button + (forward ? 1 : -1), 0,
                                           static_cast<int>(buttons.size()) - 1);
        }
        else if (g_view.osd_visible && g_view.osd_focus == View::OsdFocus::episodes)
        {
            const int wanted = forward ? 1 : 0;
            if (wanted == 0 ? g_view.has_previous : g_view.has_next)
                g_view.episode_choice = wanted;
        }
        else
        {
            /* A tap is a small, exact step. Holding past the threshold above
               never reaches here, because the scrub returns first. */
            nudge(forward ? seek::kSeekTap : -seek::kSeekTap);
        }
        wake();
        return;
    }

    /* The shoulders step a flat two minutes, so a known distance stays
       predictable however many times it is pressed. */
    if (pad::pressed(pad::Button::r1))
    {
        nudge(seek::kShoulderStep);
        wake();
        return;
    }
    if (pad::pressed(pad::Button::l1))
    {
        nudge(-seek::kShoulderStep);
        wake();
        return;
    }

    if (pad::pressed(pad::Button::triangle))
    {
        /* Triangle opens playback controls with the module row focused. */
        if (!g_view.osd_visible)
        {
            g_view.osd_focus = View::OsdFocus::modules;
            if (!buttons.empty())
                g_view.osd_button =
                    std::clamp(g_view.osd_button, 0, static_cast<int>(buttons.size()) - 1);
            wake();
            return;
        }
        /*
         * The one gesture that reaches the settings row, and the same one that
         * leaves it. The row draws the shape, so nothing has to be remembered.
         */
        g_view.osd_focus = g_view.osd_focus == View::OsdFocus::modules ? View::OsdFocus::bar
                                                                       : View::OsdFocus::modules;
        if (g_view.osd_focus == View::OsdFocus::modules && !buttons.empty())
            g_view.osd_button =
                std::clamp(g_view.osd_button, 0, static_cast<int>(buttons.size()) - 1);
        wake();
        return;
    }

    /* Options is still the direct route to the track lists. */
    if (pad::pressed(pad::Button::options))
    {
        g_view.panel_kind = g_view.playing_source.subtitle_tracks.empty() ? 1 : 0;
        for (std::size_t i = 0; i < buttons.size(); ++i)
            if ((g_view.panel_kind == 0 && buttons[i].action == PlayerAction::subtitles) ||
                (g_view.panel_kind == 1 && buttons[i].action == PlayerAction::audio))
                g_view.panel_anchor = static_cast<int>(i);
        g_view.osd_focus = View::OsdFocus::modules;
        g_view.osd_button = g_view.panel_anchor;
        g_view.panel_index = panel_current_row(status);
        g_view.panel_open = true;
        wake();
        return;
    }

    /* Square is the quick way to put subtitles on or off again. */
    if (pad::pressed(pad::Button::square))
    {
        const auto &tracks = g_view.playing_source.subtitle_tracks;
        if (!tracks.empty() && !status.subtitle_burned)
        {
            if (status.subtitle_index >= 0)
                player::show_text_subtitle(-1);
            else
            {
                int wanted = tracks.front().index;
                for (const auto &track : tracks)
                    if (track.is_default && track.is_text)
                        wanted = track.index;
                player::show_text_subtitle(wanted);
            }
        }
        /* Subtitles have nothing to do with the timeline, so they do not
           bring it up. Only pausing and up do that. */
        return;
    }

    /* Toggle playback diagnostics with the controller shortcut. */
    if (pad::pressed(pad::Button::touchpad) || pad::pressed(pad::Button::r3))
    {
        g_view.show_debug = !g_view.show_debug;
        return;
    }

    /* The controls retire by themselves, and while paused they stay up unless
       the viewer put them away on purpose. */
    if (status.paused && !g_view.osd_dismissed)
        g_view.osd_visible = true;
    else if (g_view.osd_visible && slopfin::app::now_us() - g_view.osd_idle > 4000000)
        g_view.osd_visible = false;
}

void handle_subtitle_settings_input() noexcept
{
    const auto &rows = subtitle_rows();
    const int last = static_cast<int>(rows.size()) - 1;
    const auto is_heading = [&rows](int i)
    { return rows[static_cast<std::size_t>(i)].label[0] == '\0'; };
    if (pad::pressed(pad::Button::down) && g_view.subtitle_row < last)
    {
        ++g_view.subtitle_row;
        if (is_heading(g_view.subtitle_row) && g_view.subtitle_row < last)
            ++g_view.subtitle_row;
        return;
    }
    if (pad::pressed(pad::Button::up) && g_view.subtitle_row > 1)
    {
        --g_view.subtitle_row;
        if (is_heading(g_view.subtitle_row) && g_view.subtitle_row > 1)
            --g_view.subtitle_row;
        return;
    }
    const SubtitleRow &row = rows[static_cast<std::size_t>(g_view.subtitle_row)];
    config::Settings &settings = config::current();
    const bool left = pad::pressed(pad::Button::left);
    const bool right = pad::pressed(pad::Button::right) || pad::pressed(pad::Button::cross);
    if (row.field != nullptr && (left || right))
    {
        int &value = settings.subtitle_look.*row.field;
        value = subtitle_style::wrap(value + (left ? -1 : 1), row.count);
        config::save(); /* a few hundred bytes on a press */
    }
    else if (row.field == nullptr && pad::pressed(pad::Button::cross))
    {
        settings.subtitle_look = subtitle_style::Choices{};
        config::save();
    }
}

/* Where the profile menu, the Profile screen and Settings > Account lead. */
void apply_account_choice(account::Choice choice, Screen from) noexcept
{
    switch (choice)
    {
    case account::Choice::profile:
        if (from != Screen::profile && from != Screen::dashboard)
            g_view.return_screen = from;
        account::open_profile();
        {
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            g_shared.screen = Screen::profile;
        }
        break;
    case account::Choice::dashboard:
        if (from != Screen::profile && from != Screen::dashboard)
            g_view.return_screen = from;
        dashboard::open();
        {
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            g_shared.screen = Screen::dashboard;
        }
        break;
    case account::Choice::sign_out:
        account::forget();
        set_status(Screen::connecting, "Signing out");
        submit(Request::sign_out);
        break;
    case account::Choice::none:
    case account::Choice::closed:
        break;
    }
}

void handle_settings_input() noexcept
{
    config::Settings &settings = config::current();
    if (g_view.settings_page != 0 && pad::pressed(pad::Button::circle))
    {
        g_view.settings_page = 0;
        return;
    }
    switch (g_view.settings_page)
    {
    case 1:
        handle_subtitle_settings_input();
        return;
    case 2:
    {
        autoplay::Choices &look = settings.autoplay_look;
        if (pad::pressed(pad::Button::down))
            g_view.playback_row = (g_view.playback_row + 1) % 3;
        else if (pad::pressed(pad::Button::up))
            g_view.playback_row = (g_view.playback_row + 2) % 3;
        else if (pad::pressed(pad::Button::left) || pad::pressed(pad::Button::right) ||
                 pad::pressed(pad::Button::cross))
        {
            const int step = pad::pressed(pad::Button::left) ? -1 : 1;
            if (g_view.playback_row == 0)
                look.enabled = !look.enabled;
            else if (g_view.playback_row == 1)
                look.ask_after = (std::clamp(look.ask_after, 0, 4) + step + 5) % 5;
            else
                look.ask_idle_minutes =
                    kIdleMinutes[(idle_index(look.ask_idle_minutes) + step + 6) % 6];
            config::save();
        }
        return;
    }
    case 3:
        if ((pad::pressed(pad::Button::cross) || pad::pressed(pad::Button::left) ||
             pad::pressed(pad::Button::right)) &&
            pad::trigger_effects_available())
        {
            settings.trigger_feedback = !settings.trigger_feedback;
            config::save();
            /* Felt immediately rather than at the next film. */
            pad::set_trigger_feel(settings.trigger_feedback ? pad::TriggerFeel::seek
                                                            : pad::TriggerFeel::none);
        }
        return;
    case 4:
        if (pad::pressed(pad::Button::cross))
            ask_keyboard(View::Prompt::host);
        return;
    case 6:
    {
        const bool can_send = diagnostics::destination(config::current().report_server).valid();
        const int count = can_send ? 2 : 1;
        g_view.diagnostics_row = std::clamp(g_view.diagnostics_row, 0, count - 1);
        if (pad::pressed(pad::Button::down) || pad::pressed(pad::Button::up))
        {
            g_view.diagnostics_row = (g_view.diagnostics_row + 1) % count;
            g_view.diagnostics_hold = 0;
        }
        else if (g_view.diagnostics_row == count - 1)
        {
            /* Throwing the reports away is held rather than pressed, like the
               dashboard's own one-way actions.  */
            if (!pad::held(pad::Button::cross))
                g_view.diagnostics_hold = 0;
            else if (++g_view.diagnostics_hold >= 60)
            {
                g_view.diagnostics_hold = 0;
                crash::report_forget();
                g_view.report_prompt = View::ReportPrompt::none;
            }
        }
        else if (pad::pressed(pad::Button::cross))
            send_crash_report();
        return;
    }
    case 5:
    {
        const std::vector<AccountRow> rows = account_rows();
        const int count = static_cast<int>(rows.size());
        g_view.account_row = std::clamp(g_view.account_row, 0, count - 1);
        if (pad::pressed(pad::Button::down) && g_view.account_row + 1 < count)
            ++g_view.account_row;
        else if (pad::pressed(pad::Button::up) && g_view.account_row > 0)
            --g_view.account_row;
        else if (pad::pressed(pad::Button::cross))
        {
            switch (rows[static_cast<std::size_t>(g_view.account_row)])
            {
            case AccountRow::profile:
                apply_account_choice(account::Choice::profile, Screen::settings);
                break;
            case AccountRow::dashboard:
                apply_account_choice(account::Choice::dashboard, Screen::settings);
                break;
            case AccountRow::sign_out:
                g_view.settings_page = 0;
                apply_account_choice(account::Choice::sign_out, Screen::settings);
                break;
            }
        }
        return;
    }
    default:
        break;
    }
    if (pad::pressed(pad::Button::down) && g_view.settings_index < kSettingsMenuCount - 1)
        ++g_view.settings_index;
    else if (pad::pressed(pad::Button::up) && g_view.settings_index > 0)
        --g_view.settings_index;
    else if (pad::pressed(pad::Button::left) || pad::pressed(pad::Button::circle))
        g_view.sidebar_focused = true;
    else if (pad::pressed(pad::Button::cross) || pad::pressed(pad::Button::right))
    {
        g_view.settings_page = g_view.settings_index + 1;
        g_view.subtitle_row = 1;
    }
}
} // namespace

namespace slopfin::app
{
namespace
{
bool g_exit_requested = false;
}

bool exit_requested() noexcept
{
    return g_exit_requested;
}

void start() noexcept
{
    config::load();
    (void)ime::initialize();
    (void)images::start();

    pthread_attr_t attributes;
    const bool have_attributes = pthread_attr_init(&attributes) == 0;
    if (have_attributes)
        (void)pthread_attr_setstacksize(&attributes, 4u * 1024u * 1024u);
    void *thread = nullptr;
    const int created = scePthreadCreate(&thread, have_attributes ? &attributes : nullptr, worker,
                                         nullptr, "slopfin-data");
    if (have_attributes)
        (void)pthread_attr_destroy(&attributes);
    /* It parses every response the server sends. Created from the render
       thread, it would otherwise inherit the render core. */
    if (const std::uint64_t spare = images::spare_cores(); created == 0 && spare != 0)
        (void)scePthreadSetaffinity(thread, spare);
    if (created != 0)
        set_status(Screen::connecting, "Could not start the network thread");
    /* The session file's own thread, kept off the render core. */
    crash::watch(images::spare_cores());
    /* A fatal crash report is offered once. A leftover heartbeat from a run
       that simply disappeared stays in Diagnostics but does not interrupt
       startup, because PS5 Close Game can leave the same evidence. */
    if (crash::report_waiting() &&
        diagnostics::destination(config::current().report_server).valid())
    {
        crash::report_offered();
        g_view.report_prompt = View::ReportPrompt::offer;
        g_view.report_choice = 0;
    }
    std::size_t free_bytes = 0;
    if (sceKernelAvailableFlexibleMemorySize(&free_bytes) >= 0)
        trace::mark("flexible memory free: " + std::to_string(free_bytes / (1024u * 1024u)) +
                    " MiB");

    /* How much the process heap will actually hand out, in 2 MiB blocks. */
    {
        constexpr int kProbeBlocks = 64;
        void *blocks[kProbeBlocks] = {};
        int taken = 0;
        for (; taken < kProbeBlocks; ++taken)
        {
            blocks[taken] = std::malloc(2u * 1024u * 1024u);
            if (blocks[taken] == nullptr)
                break;
            /* Touch it: a lazy allocator can succeed and fail on first write. */
            std::memset(blocks[taken], 0, 4096);
        }
        for (int i = 0; i < taken; ++i)
            std::free(blocks[i]);
        trace::mark("heap probe: " + std::to_string(taken * 2) + " MiB in 2 MiB blocks");
    }
    trace::mark(created == 0 ? "app: worker started" : "app: worker FAILED");
}

void open_item(const std::string &item_id) noexcept
{
    open_detail(item_id);
}

#ifdef SLOPFIN_HOST
void preview_search(const std::string &term) noexcept
{
    g_view.search.open = true;
    g_view.search.draft = term;
    g_view.search.reveal.jump(1.0f);
    g_view.sidebar_focused = false;
    Screen screen;
    std::vector<SidebarEntry> sidebar;
    std::vector<jellyfin::Library> libraries;
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        screen = g_shared.screen;
        libraries = g_shared.libraries;
    }
    sidebar = build_sidebar(libraries);
    run_live_search(search_scope(screen, sidebar, libraries));
    restart_focus();
}
#endif

void frame() noexcept
{
    const std::uint64_t loop_start = now_us();
    if (g_last_loop_us != 0)
        g_loop_us = loop_start - g_last_loop_us;
    g_last_loop_us = loop_start;

    ++g_view.frames;
    if (config::current().signed_in() && g_view.credential_form)
    {
        std::fill(g_view.pending_password.begin(), g_view.pending_password.end(), '\0');
        g_view.pending_password.clear();
        g_view.pending_username.clear();
        g_view.credential_form = false;
        g_view.show_password = false;
        g_view.setup_index = 0;
    }
    crash::frame(g_view.frames);
    advance_motion();
    if ((g_view.frames % 240) == 0)
        trim_artwork_fades();

    if (g_view.ime_input_guard > 0)
        --g_view.ime_input_guard;
    ime::poll();
    handle_keyboard_result();

    Screen screen;
    std::string status;
    std::string detail;
    std::string code;
    std::string server_name;
    std::string grid_title;
    bool grid_loading;
    int grid_total;
    bool busy;
    /* Large model snapshots live across frames. The worker bumps an epoch only
       when one changes, so a steady 60 Hz screen no longer deep-copies up to
       120 Items (and all of their strings/vectors) every render iteration. */
    static std::vector<Row> rows;
    static std::vector<jellyfin::Item> grid_items;
    static std::vector<jellyfin::Library> libraries;
    static jellyfin::Item detail_item;
    static std::vector<jellyfin::Item> detail_episodes;
    static jellyfin::Item detail_next_up;
    static std::vector<jellyfin::Item> search_items;
    static std::vector<search_sections::Section> search_groups;
    static std::uint64_t seen_rows = ~std::uint64_t{0};
    static std::uint64_t seen_grid = ~std::uint64_t{0};
    static std::uint64_t seen_libraries = ~std::uint64_t{0};
    static std::uint64_t seen_detail = ~std::uint64_t{0};
    static std::uint64_t seen_search = ~std::uint64_t{0};
    bool detail_loading;
    bool search_loading;
    std::string search_error;
    long home_epoch;
    bool grid_snapshot_changed = false;

    /* Resnapshot shared state after navigation so models and view state belong to the same screen.
     */
    const auto snapshot_shared = [&]
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        home_epoch = g_shared.home_epoch;
        screen = g_shared.screen;
        status = g_shared.status;
        detail = g_shared.detail;
        code = g_shared.session.code;
        server_name = g_shared.server_name;
        if (seen_rows != g_shared.rows_epoch)
        {
            rows = g_shared.rows;
            seen_rows = g_shared.rows_epoch;
        }
        if (seen_grid != g_shared.grid_epoch)
        {
            grid_items = g_shared.grid_items;
            seen_grid = g_shared.grid_epoch;
            grid_snapshot_changed = true;
        }
        grid_title = g_shared.grid_title;
        grid_loading = g_shared.grid_loading;
        grid_total = g_shared.grid_total;
        if (seen_libraries != g_shared.libraries_epoch)
        {
            libraries = g_shared.libraries;
            seen_libraries = g_shared.libraries_epoch;
        }
        if (seen_detail != g_shared.detail_epoch)
        {
            detail_item = g_shared.detail_item;
            detail_episodes = g_shared.detail_episodes;
            detail_next_up = g_shared.detail_next_up;
            seen_detail = g_shared.detail_epoch;
        }
        detail_loading = g_shared.detail_loading;
        if (seen_search != g_shared.search_epoch)
        {
            search_items = g_shared.search_items;
            search_groups = search_sections::build(search_items);
            seen_search = g_shared.search_epoch;
        }
        search_loading = g_shared.search_loading;
        search_error = g_shared.search_error;
        busy = g_shared.busy;
        if (g_shared.report_ready)
        {
            g_view.report_line = std::move(g_shared.report_message);
            g_view.report_prompt =
                g_shared.report_sent ? View::ReportPrompt::sent : View::ReportPrompt::failed;
            g_view.report_leaves = 180;
            g_shared.report_ready = false;
        }
    };
    snapshot_shared();

    const auto retire_outgoing_grid_if_ready = [&]
    {
        /* Outgoing cards cover only the network gap. Once the correct new list
           exists, do not paint two categories through one another. */
        if (grid_snapshot_changed && !grid_items.empty())
        {
            g_view.leaving_items.clear();
            g_view.leaving_frame = -1;
        }
        grid_snapshot_changed = false;
    };
    retire_outgoing_grid_if_ready();

    /* Refresh sibling availability while its worker request is pending. */
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        const int at = g_shared.sibling_index;
        const int count = static_cast<int>(g_shared.siblings.size());
        g_view.has_previous = at > 0;
        g_view.has_next = at >= 0 && at + 1 < count;
        g_view.previous_id = g_view.has_previous
                                 ? g_shared.siblings[static_cast<std::size_t>(at - 1)].id
                                 : std::string{};
        g_view.next_id = g_view.has_next ? g_shared.siblings[static_cast<std::size_t>(at + 1)].id
                                         : std::string{};
        g_view.changing_episode = g_shared.sibling_loading;
    }

    /* Start prepared sibling playback on the render thread. */
    {
        jellyfin::Item next;
        bool ready = false;
        {
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            if (g_shared.pending_play_ready)
            {
                next = std::move(g_shared.pending_play);
                g_shared.pending_play = jellyfin::Item{};
                g_shared.pending_play_ready = false;
                ready = true;
            }
        }
        if (ready && !next.id.empty())
        {
            /* The detail screen always stops before it starts; a second start
               on a live stream is refused and leaves the old one running. */
            player::stop();
            subtitles::clear();
            bool resume = false;
            {
                std::lock_guard<std::mutex> guard(g_shared.mutex);
                resume = g_shared.pending_play_resume;
                g_shared.pending_play_resume = false;
            }
            begin_playback(next, 0, resume ? next.resume_ticks / 10000000.0 : 0.0);
            screen = Screen::playing;
        }
    }

    static std::vector<SidebarEntry> sidebar_cache;
    static std::uint64_t sidebar_epoch = ~std::uint64_t{0};
    if (sidebar_epoch != seen_libraries)
    {
        sidebar_cache = build_sidebar(libraries);
        sidebar_epoch = seen_libraries;
    }
    const std::vector<SidebarEntry> &sidebar = sidebar_cache;
    g_view.sidebar_index =
        std::clamp(g_view.sidebar_index, 0, std::max(0, static_cast<int>(sidebar.size()) - 1));
    const SearchScope scope = search_scope(screen, sidebar, libraries);
    if (g_view.search.open && (g_view.search.committed_scope != scope.library_id ||
                               g_view.search.committed_types != scope.types))
        g_view.search.pending = 1;

    /* Searching is only offered where there is something to search. */
    if (g_view.search.open && screen != Screen::home && screen != Screen::grid)
        close_search();
    g_view.search.result_index = std::clamp(g_view.search.result_index, 0,
                                            std::max(0, static_cast<int>(search_items.size()) - 1));

    /* Trigger plus vertical direction switches categories. */
    const bool section_shortcut =
        !g_view.search.open && !ime::busy() &&
        (pad::held(pad::Button::l2) || pad::held(pad::Button::r2)) &&
        (screen == Screen::home || screen == Screen::grid || screen == Screen::settings);
    bool shortcut_used = false;
    if (section_shortcut && !sidebar.empty())
    {
        const int count = static_cast<int>(sidebar.size());
        int wanted = g_view.sidebar_index;
        if (pad::pressed(pad::Button::down) && wanted + 1 < count)
            ++wanted;
        else if (pad::pressed(pad::Button::up) && wanted > 0)
            --wanted;
        if (wanted != g_view.sidebar_index)
        {
            g_view.sidebar_index = wanted;
            open_section(sidebar, wanted, g_view.sidebar_focused);
            shortcut_used = true;
        }
    }

    /* Browsing screens only: Settings reaches the same places through Account. */
    const bool has_badge =
        (screen == Screen::home || screen == Screen::grid) && config::current().signed_in();
    if (has_badge)
        account::ensure_user();

    /* The keyboard owns input while it is up. */
    if (!ime::busy() && g_view.ime_input_guard == 0 && !shortcut_used)
    {
        if (report_prompt_open() && screen != Screen::playing)
        {
            /* The offer to send a crash report holds the input until it is
               answered; it is only ever up outside playback. */
            handle_report_prompt();
        }
        else if (account::menu_open())
        {
            /* The profile menu holds the input until it closes. */
            apply_account_choice(account::handle_menu_input(), screen);
        }
        else if (has_badge && !g_view.search.open && pad::pressed(pad::Button::square))
        {
            account::open_menu();
        }
        else if (handle_search_input(scope, search_items, search_groups))
        {
            /* the search has the frame */
        }
        else if (g_view.sidebar_focused &&
                 (screen == Screen::home || screen == Screen::grid || screen == Screen::settings))
        {
            handle_sidebar_input(sidebar);
        }
        else
        {
            switch (screen)
            {
            case Screen::home:
                clamp_home(rows);
                handle_home_input(rows);
                break;
            case Screen::grid:
                if (!grid_items.empty())
                {
                    std::lock_guard<std::mutex> guard(g_shared.mutex);
                    if (g_shared.grid_restore >= 0)
                    {
                        g_view.grid_index = std::clamp(g_shared.grid_restore, 0,
                                                       static_cast<int>(grid_items.size()) - 1);
                        g_shared.grid_restore = -1;
                    }
                }
                g_view.grid_index = std::clamp(
                    g_view.grid_index, 0, std::max(0, static_cast<int>(grid_items.size()) - 1));
                handle_grid_input(grid_items);
                break;
            case Screen::detail:
                handle_detail_input(detail_item, detail_episodes, detail_next_up);
                break;
            case Screen::playing:
                handle_player_input();
                break;
            case Screen::settings:
                handle_settings_input();
                break;
            case Screen::profile:
            {
                const account::Choice choice = account::handle_profile_input();
                if (choice == account::Choice::closed)
                {
                    std::lock_guard<std::mutex> guard(g_shared.mutex);
                    g_shared.screen = g_view.return_screen;
                }
                else
                    apply_account_choice(choice, Screen::profile);
                break;
            }
            case Screen::dashboard:
                if (!dashboard::handle_input())
                {
                    std::lock_guard<std::mutex> guard(g_shared.mutex);
                    g_shared.screen = g_view.return_screen;
                }
                break;
            case Screen::server_setup:
            case Screen::sign_in:
                handle_setup_input(screen);
                break;
            case Screen::connecting:
                break;
            }
        }
    }

    /* Apply navigation and refresh model snapshots before drawing. */
    snapshot_shared();
    retire_outgoing_grid_if_ready();

    if (screen == Screen::home)
    {
        clamp_home(rows);
        update_home_scroll(rows);
    }

    /* Output mode changes are a frame-boundary operation, not a side effect of
       clearing a UI screen. The playback worker waits for this acknowledgement
       before it configures HDMI audio and starts decoding. */
    gfx::apply_pending_output_mode();

    // Playback draws its own full frame/bars. A menu gradient here both
    // wastes HDR work and invalidates an unchanged video staging buffer.
    if (screen != Screen::playing)
        draw_background();

    /* Search replaces category content while preserving the underlying browsing state. */
    if (g_view.search.open)
    {
        /* The keys first, the bar over them: the bar is the thing being
           typed into and nothing should ever be laid across it. */
        draw_search_results(search_items, search_groups, search_loading, search_error);
        draw_search_bar(scope, search_loading, search_items.size());
    }
    else
    {
        /* Determine scroll clipping before drawing the arriving model. */
        /* Reveal category cards when their result generation changes. */
        if (screen == Screen::grid && !grid_items.empty())
        {
            const std::string arrived = "grid|" + grid_title + "|" +
                                        std::to_string(grid_items.size()) + "|" +
                                        grid_items.front().id;
            if (arrived != g_reveal_of)
            {
                g_reveal_of = arrived;
                g_reveal_frame = g_view.frames;
            }
        }
        else if (screen == Screen::home && !rows.empty() && !rows.front().items.empty())
        {
            /* The load this came from, not what it holds: a Continue Watching
               refresh reorders the first row and must not re-deal the screen. */
            const std::string arrived = "home|" + std::to_string(home_epoch);
            if (arrived != g_reveal_of)
            {
                g_reveal_of = arrived;
                g_reveal_frame = g_view.frames;
            }
        }
        /* Ease newly loaded page content into view. */
        {
            /* Compared, never built: this runs every frame, and putting a key
               together here would be an allocation on the render thread sixty
               times a second for something that changes once a page.
                */
            const std::string_view id = screen == Screen::detail ? std::string_view{detail_item.id}
                                        : screen == Screen::grid ? std::string_view{grid_title}
                                                                 : std::string_view{};
            /* Skip page reveals until there is visible content. */
            const bool ready = screen == Screen::detail ? !detail_item.id.empty()
                               : screen == Screen::grid ? !grid_items.empty()
                               : screen == Screen::home ? !rows.empty()
                                                        : true;
            const bool changed = screen != g_page_screen || id != g_page_id || !g_page_seen;
            if (changed)
            {
                const bool first = !g_page_seen;
                g_page_seen = true;
                g_page_screen = screen;
                g_page_id.assign(id);
                /* Arriving at Home is the moment to ask again: coming back from a
                   title, the rows behind it may already be out of date. */
                if (screen == Screen::home && !first)
                {
                    g_latest_due.store(true);
                    std::lock_guard<std::mutex> guard(g_shared.mutex);
                    g_shared.progress_stale = true;
                    if (g_progress_ready_frame < 0)
                        g_progress_ready_frame = static_cast<long>(g_view.frames);
                }
                g_page_changed = true;
                if (first || !ready || screen == Screen::playing)
                    g_view.page_in.jump(1.0f);
                else
                {
                    g_view.page_in.jump(0.0f);
                    g_view.page_in.aim(1.0f);
                }
            }
        }
        /* Prefetch details after the cursor settles. */
        if (screen == Screen::home || screen == Screen::grid)
        {
            std::string resting;
            if (screen == Screen::grid && !grid_items.empty())
                resting =
                    grid_items[static_cast<std::size_t>(std::clamp(
                                   g_view.grid_index, 0, static_cast<int>(grid_items.size()) - 1))]
                        .id;
            else if (screen == Screen::home && !rows.empty())
            {
                const int r = std::clamp(g_view.row_index, 0, static_cast<int>(rows.size()) - 1);
                const auto &items = rows[static_cast<std::size_t>(r)].items;
                if (!items.empty() && static_cast<int>(g_view.column.size()) > r)
                    resting = items[static_cast<std::size_t>(
                                        std::clamp(g_view.column[static_cast<std::size_t>(r)], 0,
                                                   static_cast<int>(items.size()) - 1))]
                                  .id;
            }
            if (resting != g_resting_id)
            {
                g_resting_id = resting;
                g_resting_since = g_view.frames;
            }
            else if (!resting.empty())
            {
                const int age = g_view.frames - g_resting_since;
                if (age == 15)
                    prefetch_detail(resting);
                /* Metadata normally lands well before this. Retry once at 0.8 s
                   for a slow LAN response without turning a held cursor into a
                   polling loop. */
                else if (age == 36 || age == 48)
                    prefetch_detail_rows(resting);
            }
        }

        /* The report says what was on screen when it stopped: built when the page
           changes, not every frame, for the same reason as the key above. */
        {
            if (g_page_changed)
            {
                std::string doing = screen_name(screen);
                if (screen == Screen::detail && !detail_item.id.empty())
                    doing += " " + detail_item.name;
                else if (screen == Screen::grid && !grid_title.empty())
                    doing += " " + grid_title;
                else if (screen == Screen::playing && !g_view.playing_series.empty())
                    doing += " " + g_view.playing_series;
                crash::phase(doing);
            }
            /* Refreshed about once a second: if the console kills the app, this is
               the last thing anyone will know about it. */
            if ((g_view.frames % 60) == 0)
            {
                std::size_t free_bytes = 0;
                (void)sceKernelAvailableFlexibleMemorySize(&free_bytes);
                /* Both pools: a console that kills an application for memory
                   usually does it over direct memory, which flexible free space
                   says nothing about.  */
                std::string state = "free " + std::to_string(free_bytes / (1024 * 1024)) +
                                    " MiB, images " +
                                    std::to_string(images::cached_bytes() / (1024 * 1024)) + " MiB";
                if (screen == Screen::playing)
                {
                    const player::Status playing = player::status();
                    state = clock_text(playing.position_seconds) + " of " +
                            clock_text(playing.duration_seconds) + ", " + state;
                    if (playing.paused)
                        state += ", paused";
                }
                crash::detail(state);
            }
            g_page_changed = false;
        }
        const float page_in = g_view.page_in.value();
        const bool page_moving = page_in < 0.995f && screen != Screen::playing;
        if (page_moving)
            gfx::set_origin(0, static_cast<int>((1.0f - page_in) * 30.0f));

        switch (screen)
        {
        case Screen::connecting:
            draw_message(status, detail);
            break;
        case Screen::server_setup:
            draw_server_setup(detail);
            break;
        case Screen::sign_in:
            draw_sign_in(code, status, detail, server_name);
            break;
        case Screen::home:
            if (rows.empty())
                draw_message(status, detail);
            else
                draw_home(rows);
            break;
        case Screen::grid:
            draw_grid(grid_items, grid_title, grid_loading, grid_total);
            break;
        case Screen::detail:
            draw_detail(detail_item, detail_episodes, detail_next_up, detail_loading);
            if (g_view.details_open)
                draw_details_sheet(detail_item);
            break;
        case Screen::playing:
            draw_player();
            break;
        case Screen::settings:
            draw_settings(server_name);
            break;
        case Screen::profile:
            account::draw_profile(server_name);
            break;
        case Screen::dashboard:
            dashboard::draw();
            break;
        }
        if (page_moving)
        {
            gfx::set_origin(0, 0);
            /* The sidebar stays put: it is the one thing that does not change
               between these pages, so moving it would say the wrong thing. */
            const bool with_sidebar = screen != Screen::playing && screen != Screen::detail &&
                                      screen != Screen::profile && screen != Screen::dashboard;
            const int from = with_sidebar ? kSidebarWidth : 0;
            gfx::fill_rect(from, 0, gfx::kWidth - from, gfx::kHeight,
                           gfx::with_alpha(gfx::palette::background,
                                           static_cast<std::uint8_t>((1.0f - page_in) * 255.0f)));
        }
    }
    /* Dim content while sidebar navigation owns input. */
    if (g_view.sidebar_dim.value() > 0.004f &&
        (screen == Screen::home || screen == Screen::grid || screen == Screen::settings))
    {
        const float t = g_view.sidebar_dim.value();
        gfx::horizontal_gradient(kSidebarWidth, 0, gfx::kWidth - kSidebarWidth, gfx::kHeight,
                                 gfx::rgba(0x00, 0x00, 0x00, static_cast<std::uint8_t>(t * 0xa8)),
                                 gfx::rgba(0x00, 0x00, 0x00, static_cast<std::uint8_t>(t * 0x40)));
    }

    if (screen != Screen::playing && screen != Screen::detail && screen != Screen::profile &&
        screen != Screen::dashboard)
        draw_sidebar(screen, sidebar, server_name);

    /* The profile badge holds the top right of every browsing screen, and its
       menu goes over everything, the sidebar included. */
    /* The menu's hush goes over the page, and the badge it came from stays lit above it. */
    if (account::menu_visible())
        account::draw_menu(kContentRight, kTopBarY + ui::kTopChipHeight);
    if (has_badge && !g_view.search.open)
        account::draw_badge(kContentRight, kTopBarY);
    /* Never over a film: the offer waits until browsing. */
    if (screen != Screen::playing)
        draw_report_prompt();

    g_draw_us = now_us() - loop_start;

    /* Record frame timings on all screens. */
    {
        if (telemetry::enabled())
        {
            const player::Status timing = player::status();
            telemetry::Sample sample{};
            sample.at_us = loop_start;
            sample.loop_us = static_cast<std::uint32_t>(g_loop_us);
            sample.draw_us = static_cast<std::uint32_t>(g_draw_us);
            sample.present_us = static_cast<std::uint32_t>(g_present_us);
            sample.decode_us = static_cast<std::uint32_t>(timing.decode_us);
            sample.convert_us = static_cast<std::uint32_t>(timing.convert_us);
            sample.pts_ms = static_cast<std::int32_t>(timing.position_seconds * 1000.0);
            sample.advanced = timing.shown != g_last_shown ? 1 : 0;
            sample.queued = static_cast<std::uint8_t>(timing.queued);
            sample.network_us = static_cast<std::uint32_t>(timing.network_us);
            sample.slotwait_us = static_cast<std::uint32_t>(timing.slotwait_us);
            sample.pad_us = static_cast<std::uint32_t>(g_pad_us);
            sample.composed = static_cast<std::uint8_t>(g_wash_composed & 0xffu);
            sample.decoded = static_cast<std::uint32_t>(timing.decoded);
            sample.converted = static_cast<std::uint32_t>(timing.frames);
            sample.shown = static_cast<std::uint32_t>(timing.shown);
            sample.dropped = static_cast<std::uint32_t>(timing.dropped);
            sample.decode_failures = static_cast<std::uint32_t>(timing.decode_failures);
            sample.input_stage = static_cast<std::uint32_t>(timing.input_stage);
            sample.input_wait_us =
                timing.input_since_us ? static_cast<std::uint32_t>(now_us() - timing.input_since_us)
                                      : 0;
            const auto audio_stats = audio::output_stats();
            sample.audio_buffered = audio_stats.buffered_frames;
            sample.audio_played = static_cast<std::uint32_t>(audio_stats.played_frames);
            sample.audio_underrun = static_cast<std::uint32_t>(audio_stats.underrun_frames);
            sample.audio_errors = audio_stats.errors;
            sample.audio_buffering = audio_stats.buffering_frames;
            sample.audio_rebuffers = audio_stats.rebuffer_events;
            sample.software_frames = audio_stats.software_frames;
            sample.software_work_us = audio_stats.software_work_us;
            sample.software_queue_us = audio_stats.software_queue_us;
            sample.software_bytes = audio_stats.software_bytes;
            g_last_shown = timing.shown;
            telemetry::record(sample);
        }
        /* Write trace and screenshot files only on explicit marker requests; writes stall
         * rendering. */
        if ((g_view.frames % 30) == 11)
        {
            if (std::FILE *listen = std::fopen("/data/slopfin-listen", "rb"); listen != nullptr)
            {
                (void)std::fclose(listen);
                (void)unlink("/data/slopfin-listen");
                audio::begin_capture();
            }
            else if (std::FILE *heard = std::fopen("/data/slopfin-listen-dump", "rb");
                     heard != nullptr)
            {
                (void)std::fclose(heard);
                (void)unlink("/data/slopfin-listen-dump");
                audio::write_capture();
            }
            else if (std::FILE *request = std::fopen("/data/slopfin-trace-on", "rb");
                     request != nullptr)
            {
                (void)std::fclose(request);
                (void)unlink("/data/slopfin-trace-on");
                telemetry::enable(true);
            }
            else if (std::FILE *dump = std::fopen("/data/slopfin-trace-dump", "rb");
                     dump != nullptr)
            {
                (void)std::fclose(dump);
                (void)unlink("/data/slopfin-trace-dump");
                telemetry::flush();
                telemetry::enable(false);
            }
        }
    }

    /* Update artwork cache policy when entering or leaving playback. */

    /* Mark Home progress rows stale after playback ends. */
    const bool playing_now = screen == Screen::playing;
    if (g_was_playing && !playing_now)
    {
        g_progress_ready_frame = static_cast<long>(g_view.frames) + kProgressSettleFrames;
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        g_shared.progress_stale = true;
    }
    g_was_playing = playing_now;

    /* Refresh stale Home progress immediately and then periodically. */
    if (screen == Screen::home)
    {
        const long now = static_cast<long>(g_view.frames);
        bool queued = false;
        {
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            const bool settled = g_shared.progress_stale && now >= g_progress_ready_frame;
            const bool due =
                g_progress_asked_frame < 0 || now - g_progress_asked_frame >= kProgressPollFrames;
            if ((settled || due) && !g_shared.busy && g_shared.request == Request::none)
            {
                g_shared.request = Request::refresh_progress;
                g_shared.progress_stale = false;
                g_progress_asked_frame = now;
                queued = true;
            }
        }
        if (queued)
            g_request_wake.notify_one();
    }

    images::set_playback(screen == Screen::playing);
    if ((g_view.frames % 300) == 0 && g_first_ready + g_first_missing != g_first_reported)
    {
        g_first_reported = g_first_ready + g_first_missing;
        trace::mark("art: " + std::to_string(g_first_reported) + " cards first drawn, " +
                    std::to_string(g_first_ready) + " already loaded, " +
                    std::to_string(g_first_missing) + " not");
    }
    if ((g_view.frames % (screen == Screen::playing ? 30 : 120)) == 0)
        images::trim();

    /* Write screenshots only on explicit requests to avoid periodic frame stalls. */
    if ((g_view.frames % 15) == 7)
    {
        /* Liveness probe: deleting the marker proves the loop is still turning. */
        if (std::FILE *alive = std::fopen("/data/slopfin-alive", "rb"); alive != nullptr)
        {
            (void)std::fclose(alive);
            (void)unlink("/data/slopfin-alive");
        }
        if (std::FILE *request = std::fopen("/data/slopfin-shot", "rb"); request != nullptr)
        {
            /* An optional crop marker uses physical framebuffer coordinates. */
            (void)std::fclose(request);
            /* Use POSIX reads; this runtime does not reliably implement fread for marker files. */
            char text[64] = {};
            std::size_t seen = 0;
            if (const int fd = open("/data/slopfin-shot", kReadOnly); fd >= 0)
            {
                const long got = read(fd, text, sizeof(text) - 1);
                seen = got > 0 ? static_cast<std::size_t>(got) : 0;
                (void)close(fd);
            }
            (void)unlink("/data/slopfin-shot");
            int box[4] = {0, 0, 0, 0};
            int found = 0;
            for (const char *at = text; found < 4 && at < text + seen;)
            {
                while (at < text + seen && (*at == ' ' || *at == '\n' || *at == '\r'))
                    ++at;
                char *end = nullptr;
                const long value = std::strtol(at, &end, 10);
                if (end == at)
                    break;
                box[found++] = static_cast<int>(value);
                at = end;
            }
            const bool cropped = found == 4 && box[2] > 0 && box[3] > 0;
            (void)gfx::request_capture(box[0], box[1], box[2], box[3], cropped ? 1 : 2);
            gfx::report_output_mode();
        }
        if (std::FILE *display = std::fopen("/data/slopfin-display", "rb"); display != nullptr)
        {
            (void)std::fclose(display);
            (void)unlink("/data/slopfin-display");
            gfx::report_display_info();
        }
        if (std::FILE *bench = std::fopen("/data/slopfin-bench", "rb"); bench != nullptr)
        {
            (void)std::fclose(bench);
            (void)unlink("/data/slopfin-bench");
            gfx::benchmark("/data/slopfin-bench.txt");
        }

        /*
         * The capability probe opens and closes every audio output format, so
         * it must never run while the player owns the sink.
         */
        if (screen != Screen::playing)
        {
            /* HDMI bitstream experiment; the marker's text is
               "mode device user port seconds words file". */
            /* prove the crash report on the console
               itself. The marker names the fault: 1 bad memory access, 2 an
               abandoned program, anything else an abort. */
            if (int marker = open("/data/slopfin-crash-test", O_RDONLY); marker >= 0)
            {
                char kind[8] = {};
                (void)read(marker, kind, sizeof(kind) - 1);
                (void)close(marker);
                (void)unlink("/data/slopfin-crash-test");
                crash::self_test(std::atoi(kind));
            }
            if (int marker = open("/data/slopfin-bitstream-test", O_RDONLY); marker >= 0)
            {
                char arguments[64] = {};
                (void)read(marker, arguments, sizeof(arguments) - 1);
                (void)close(marker);
                (void)unlink("/data/slopfin-bitstream-test");
                bitstream_probe::start(arguments);
            }
            if (std::FILE *caps = std::fopen("/data/slopfin-audio-caps", "rb"); caps != nullptr)
            {
                (void)std::fclose(caps);
                (void)unlink("/data/slopfin-audio-caps");
                audiocaps::probe();
            }
            else if (std::FILE *vcaps = std::fopen("/data/slopfin-video-caps", "rb");
                     vcaps != nullptr)
            {
                (void)std::fclose(vcaps);
                (void)unlink("/data/slopfin-video-caps");
                player::probe_decoder_support();
            }
        }
    }

    /*
     * Development aid: a file of button names is replayed as input, one press
     * per frame, so navigation can be exercised from a remote script.
     */
    if ((g_view.frames % 20) == 10)
    {
        /* Development aid: bring up "Are you still watching?" now, rather than
           sitting through three episodes to see it. Reachable during playback,
           which is the only time it means anything. */
        if (int marker = open("/data/slopfin-ask-test", O_RDONLY); marker >= 0)
        {
            (void)close(marker);
            (void)unlink("/data/slopfin-ask-test");
            g_view.autoplay.asked();
        }
        if (std::FILE *script = std::fopen("/data/slopfin-input", "rb"); script != nullptr)
        {
            char word[96] = {};
            while (std::fscanf(script, "%95s", word) == 1)
            {
                const std::string name{word};
                if (name.rfind("open:", 0) == 0)
                {
                    open_detail(name.substr(5));
                    continue;
                }
                if (name.rfind("play:", 0) == 0)
                {

                    /* play:<itemId>[:<startSeconds>[:<subtitleIndex>[:<audioIndex>[:<maxBitrate>]]]]
                     */
                    std::vector<std::string> fields;
                    for (std::size_t from = 5;;)
                    {
                        const std::size_t colon = name.find(':', from);
                        fields.push_back(name.substr(
                            from, colon == std::string::npos ? std::string::npos : colon - from));
                        if (colon == std::string::npos)
                            break;
                        from = colon + 1;
                    }
                    jellyfin::PlaybackRequest request;
                    request.item_id = fields[0];
                    if (fields.size() > 1)
                        request.start_seconds = std::atof(fields[1].c_str());
                    if (fields.size() > 2)
                        request.subtitle_index = std::atoi(fields[2].c_str());
                    if (fields.size() > 3)
                        request.audio_index = std::atoi(fields[3].c_str());
                    if (fields.size() > 4)
                        request.max_bitrate = std::max(0LL, std::atoll(fields[4].c_str()));
                    /* Development aid: fetch the item so the track panel, the
                       title block and the scrubber have what they would have
                       had via the detail screen. */
                    std::string title = "Playback test";
                    double duration = 0.0;
                    jellyfin::Item played;
                    if (jellyfin::details(request.item_id, played))
                    {
                        g_view.playing_item = played;
                        g_view.end_choice = 0;
                        g_view.finished = false;
                        g_view.playing_source =
                            played.sources.empty() ? jellyfin::MediaSource{} : played.sources[0];
                        set_playing_title(played);
                        title = played.name;
                        duration = played.runtime_minutes * 60.0;
                    }
                    /* The detail screen always stops before it starts, so the
                       test path does too; otherwise a second play is refused
                       and the old stream keeps running under the new title. */
                    player::stop();
                    /* And it sets up what the real path sets up, or a test
                       play has no neighbouring episodes and nothing that
                       depends on them -- autoplay included -- can be tried
                       from the tooling at all.  */
                    g_view.autoplay.chosen_by_hand();
                    g_view.up_next = jellyfin::Item{};
                    g_view.up_next_in.jump(0.0f);
                    g_view.intro_visible = false;
                    g_view.intro_dismissed = false;
                    g_view.intro_seek_failed = false;
                    g_view.intro_in.jump(0.0f);
                    g_view.has_previous = g_view.has_next = false;
                    g_view.previous_id.clear();
                    g_view.next_id.clear();
                    {
                        std::lock_guard<std::mutex> guard(g_shared.mutex);
                        g_shared.siblings.clear();
                        g_shared.sibling_index = -1;
                    }
                    if (played.type == "Episode" && !played.series_id.empty())
                        submit_siblings(played.series_id, played.id);
                    g_view.osd_visible = true;
                    g_view.osd_dismissed = false;
                    g_view.osd_idle = slopfin::app::now_us();
                    g_view.osd_focus = View::OsdFocus::bar;
                    g_view.osd_button = 0;
                    g_view.panel_open = false;
                    g_view.seek_target = -1.0;
                    images::set_playback(!request.item_id.empty());
                    if (!request.item_id.empty() && player::start(request, title, duration))
                    {
                        std::lock_guard<std::mutex> guard(g_shared.mutex);
                        g_shared.screen = Screen::playing;
                    }
                    continue;
                }
                /* Synthetic hold input persists for the specified frame count. */
                if (name.rfind("hold:", 0) == 0)
                {
                    const std::size_t colon = name.find(':', 5);
                    const std::string which =
                        name.substr(5, colon == std::string::npos ? std::string::npos : colon - 5);
                    const int frames =
                        colon == std::string::npos ? 60 : std::atoi(name.c_str() + colon + 1);
                    static const std::pair<const char *, pad::Button> kNames[] = {
                        {"up", pad::Button::up},
                        {"down", pad::Button::down},
                        {"left", pad::Button::left},
                        {"right", pad::Button::right},
                        {"cross", pad::Button::cross},
                        {"circle", pad::Button::circle},
                        {"triangle", pad::Button::triangle},
                        {"square", pad::Button::square},
                        {"l1", pad::Button::l1},
                        {"r1", pad::Button::r1},
                        {"l2", pad::Button::l2},
                        {"r2", pad::Button::r2},
                        {"options", pad::Button::options},
                        {"touchpad", pad::Button::touchpad},
                    };
                    for (const auto &entry : kNames)
                        if (which == entry.first)
                            pad::inject_hold(entry.second, frames);
                    continue;
                }
                if (name == "up")
                    pad::inject(pad::Button::up);
                else if (name == "down")
                    pad::inject(pad::Button::down);
                else if (name == "left")
                    pad::inject(pad::Button::left);
                else if (name == "right")
                    pad::inject(pad::Button::right);
                else if (name == "cross")
                    pad::inject(pad::Button::cross);
                else if (name == "circle")
                    pad::inject(pad::Button::circle);
                else if (name == "triangle")
                    pad::inject(pad::Button::triangle);
                else if (name == "square")
                    pad::inject(pad::Button::square);
                else if (name == "l1")
                    pad::inject(pad::Button::l1);
                else if (name == "r1")
                    pad::inject(pad::Button::r1);
                else if (name == "l2")
                    pad::inject(pad::Button::l2);
                else if (name == "r2")
                    pad::inject(pad::Button::r2);
                else if (name == "options")
                    pad::inject(pad::Button::options);
                else if (name == "touchpad")
                    pad::inject(pad::Button::touchpad);
            }
            (void)std::fclose(script);
            (void)unlink("/data/slopfin-input");
        }
    }

    /*
     * Development aid: dropping a marker file asks the app to close itself, so
     * a new build can be launched without touching the console.
     */
    if ((g_view.frames % 30) == 0)
    {
        if (std::FILE *marker = std::fopen("/data/slopfin-quit", "rb"); marker != nullptr)
        {
            (void)std::fclose(marker);
            (void)unlink("/data/slopfin-quit");
            // The main loop owns teardown. Presenting after closing VideoOut
            // dereferences the deleted flip queue if process termination returns.
            g_exit_requested = true;
            trace::mark("app: close requested");
        }
    }
}

std::uint64_t now_us() noexcept
{
    struct
    {
        std::int64_t seconds;
        std::int64_t microseconds;
    } stamp{};
    if (sceKernelGettimeofday(&stamp) != 0)
        return 0;
    return static_cast<std::uint64_t>(stamp.seconds) * 1000000u +
           static_cast<std::uint64_t>(stamp.microseconds);
}

void note_present(std::uint64_t microseconds) noexcept
{
    g_present_us = microseconds;
}

void note_pad(std::uint64_t microseconds) noexcept
{
    g_pad_us = microseconds;
}

} // namespace slopfin::app
