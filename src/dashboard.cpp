/*
 * SlopFin - the server's dashboard, for administrators.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  Endpoints are Jellyfin 10.11's, checked against the
 * server's own OpenAPI document rather than remembered: 10.11 moved the
 * activity log to /System/ActivityLog/Entries, for one.
 *
 * Every page is built on the background thread into rows, and the render
 * thread only draws rows and sends back what was chosen. The two settings
 * pages keep the configuration the server sent, change it in place and post
 * the whole document back, so fields this screen does not show survive.
 */

#include "dashboard.hpp"

#include "account.hpp"
#include "background.hpp"
#include "config.hpp"
#include "diagnostics.hpp"
#include "gfx.hpp"
#include "http.hpp"
#include "icons.hpp"
#include "ime.hpp"
#include "jellyfin.hpp"
#include "json.hpp"
#include "pad.hpp"
#include "text.hpp"
#include "trace.hpp"
#include "ui_common.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace slopfin::dashboard
{
namespace
{
using text::Weight;
using namespace slopfin::ui;

/* ---------------------------------------------------------------- pages */

enum class PageId : unsigned char
{
    overview,
    activity,
    devices,
    libraries,
    users,
    tasks,
    playback,
    general,
    logs,
    plugins,
};

struct PageInfo
{
    PageId id;
    const char *label;
};

constexpr PageInfo kPages[] = {
    {PageId::overview, "Overview"}, {PageId::activity, "Activity"},
    {PageId::devices, "Devices"},   {PageId::libraries, "Libraries"},
    {PageId::users, "Users"},       {PageId::tasks, "Scheduled tasks"},
    {PageId::playback, "Playback"}, {PageId::general, "General"},
    {PageId::logs, "Logs"},         {PageId::plugins, "Plugins"},
};
constexpr int kPageCount = static_cast<int>(sizeof(kPages) / sizeof(kPages[0]));

/* A second of holding, at sixty frames. */
constexpr int kHoldFrames = 60;

/* ---------------------------------------------------------------- rows */

enum class Special : unsigned char
{
    none,
    message,  /* ask for text, then post it to the session */
    view_log, /* open the named log */
    send_log, /* put the end of the named log on the report server */
    policy,   /* flip a policy flag on a user */
    save,     /* post the page's configuration back */
};

struct Action
{
    std::string label;
    std::string method;
    std::string path;
    std::string body;
    std::string confirm; /* asked first when not empty */
    Special special = Special::none;
    std::string arg;    /* log name, or the policy field */
    bool value = false; /* the policy value */
    /*
     * Something that cannot simply be done again: deleting a user, wiping a
     * library's metadata, taking the server down. These sit apart at the foot
     * of the menu and are not run by a press -- the button has to be held.
     *
     */
    bool destructive = false;

    Action(std::string label_ = {}, std::string method_ = {}, std::string path_ = {},
           std::string body_ = {}, std::string confirm_ = {}, Special special_ = Special::none,
           std::string arg_ = {}, bool value_ = false, bool destructive_ = false)
        : label(std::move(label_)), method(std::move(method_)), path(std::move(path_)),
          body(std::move(body_)), confirm(std::move(confirm_)), special(special_),
          arg(std::move(arg_)), value(value_), destructive(destructive_)
    {
    }
};

/* Marks an action destructive without repeating its fields. */
Action grave(Action action) noexcept
{
    action.destructive = true;
    return action;
}

enum class FieldType : unsigned char
{
    boolean,
    integer,
    text,
    choice,
};

struct Field
{
    const char *key;
    const char *label;
    FieldType type;
    const char *hint;
    std::vector<const char *> choices{};
};

struct Row
{
    enum class Kind : unsigned char
    {
        header,
        item,
        field,
        note,
    } kind = Kind::item;
    std::string title;
    std::string subtitle;
    std::string right;
    float progress = -1.0f;
    bool warning = false;
    std::vector<Action> actions;
    int field = -1; /* index into the page's fields */
};

struct PageData
{
    std::vector<Row> rows;
    std::string error;
    /* Settings pages: the document the server sent, and where it goes back. */
    std::unique_ptr<json::Value> config;
    std::string config_key;
};

const std::vector<Field> &playback_fields() noexcept
{
    static const std::vector<Field> fields = {
        {"HardwareAccelerationType",
         "Hardware acceleration",
         FieldType::choice,
         "Which GPU encoder transcodes use.",
         {"none", "nvenc", "qsv", "vaapi", "amf", "videotoolbox", "v4l2m2m", "rkmpp"}},
        {"EnableHardwareEncoding", "Hardware encoding", FieldType::boolean,
         "Encode on the GPU as well as decode."},
        {"EnableTonemapping", "Tone mapping", FieldType::boolean,
         "Convert HDR to SDR when a client needs SDR."},
        {"EnableThrottling", "Throttle transcodes", FieldType::boolean,
         "Pause a transcode once it is far enough ahead of the viewer."},
        {"ThrottleDelaySeconds", "Throttle after (seconds)", FieldType::integer,
         "How far ahead a transcode gets before it is paused."},
        {"EnableSegmentDeletion", "Delete old segments", FieldType::boolean,
         "Remove transcoded segments the viewer is past."},
        {"SegmentKeepSeconds", "Keep segments (seconds)", FieldType::integer,
         "How long a watched segment is kept."},
        {"EncodingThreadCount", "Encoding threads", FieldType::integer, "-1 lets Jellyfin decide."},
        {"H264Crf", "H.264 quality (CRF)", FieldType::integer,
         "Lower is better quality and larger."},
        {"H265Crf", "HEVC quality (CRF)", FieldType::integer,
         "Lower is better quality and larger."},
    };
    return fields;
}

const std::vector<Field> &general_fields() noexcept
{
    static const std::vector<Field> fields = {
        {"ServerName", "Server name", FieldType::text, "What clients call this server."},
        {"PreferredMetadataLanguage", "Metadata language", FieldType::text,
         "A two-letter code, such as en."},
        {"MetadataCountryCode", "Metadata country", FieldType::text,
         "A two-letter code, such as US."},
        {"QuickConnectAvailable", "Quick Connect", FieldType::boolean,
         "Let devices sign in with a code."},
        {"LibraryMonitorDelay", "Library monitor delay (seconds)", FieldType::integer,
         "How long to wait after a file changes before scanning it."},
        {"LibraryScanFanoutConcurrency", "Parallel library scans", FieldType::integer,
         "0 lets Jellyfin decide."},
        {"ParallelImageEncodingLimit", "Parallel image encodes", FieldType::integer,
         "0 lets Jellyfin decide."},
        {"EnableMetrics", "Prometheus metrics", FieldType::boolean,
         "Expose /metrics for monitoring."},
    };
    return fields;
}

const std::vector<Field> &fields_for(PageId page) noexcept
{
    static const std::vector<Field> none;
    return page == PageId::playback  ? playback_fields()
           : page == PageId::general ? general_fields()
                                     : none;
}

/* ---------------------------------------------------------------- json out */

void write_json(const json::Value &value, std::string &out) noexcept
{
    switch (value.type)
    {
    case json::Type::null:
        out += "null";
        break;
    case json::Type::boolean:
        out += value.boolean ? "true" : "false";
        break;
    case json::Type::number:
    {
        char buffer[40];
        const double n = value.number;
        if (std::floor(n) == n && std::fabs(n) < 9.0e15)
            std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(n));
        else
            std::snprintf(buffer, sizeof(buffer), "%.17g", n);
        out += buffer;
        break;
    }
    case json::Type::string:
        out += '"';
        out += json::escape(value.text);
        out += '"';
        break;
    case json::Type::array:
        out += '[';
        for (std::size_t i = 0; i < value.elements.size(); ++i)
        {
            if (i > 0)
                out += ',';
            if (value.elements[i])
                write_json(*value.elements[i], out);
            else
                out += "null";
        }
        out += ']';
        break;
    case json::Type::object:
        out += '{';
        for (std::size_t i = 0; i < value.fields.size(); ++i)
        {
            if (i > 0)
                out += ',';
            out += '"';
            out += json::escape(value.fields[i].first);
            out += "\":";
            if (value.fields[i].second)
                write_json(*value.fields[i].second, out);
            else
                out += "null";
        }
        out += '}';
        break;
    }
}

json::Value *field_value(json::Value *object, const char *key) noexcept
{
    if (object == nullptr || object->type != json::Type::object)
        return nullptr;
    for (auto &[name, value] : object->fields)
        if (name == key)
            return value.get();
    return nullptr;
}

/* ---------------------------------------------------------------- words */

std::string text_of(const json::Value *node, std::string_view key) noexcept
{
    return node != nullptr ? std::string{node->str(key)} : std::string{};
}

/* Seconds since the epoch for an ISO 8601 UTC time, 0 when it will not parse. */
std::int64_t parse_iso(const std::string &iso) noexcept
{
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute,
                    &second) < 3)
        return 0;
    /* Days from the civil calendar (Howard Hinnant's algorithm). */
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const std::int64_t yoe = year - era * 400;
    const std::int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days = era * 146097 + doe - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

std::string ago(const std::string &iso) noexcept
{
    const std::int64_t then = parse_iso(iso);
    if (then <= 0)
        return {};
    const std::int64_t seconds = static_cast<std::int64_t>(std::time(nullptr)) - then;
    if (seconds < 60)
        return "just now";
    const auto unit = [](std::int64_t n, const char *one)
    { return std::to_string(n) + " " + one + (n == 1 ? "" : "s") + " ago"; };
    if (seconds < 3600)
        return unit(seconds / 60, "minute");
    if (seconds < 86400)
        return unit(seconds / 3600, "hour");
    if (seconds < 86400 * 60)
        return unit(seconds / 86400, "day");
    return unit(seconds / (86400 * 30), "month");
}

std::string clock_of(double ticks) noexcept
{
    const long total = static_cast<long>(ticks / 10000000.0);
    char buffer[24];
    if (total >= 3600)
        std::snprintf(buffer, sizeof(buffer), "%ld:%02ld:%02ld", total / 3600, (total / 60) % 60,
                      total % 60);
    else
        std::snprintf(buffer, sizeof(buffer), "%ld:%02ld", total / 60, total % 60);
    return buffer;
}

std::string size_of(double bytes) noexcept
{
    char buffer[32];
    if (bytes >= 1024.0 * 1024.0)
        std::snprintf(buffer, sizeof(buffer), "%.1f MB", bytes / (1024.0 * 1024.0));
    else
        std::snprintf(buffer, sizeof(buffer), "%.0f KB", bytes / 1024.0);
    return buffer;
}

std::string with_commas(long long n) noexcept
{
    std::string digits = std::to_string(n);
    for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3)
        digits.insert(static_cast<std::size_t>(at), ",");
    return digits;
}

json::ValuePtr fetch(const std::string &path, std::string &error) noexcept
{
    const http::Response response = jellyfin::api("GET", path);
    if (!response.ok())
    {
        error = response.status == 0 ? "Could not reach the server."
                : response.status == 401 || response.status == 403
                    ? "The server says this account may not see that."
                    : "The server answered " + std::to_string(response.status) + " for " + path;
        return nullptr;
    }
    json::ValuePtr root = json::parse(response.body);
    if (!root)
        error = "The server's answer could not be read.";
    return root;
}

Row header(const std::string &title) noexcept
{
    Row row;
    row.kind = Row::Kind::header;
    row.title = title;
    return row;
}

Row note(const std::string &title) noexcept
{
    Row row;
    row.kind = Row::Kind::note;
    row.title = title;
    return row;
}

Row activity_row(const json::Value &entry) noexcept
{
    Row row;
    row.title = text_of(&entry, "Name");
    row.subtitle = text_of(&entry, "ShortOverview");
    if (row.subtitle.empty())
        row.subtitle = text_of(&entry, "Overview");
    row.right = ago(text_of(&entry, "Date"));
    const std::string severity = text_of(&entry, "Severity");
    row.warning = severity == "Error" || severity == "Warning" || severity == "Critical";
    return row;
}

/* ---------------------------------------------------------------- builders */

void build_sessions(PageData &page, std::string &error) noexcept
{
    json::ValuePtr sessions = fetch("/Sessions?activeWithinSeconds=960", error);
    if (!sessions)
        return;
    page.rows.push_back(header("Active devices"));
    int shown = 0;
    for (const json::ValuePtr &node : sessions->elements)
    {
        if (!node)
            continue;
        const json::Value &session = *node;
        const std::string id = text_of(&session, "Id");
        const std::string user = text_of(&session, "UserName");
        Row row;
        row.title = (user.empty() ? std::string{"Nobody signed in"} : user) + "  \xc2\xb7  " +
                    text_of(&session, "Client") + " on " + text_of(&session, "DeviceName");
        const json::Value *playing = session.find("NowPlayingItem");
        const json::Value *state = session.find("PlayState");
        if (playing != nullptr && playing->type == json::Type::object)
        {
            std::string title = text_of(playing, "Name");
            const std::string series = text_of(playing, "SeriesName");
            if (!series.empty())
            {
                const int season = static_cast<int>(playing->num("ParentIndexNumber"));
                const int episode = static_cast<int>(playing->num("IndexNumber"));
                title =
                    series +
                    (season > 0 ? "  S" + std::to_string(season) + ":E" + std::to_string(episode)
                                : "") +
                    "  " + title;
            }
            const double runtime = playing->num("RunTimeTicks");
            const double position = state != nullptr ? state->num("PositionTicks") : 0.0;
            const bool paused = state != nullptr && state->flag("IsPaused");
            row.subtitle = std::string{paused ? "Paused  " : "Playing  "} + title;
            if (runtime > 0.0)
            {
                row.progress = static_cast<float>(std::clamp(position / runtime, 0.0, 1.0));
                row.right = clock_of(position) + " / " + clock_of(runtime);
            }
            const json::Value *transcode = session.find("TranscodingInfo");
            const std::string method =
                state != nullptr ? text_of(state, "PlayMethod") : std::string{};
            if (transcode != nullptr && transcode->type == json::Type::object)
            {
                const bool video_direct = transcode->flag("IsVideoDirect");
                const bool audio_direct = transcode->flag("IsAudioDirect");
                std::string detail =
                    "Transcoding: video " +
                    std::string{video_direct ? "copied" : text_of(transcode, "VideoCodec")} +
                    ", audio " +
                    (audio_direct ? std::string{"copied"} : text_of(transcode, "AudioCodec"));
                const double bitrate = transcode->num("Bitrate");
                if (bitrate > 0.0)
                {
                    char mbps[24];
                    std::snprintf(mbps, sizeof(mbps), ", %.1f Mb/s", bitrate / 1e6);
                    detail += mbps;
                }
                row.subtitle += "\n" + detail;
            }
            else if (!method.empty())
                row.subtitle += "\n" + method;
            if (session.flag("SupportsRemoteControl") || !id.empty())
            {
                row.actions.push_back(
                    {paused ? "Resume" : "Pause", "POST",
                     "/Sessions/" + id + "/Playing/" + (paused ? "Unpause" : "Pause")});
                row.actions.push_back({"Stop playback",
                                       "POST",
                                       "/Sessions/" + id + "/Playing/Stop",
                                       {},
                                       "Stop what " +
                                           (user.empty() ? std::string{"this device"} : user) +
                                           " is watching?"});
            }
        }
        else
        {
            row.subtitle = "Idle, last seen " + ago(text_of(&session, "LastActivityDate"));
        }
        if (!id.empty())
        {
            Action message{"Send a message", "POST", "/Sessions/" + id + "/Message"};
            message.special = Special::message;
            row.actions.push_back(message);
        }
        page.rows.push_back(std::move(row));
        ++shown;
    }
    if (shown == 0)
        page.rows.push_back(note("No devices have been active in the last quarter of an hour."));
}

void build_overview(PageData &page, std::string &error) noexcept
{
    json::ValuePtr info = fetch("/System/Info", error);
    if (!info)
        return;
    page.rows.push_back(header("Server"));
    Row server;
    server.title = text_of(info.get(), "ServerName");
    server.subtitle = "Jellyfin " + text_of(info.get(), "Version");
    const std::string os = text_of(info.get(), "OperatingSystemDisplayName");
    if (!os.empty())
        server.subtitle += "  \xc2\xb7  " + os;
    if (info->flag("HasPendingRestart"))
    {
        server.right = "Restart needed";
        server.warning = true;
    }
    else if (info->flag("HasUpdateAvailable"))
    {
        server.right = "Update available";
        server.warning = true;
    }
    server.actions.push_back({"Scan all libraries",
                              "POST",
                              "/Library/Refresh",
                              {},
                              "Scan every library for new and changed files?"});
    server.actions.push_back(grave({"Restart server",
                                    "POST",
                                    "/System/Restart",
                                    {},
                                    "Restart the server? Everyone watching will be interrupted."}));
    server.actions.push_back(grave(
        {"Shut down server",
         "POST",
         "/System/Shutdown",
         {},
         "Shut the server down? It will stay off until it is started again on the machine."}));
    page.rows.push_back(std::move(server));

    std::string ignored;
    if (json::ValuePtr counts = fetch("/Items/Counts", ignored))
    {
        Row library;
        const auto count = [&counts](const char *key)
        { return static_cast<long long>(counts->num(key)); };
        library.title = with_commas(count("MovieCount")) + " movies  \xc2\xb7  " +
                        with_commas(count("SeriesCount")) + " shows  \xc2\xb7  " +
                        with_commas(count("EpisodeCount")) + " episodes";
        const long long songs = count("SongCount");
        const long long books = count("BookCount");
        if (songs > 0 || books > 0)
            library.subtitle =
                with_commas(songs) + " songs  \xc2\xb7  " + with_commas(books) + " books";
        page.rows.push_back(std::move(library));
    }

    build_sessions(page, ignored);

    if (json::ValuePtr log = fetch("/System/ActivityLog/Entries?startIndex=0&limit=6", ignored))
    {
        page.rows.push_back(header("Latest activity"));
        if (const json::Value *items = log->find("Items"))
            for (const json::ValuePtr &entry : items->elements)
                if (entry)
                    page.rows.push_back(activity_row(*entry));
    }
}

void build_activity(PageData &page, std::string &error) noexcept
{
    json::ValuePtr log = fetch("/System/ActivityLog/Entries?startIndex=0&limit=80", error);
    if (!log)
        return;
    if (const json::Value *items = log->find("Items"))
        for (const json::ValuePtr &entry : items->elements)
            if (entry)
                page.rows.push_back(activity_row(*entry));
    if (page.rows.empty())
        page.rows.push_back(note("Nothing has been recorded yet."));
}

void build_devices(PageData &page, std::string &error) noexcept
{
    json::ValuePtr devices = fetch("/Devices", error);
    if (!devices)
        return;
    if (const json::Value *items = devices->find("Items"))
        for (const json::ValuePtr &node : items->elements)
        {
            if (!node)
                continue;
            Row row;
            row.title = text_of(node.get(), "Name");
            row.subtitle = text_of(node.get(), "AppName") + " " + text_of(node.get(), "AppVersion");
            const std::string last_user = text_of(node.get(), "LastUserName");
            if (!last_user.empty())
                row.subtitle += "  \xc2\xb7  " + last_user;
            row.right = ago(text_of(node.get(), "DateLastActivity"));
            const std::string id = text_of(node.get(), "Id");
            if (!id.empty())
                row.actions.push_back(
                    grave({"Remove device",
                           "DELETE",
                           "/Devices?id=" + http::url_encode(id),
                           {},
                           "Remove " + row.title + "? It will have to sign in again."}));
            page.rows.push_back(std::move(row));
        }
}

void build_libraries(PageData &page, std::string &error) noexcept
{
    json::ValuePtr folders = fetch("/Library/VirtualFolders", error);
    if (!folders)
        return;
    Row all;
    all.title = "All libraries";
    all.subtitle = "Look for new and changed files everywhere";
    all.actions.push_back({"Scan all libraries",
                           "POST",
                           "/Library/Refresh",
                           {},
                           "Scan every library for new and changed files?"});
    page.rows.push_back(std::move(all));
    for (const json::ValuePtr &node : folders->elements)
    {
        if (!node)
            continue;
        Row row;
        row.title = text_of(node.get(), "Name");
        const std::string type = text_of(node.get(), "CollectionType");
        row.subtitle = type.empty() ? std::string{"Mixed"} : type;
        if (const json::Value *locations = node->find("Locations"))
            for (const json::ValuePtr &location : locations->elements)
                if (location)
                    row.subtitle += "  \xc2\xb7  " + location->text;
        const double progress = node->num("RefreshProgress", -1.0);
        if (progress >= 0.0 && text_of(node.get(), "RefreshStatus") == "Active")
        {
            row.progress = static_cast<float>(progress / 100.0);
            row.right = "Scanning";
        }
        const std::string id = text_of(node.get(), "ItemId");
        if (!id.empty())
        {
            /* The everyday one first: look for what has changed. */
            row.actions.push_back(
                {"Scan this library", "POST",
                 "/Items/" + id +
                     "/Refresh?Recursive=true&ImageRefreshMode=Default&MetadataRefreshMode=Default"
                     "&ReplaceAllImages=false&ReplaceAllMetadata=false"});
            row.actions.push_back(
                grave({"Replace all metadata",
                       "POST",
                       "/Items/" + id +
                           "/Refresh?Recursive=true&ImageRefreshMode=FullRefresh"
                           "&MetadataRefreshMode=FullRefresh&ReplaceAllMetadata=true"
                           "&ReplaceAllImages=true",
                       {},
                       "Fetch every piece of metadata and artwork for this library again? "
                       "It takes a while and overwrites what is there."}));
        }
        page.rows.push_back(std::move(row));
    }
}

void build_users(PageData &page, std::string &error) noexcept
{
    const std::vector<jellyfin::User> users = jellyfin::users(error);
    if (users.empty())
        return;
    const std::string self = config::current().user_id;
    for (const jellyfin::User &user : users)
    {
        Row row;
        row.title = user.name;
        row.subtitle = std::string{user.is_admin ? "Administrator" : "User"} +
                       (user.is_disabled ? "  \xc2\xb7  Disabled" : "");
        row.warning = user.is_disabled;
        row.right = user.last_activity.empty() ? std::string{"Never active"}
                                               : "Active " + ago(user.last_activity);
        if (user.id != self)
        {
            Action disable{user.is_disabled ? "Enable user" : "Disable user"};
            disable.special = Special::policy;
            disable.arg = "IsDisabled";
            disable.value = !user.is_disabled;
            disable.path = user.id;
            disable.confirm = user.is_disabled ? "Let " + user.name + " sign in again?"
                                               : "Stop " + user.name + " signing in?";
            row.actions.push_back(disable);
            Action admin{user.is_admin ? "Remove administrator" : "Make administrator"};
            admin.special = Special::policy;
            admin.arg = "IsAdministrator";
            admin.value = !user.is_admin;
            admin.path = user.id;
            admin.confirm = user.is_admin ? "Take away " + user.name + "'s administrator rights?"
                                          : "Give " + user.name +
                                                " administrator rights, including this dashboard?";
            row.actions.push_back(admin);
        }
        /*
         * The switches an administrator actually reaches for, the way the web
         * dashboard groups them under a user. Each is one policy flag, posted
         * back with the rest of that user's policy untouched.
         *
         */
        struct Switch
        {
            const char *flag;
            const char *on; /* what choosing it does when the flag is false */
            const char *off;
            bool value;
        };
        const Switch switches[] = {
            {"EnableRemoteAccess", "Allow access from outside the network",
             "Block access from outside the network", user.remote_access},
            {"EnableContentDownloading", "Allow downloads", "Stop downloads", user.can_download},
            {"EnableLiveTvAccess", "Allow Live TV", "Stop Live TV", user.live_tv},
            {"EnableMediaPlayback", "Allow playback", "Stop playback altogether", user.can_play},
        };
        for (const Switch &entry : switches)
        {
            Action toggle{entry.value ? entry.off : entry.on};
            toggle.special = Special::policy;
            toggle.arg = entry.flag;
            toggle.value = !entry.value;
            toggle.path = user.id;
            toggle.confirm =
                std::string{entry.value ? entry.off : entry.on} + " for " + user.name + "?";
            row.actions.push_back(toggle);
        }
        Action password{"Reset the password"};
        password.destructive = true;
        password.method = "POST";
        password.path = "/Users/Password?userId=" + user.id;
        password.body = R"({"ResetPassword":true})";
        password.confirm = "Clear " + user.name +
                           "'s password? They will sign in with no password until they set one.";
        row.actions.push_back(password);
        if (user.id != self)
        {
            Action remove{"Delete this user"};
            remove.destructive = true;
            remove.method = "DELETE";
            remove.path = "/Users/" + user.id;
            remove.confirm =
                "Delete " + user.name + " and everything about them? This cannot be undone.";
            row.actions.push_back(remove);
        }
        page.rows.push_back(std::move(row));
    }

    Row everywhere;
    everywhere.title = "Sign every device out";
    everywhere.subtitle = "Ends every session on this server, this console included.";
    everywhere.actions.push_back(grave({"Sign every device out",
                                        "POST",
                                        "/Sessions/Logout",
                                        {},
                                        "Sign every device out, including this one?"}));
    page.rows.push_back(std::move(everywhere));
}

void build_tasks(PageData &page, std::string &error) noexcept
{
    json::ValuePtr tasks = fetch("/ScheduledTasks?isHidden=false", error);
    if (!tasks)
        return;
    std::vector<const json::Value *> sorted;
    for (const json::ValuePtr &node : tasks->elements)
        if (node)
            sorted.push_back(node.get());
    std::stable_sort(sorted.begin(), sorted.end(), [](const json::Value *a, const json::Value *b)
                     { return a->str("Category") < b->str("Category"); });
    std::string category;
    for (const json::Value *task : sorted)
    {
        if (task->str("Category") != category)
        {
            category = std::string{task->str("Category")};
            page.rows.push_back(header(category));
        }
        Row row;
        row.title = text_of(task, "Name");
        const std::string state = text_of(task, "State");
        const std::string id = text_of(task, "Id");
        if (state == "Running")
        {
            const double progress = task->num("CurrentProgressPercentage");
            row.progress = static_cast<float>(progress / 100.0);
            char percent[16];
            std::snprintf(percent, sizeof(percent), "%.0f%%", progress);
            row.right = percent;
            row.subtitle = "Running";
            row.actions.push_back({"Stop", "DELETE", "/ScheduledTasks/Running/" + id});
        }
        else
        {
            const json::Value *last = task->find("LastExecutionResult");
            if (last != nullptr && last->type == json::Type::object)
            {
                row.subtitle = "Last ran " + ago(text_of(last, "EndTimeUtc"));
                const std::string status = text_of(last, "Status");
                if (!status.empty() && status != "Completed")
                {
                    row.right = status;
                    row.warning = true;
                }
            }
            else
                row.subtitle = "Has not run yet";
            if (state == "Cancelling")
                row.right = "Stopping";
            else
                row.actions.push_back({"Run now", "POST", "/ScheduledTasks/Running/" + id});
        }
        page.rows.push_back(std::move(row));
    }
}

void build_settings(PageData &page, PageId id, std::string &error) noexcept
{
    const std::string key = id == PageId::playback ? "encoding" : "";
    const std::string path = key.empty() ? "/System/Configuration" : "/System/Configuration/" + key;
    json::ValuePtr config = fetch(path, error);
    if (!config)
        return;
    const std::vector<Field> &fields = fields_for(id);
    for (std::size_t i = 0; i < fields.size(); ++i)
    {
        if (field_value(config.get(), fields[i].key) == nullptr)
            continue; /* not something this server version has */
        Row row;
        row.kind = Row::Kind::field;
        row.field = static_cast<int>(i);
        row.title = fields[i].label;
        row.subtitle = fields[i].hint;
        page.rows.push_back(std::move(row));
    }
    Row save;
    save.title = "Save changes";
    save.subtitle = "Nothing is sent to the server until this is chosen.";
    Action action{"Save changes"};
    action.special = Special::save;
    action.confirm = "Save these settings on the server?";
    save.actions.push_back(action);
    page.rows.push_back(std::move(save));
    page.config.reset(config.release());
    page.config_key = path;
}

void build_logs(PageData &page, std::string &error) noexcept
{
    json::ValuePtr logs = fetch("/System/Logs", error);
    if (!logs)
        return;
    for (const json::ValuePtr &node : logs->elements)
    {
        if (!node)
            continue;
        Row row;
        row.title = text_of(node.get(), "Name");
        row.subtitle = size_of(node->num("Size"));
        row.right = ago(text_of(node.get(), "DateModified"));
        Action view{"View the end of this log"};
        view.special = Special::view_log;
        view.arg = row.title;
        row.actions.push_back(view);
        if (diagnostics::destination(config::current().report_server).valid())
        {
            Action send{"Send to report receiver"};
            send.special = Special::send_log;
            send.arg = row.title;
            row.actions.push_back(send);
        }
        page.rows.push_back(std::move(row));
    }
}

void build_plugins(PageData &page, std::string &error) noexcept
{
    json::ValuePtr plugins = fetch("/Plugins", error);
    if (!plugins)
        return;
    for (const json::ValuePtr &node : plugins->elements)
    {
        if (!node)
            continue;
        Row row;
        row.title = text_of(node.get(), "Name");
        row.subtitle = text_of(node.get(), "Description");
        const std::string status = text_of(node.get(), "Status");
        const std::string id = text_of(node.get(), "Id");
        const std::string version = text_of(node.get(), "Version");
        row.right = version + "  " + status;
        row.warning = status != "Active";
        if (!id.empty() && !version.empty())
        {
            const std::string base = "/Plugins/" + id + "/" + version;
            if (status == "Disabled")
                row.actions.push_back(
                    {"Enable",
                     "POST",
                     base + "/Enable",
                     {},
                     "Enable " + row.title + "? The server restarts to pick it up."});
            else
                row.actions.push_back(
                    {"Disable",
                     "POST",
                     base + "/Disable",
                     {},
                     "Disable " + row.title + "? The server restarts to drop it."});
            row.actions.push_back(grave(
                {"Uninstall", "DELETE", base, {}, "Remove " + row.title + " from the server?"}));
        }
        page.rows.push_back(std::move(row));
    }
}

/* ---------------------------------------------------------------- state */

struct Shared
{
    std::mutex mutex;
    /* A finished page, waiting for the render thread to take it. */
    std::unique_ptr<PageData> arrived;
    PageId arrived_for = PageId::overview;
    bool loading = false;
    std::string message;
    bool message_is_error = false;
    /* The log viewer's lines, once fetched. */
    std::vector<std::string> log_lines;
    std::string log_name;
    bool log_ready = false;
};
Shared g_shared;

enum class Mode : unsigned char
{
    browse,
    actions, /* the focused row's list of things to do */
    confirm,
    log,
};

struct View
{
    int page = 0;
    bool content_focus = false;
    int row = 0;
    int scroll = 0; /* pixels */
    Mode mode = Mode::browse;
    int action_index = 0;
    Action pending; /* what the confirm dialog is about */
    bool confirm_yes = false;
    /* Frames the button has been held on a destructive confirmation. */
    int hold = 0;
    PageData data; /* the page on screen */
    bool have_data = false;
    PageId data_for = PageId::overview;
    int frames_since_load = 0;
    /* A keyboard this screen asked for, and what for. */
    enum class Prompt : unsigned char
    {
        none,
        message,
        field,
    } prompt = Prompt::none;
    std::string prompt_path;
    int prompt_field = -1;
    int log_scroll = 0;
};
View g_view;

void load_page(PageId id) noexcept
{
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        g_shared.loading = true;
    }
    background::run(
        [id]
        {
            auto page = std::make_unique<PageData>();
            std::string error;
            switch (id)
            {
            case PageId::overview:
                build_overview(*page, error);
                break;
            case PageId::activity:
                build_activity(*page, error);
                break;
            case PageId::devices:
                build_devices(*page, error);
                break;
            case PageId::libraries:
                build_libraries(*page, error);
                break;
            case PageId::users:
                build_users(*page, error);
                break;
            case PageId::tasks:
                build_tasks(*page, error);
                break;
            case PageId::playback:
            case PageId::general:
                build_settings(*page, id, error);
                break;
            case PageId::logs:
                build_logs(*page, error);
                break;
            case PageId::plugins:
                build_plugins(*page, error);
                break;
            }
            page->error = error;
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            g_shared.arrived = std::move(page);
            g_shared.arrived_for = id;
            g_shared.loading = false;
        });
}

PageId current_page() noexcept
{
    return kPages[std::clamp(g_view.page, 0, kPageCount - 1)].id;
}

void notify(const std::string &message, bool error) noexcept
{
    std::lock_guard<std::mutex> guard(g_shared.mutex);
    g_shared.message = message;
    g_shared.message_is_error = error;
}

void run_action(const Action &action, const std::string &text = {}) noexcept
{
    const PageId page = current_page();
    std::string body = action.body;
    std::unique_ptr<json::Value> config;
    std::string config_key;
    if (action.special == Special::save && g_view.data.config)
    {
        std::string serialized;
        write_json(*g_view.data.config, serialized);
        body = std::move(serialized);
        config_key = g_view.data.config_key;
    }
    if (action.special == Special::message)
        body =
            "{\"Header\":\"SlopFin\",\"Text\":\"" + json::escape(text) + "\",\"TimeoutMs\":10000}";
    notify(action.label + "\xe2\x80\xa6", false);
    background::run(
        [action, body, config_key, page]
        {
            bool ok = false;
            std::string failure;
            if (action.special == Special::send_log)
            {
                /* Upload only the log tail to the explicitly configured receiver. */
                constexpr std::size_t kTail = 64u * 1024u;
                const config::Settings &settings = config::current();
                const auto destination = diagnostics::destination(settings.report_server);
                if (!destination.valid())
                {
                    notify("No report receiver is configured. Logs remain on the Jellyfin server.",
                           true);
                    return;
                }
                std::string tail;
                http::Stream stream;
                if (stream.open(settings.host, settings.port,
                                "/System/Logs/Log?name=" + http::url_encode(action.arg),
                                jellyfin::authorization()) &&
                    stream.status() >= 200 && stream.status() < 300)
                {
                    char chunk[16384];
                    long got = 0;
                    while ((got = stream.read(chunk, sizeof(chunk))) > 0)
                    {
                        tail.append(chunk, static_cast<std::size_t>(got));
                        if (tail.size() > kTail * 2)
                            tail.erase(0, tail.size() - kTail);
                    }
                }
                stream.close();
                if (tail.size() > kTail)
                    tail.erase(0, tail.size() - kTail);
                if (tail.empty())
                    failure = "that log could not be read";
                else
                {
                    const std::vector<std::string> headers{"X-SlopFin-Kind: server-log",
                                                           "X-SlopFin-Build: " + action.arg,
                                                           "X-SlopFin-Device: dashboard"};
                    const http::Response answer =
                        http::post(destination.host, destination.port, "/report", headers, tail,
                                   "text/plain; charset=utf-8");
                    ok = answer.ok();
                    if (!ok)
                        failure = answer.status > 0
                                      ? "the receiver answered " + std::to_string(answer.status)
                                      : "the configured report receiver could not be reached";
                }
                trace::mark("dashboard: send log " + action.arg + (ok ? " ok" : " failed"));
                std::lock_guard<std::mutex> guard(g_shared.mutex);
                g_shared.message = ok ? action.arg + " is on the report server"
                                      : "Could not send it (" + failure + ")";
                g_shared.message_is_error = !ok;
                return;
            }
            if (action.special == Special::view_log)
            {
                /* The end of the log only: a whole day's log is megabytes, and the
                   process heap holds about ten. */
                constexpr std::size_t kTail = 96u * 1024u;
                const config::Settings &settings = config::current();
                http::Stream stream;
                std::string tail;
                if (stream.open(settings.host, settings.port,
                                "/System/Logs/Log?name=" + http::url_encode(action.arg),
                                jellyfin::authorization()) &&
                    stream.status() >= 200 && stream.status() < 300)
                {
                    char chunk[16384];
                    long got = 0;
                    while ((got = stream.read(chunk, sizeof(chunk))) > 0)
                    {
                        tail.append(chunk, static_cast<std::size_t>(got));
                        if (tail.size() > kTail * 2)
                            tail.erase(0, tail.size() - kTail);
                    }
                    ok = true;
                }
                stream.close();
                if (tail.size() > kTail)
                    tail.erase(0, tail.size() - kTail);
                std::vector<std::string> lines;
                std::size_t start = tail.find('\n');
                start = start == std::string::npos ? 0 : start + 1; /* the first line is cut */
                while (start < tail.size())
                {
                    std::size_t end = tail.find('\n', start);
                    if (end == std::string::npos)
                        end = tail.size();
                    std::string line = tail.substr(start, end - start);
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    lines.push_back(std::move(line));
                    start = end + 1;
                }
                std::lock_guard<std::mutex> guard(g_shared.mutex);
                g_shared.log_lines = std::move(lines);
                g_shared.log_name = action.arg;
                g_shared.log_ready = true;
                g_shared.message = ok ? std::string{} : "That log could not be read.";
                g_shared.message_is_error = !ok;
                return;
            }
            if (action.special == Special::policy)
            {
                std::string error;
                json::ValuePtr user = fetch("/Users/" + action.path, error);
                json::Value *policy = user ? field_value(user.get(), "Policy") : nullptr;
                json::Value *flag = field_value(policy, action.arg.c_str());
                if (flag != nullptr)
                {
                    flag->type = json::Type::boolean;
                    flag->boolean = action.value;
                    std::string serialized;
                    write_json(*policy, serialized);
                    const http::Response response =
                        jellyfin::api("POST", "/Users/" + action.path + "/Policy", serialized);
                    ok = response.ok();
                    if (!ok)
                        failure = "status " + std::to_string(response.status);
                }
                else
                    failure = error.empty() ? "that user's policy could not be read" : error;
            }
            else
            {
                const std::string &path =
                    action.special == Special::save ? config_key : action.path;
                const http::Response response =
                    jellyfin::api(action.method.empty() ? "POST" : action.method, path, body);
                ok = response.ok();
                if (!ok)
                    failure = response.status == 0 ? "the server did not answer"
                                                   : "status " + std::to_string(response.status);
            }
            trace::mark("dashboard: " + action.label + (ok ? " ok" : " failed, " + failure));
            {
                std::lock_guard<std::mutex> guard(g_shared.mutex);
                g_shared.message =
                    ok ? action.label + ": done" : action.label + " failed (" + failure + ")";
                g_shared.message_is_error = !ok;
            }
            if (action.method != "POST" || action.path.find("/System/") != 0)
            {
                /* Let a task or scan get going before the page is read again. */
                timespec pause{0, 600 * 1000 * 1000};
                (void)nanosleep(&pause, nullptr);
            }
            (void)page;
        });
    if (action.special != Special::view_log && action.special != Special::send_log &&
        action.path != "/System/Restart" && action.path != "/System/Shutdown")
        load_page(page);
}

/* ---------------------------------------------------------------- fields */

std::string field_text(const Field &field, const json::Value *value) noexcept
{
    if (value == nullptr)
        return {};
    switch (field.type)
    {
    case FieldType::boolean:
        return value->boolean ? "On" : "Off";
    case FieldType::integer:
        return std::to_string(static_cast<long long>(value->number));
    case FieldType::text:
    case FieldType::choice:
        return value->type == json::Type::null ? std::string{"Not set"} : value->text;
    }
    return {};
}

void step_field(const Field &field, json::Value *value, int direction) noexcept
{
    if (value == nullptr)
        return;
    if (field.type == FieldType::boolean)
    {
        value->type = json::Type::boolean;
        value->boolean = !value->boolean;
    }
    else if (field.type == FieldType::choice && !field.choices.empty())
    {
        const std::size_t count = field.choices.size();
        std::size_t at = 0;
        for (std::size_t i = 0; i < count; ++i)
            if (value->text == field.choices[i])
                at = i;
        at = direction < 0 ? (at == 0 ? count - 1 : at - 1) : (at + 1 == count ? 0 : at + 1);
        value->type = json::Type::string;
        value->text = field.choices[static_cast<std::size_t>(at)];
    }
}

/* ---------------------------------------------------------------- input */

bool focusable(const Row &row) noexcept
{
    return row.kind == Row::Kind::item || row.kind == Row::Kind::field;
}

void move_row(int direction) noexcept
{
    const auto &rows = g_view.data.rows;
    int at = g_view.row;
    for (int i = at + direction; i >= 0 && i < static_cast<int>(rows.size()); i += direction)
        if (focusable(rows[static_cast<std::size_t>(i)]))
        {
            at = i;
            break;
        }
    g_view.row = at;
}

void first_focusable() noexcept
{
    g_view.row = 0;
    const auto &rows = g_view.data.rows;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (focusable(rows[i]))
        {
            g_view.row = static_cast<int>(i);
            return;
        }
}

bool service_prompt() noexcept
{
    if (g_view.prompt == View::Prompt::none)
        return false;
    std::string entered;
    if (!ime::take_result(entered))
    {
        if (!ime::busy())
            g_view.prompt = View::Prompt::none;
        return true;
    }
    if (g_view.prompt == View::Prompt::message)
    {
        if (!entered.empty())
        {
            Action send{"Send a message", "POST", g_view.prompt_path};
            send.special = Special::message;
            run_action(send, entered);
        }
    }
    else if (g_view.prompt == View::Prompt::field && g_view.data.config)
    {
        const std::vector<Field> &fields = fields_for(g_view.data_for);
        if (g_view.prompt_field >= 0 && g_view.prompt_field < static_cast<int>(fields.size()))
        {
            const Field &field = fields[static_cast<std::size_t>(g_view.prompt_field)];
            if (json::Value *value = field_value(g_view.data.config.get(), field.key))
            {
                if (field.type == FieldType::integer)
                {
                    char *end = nullptr;
                    const long parsed = std::strtol(entered.c_str(), &end, 10);
                    if (end != nullptr && end != entered.c_str() && *end == '\0')
                    {
                        value->type = json::Type::number;
                        value->number = static_cast<double>(parsed);
                    }
                    else
                        notify("\"" + entered + "\" is not a whole number.", true);
                }
                else
                {
                    value->type = json::Type::string;
                    value->text = entered;
                }
            }
        }
    }
    g_view.prompt = View::Prompt::none;
    return true;
}

void execute(const Action &action) noexcept
{
    if (action.special == Special::message)
    {
        g_view.prompt = View::Prompt::message;
        g_view.prompt_path = action.path;
        ime::request("", "Message", "Shown on that device for ten seconds", ime::Mode::text);
        return;
    }
    if (action.special == Special::view_log)
    {
        {
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            g_shared.log_ready = false;
            g_shared.log_lines.clear();
            g_shared.log_name = action.arg;
        }
        g_view.mode = Mode::log;
        g_view.log_scroll = -1; /* to the end once it arrives */
    }
    run_action(action);
}

} // namespace

/* ---------------------------------------------------------------- public */

void open() noexcept
{
    g_view = View{};
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        g_shared.message.clear();
    }
    load_page(PageId::overview);
}

bool handle_input() noexcept
{
    if (service_prompt())
        return true;

    /* A page that has arrived replaces the one on screen. Refreshes keep the
       cursor where it was; a new page starts at its top. */
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        if (g_shared.arrived)
        {
            const bool same = g_view.have_data && g_view.data_for == g_shared.arrived_for;
            g_view.data = std::move(*g_shared.arrived);
            g_shared.arrived.reset();
            g_view.data_for = g_shared.arrived_for;
            g_view.have_data = true;
            g_view.frames_since_load = 0;
            if (!same)
            {
                g_view.scroll = 0;
                first_focusable();
            }
            g_view.row = std::clamp(g_view.row, 0,
                                    std::max(0, static_cast<int>(g_view.data.rows.size()) - 1));
        }
    }
    ++g_view.frames_since_load;
    bool loading = false;
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        loading = g_shared.loading;
    }
    /* Live pages read themselves again: devices every five seconds, a running
       task every two. */
    const PageId page = current_page();
    if (!loading && g_view.mode == Mode::browse && g_view.have_data && g_view.data_for == page)
    {
        bool running = false;
        for (const Row &row : g_view.data.rows)
            running = running || (row.progress >= 0.0f);
        /* Only what is actually moving is asked about often: a running task
           every two seconds, a scan every five, and the overview every ten.
           Nothing polls while another page is showing.  */
        if ((page == PageId::overview && g_view.frames_since_load > 600) ||
            (page == PageId::tasks && running && g_view.frames_since_load > 120) ||
            (page == PageId::libraries && running && g_view.frames_since_load > 300))
            load_page(page);
    }

    switch (g_view.mode)
    {
    case Mode::log:
        if (pad::pressed(pad::Button::circle))
            g_view.mode = Mode::browse;
        else if (pad::pressed(pad::Button::down))
            g_view.log_scroll += 3;
        else if (pad::pressed(pad::Button::up))
            g_view.log_scroll = std::max(0, g_view.log_scroll - 3);
        else if (pad::pressed(pad::Button::r1))
            g_view.log_scroll += 30;
        else if (pad::pressed(pad::Button::l1))
            g_view.log_scroll = std::max(0, g_view.log_scroll - 30);
        return true;
    case Mode::confirm:
        if (pad::pressed(pad::Button::circle))
        {
            g_view.mode = Mode::browse;
            g_view.hold = 0;
        }
        else if (pad::pressed(pad::Button::left) || pad::pressed(pad::Button::right))
        {
            g_view.confirm_yes = !g_view.confirm_yes;
            g_view.hold = 0;
        }
        else if (g_view.pending.destructive)
        {
            /*
             * Held, not pressed. Two presses of Cross in quick succession are
             * exactly how a menu is used, so a press alone must never be able
             * to delete a user or take the server down; a second of holding
             * cannot happen by accident. Letting go starts over.
             */
            if (!g_view.confirm_yes || !pad::held(pad::Button::cross))
                g_view.hold = 0;
            else if (++g_view.hold >= kHoldFrames)
            {
                g_view.hold = 0;
                g_view.mode = Mode::browse;
                execute(g_view.pending);
            }
        }
        else if (pad::pressed(pad::Button::cross))
        {
            g_view.mode = Mode::browse;
            if (g_view.confirm_yes)
                execute(g_view.pending);
        }
        return true;
    case Mode::actions:
    {
        const auto &rows = g_view.data.rows;
        if (g_view.row >= static_cast<int>(rows.size()))
        {
            g_view.mode = Mode::browse;
            return true;
        }
        const auto &actions = rows[static_cast<std::size_t>(g_view.row)].actions;
        const int count = static_cast<int>(actions.size());
        if (pad::pressed(pad::Button::circle) || count == 0)
            g_view.mode = Mode::browse;
        else if (pad::pressed(pad::Button::down) && g_view.action_index + 1 < count)
            ++g_view.action_index;
        else if (pad::pressed(pad::Button::up) && g_view.action_index > 0)
            --g_view.action_index;
        else if (pad::pressed(pad::Button::cross))
        {
            const Action chosen = actions[static_cast<std::size_t>(g_view.action_index)];
            if (!chosen.confirm.empty() || chosen.destructive)
            {
                g_view.pending = chosen;
                if (g_view.pending.confirm.empty())
                    g_view.pending.confirm = chosen.label + "?";
                g_view.confirm_yes = false;
                g_view.hold = 0;
                g_view.mode = Mode::confirm;
            }
            else
            {
                g_view.mode = Mode::browse;
                execute(chosen);
            }
        }
        return true;
    }
    case Mode::browse:
        break;
    }

    if (!g_view.content_focus)
    {
        if (pad::pressed(pad::Button::circle))
            return false;
        int wanted = g_view.page;
        if (pad::pressed(pad::Button::down) && wanted + 1 < kPageCount)
            ++wanted;
        else if (pad::pressed(pad::Button::up) && wanted > 0)
            --wanted;
        else if ((pad::pressed(pad::Button::right) || pad::pressed(pad::Button::cross)) &&
                 g_view.have_data && g_view.data_for == current_page() && !g_view.data.rows.empty())
        {
            g_view.content_focus = true;
            first_focusable();
        }
        else if (pad::pressed(pad::Button::triangle))
            load_page(current_page());
        if (wanted != g_view.page)
        {
            g_view.page = wanted;
            g_view.have_data = false;
            g_view.data = PageData{};
            notify({}, false);
            load_page(current_page());
        }
        return true;
    }

    auto &rows = g_view.data.rows;
    if (rows.empty())
    {
        g_view.content_focus = false;
        return true;
    }
    Row &row = rows[static_cast<std::size_t>(
        std::clamp(g_view.row, 0, static_cast<int>(rows.size()) - 1))];
    const std::vector<Field> &fields = fields_for(g_view.data_for);
    const Field *field = row.kind == Row::Kind::field && row.field >= 0 &&
                                 row.field < static_cast<int>(fields.size())
                             ? &fields[static_cast<std::size_t>(row.field)]
                             : nullptr;
    json::Value *value =
        field != nullptr ? field_value(g_view.data.config.get(), field->key) : nullptr;

    if (pad::pressed(pad::Button::circle))
        g_view.content_focus = false;
    else if (pad::pressed(pad::Button::down))
        move_row(1);
    else if (pad::pressed(pad::Button::up))
        move_row(-1);
    else if (pad::pressed(pad::Button::triangle))
        load_page(current_page());
    else if (field != nullptr &&
             (field->type == FieldType::boolean || field->type == FieldType::choice) &&
             (pad::pressed(pad::Button::left) || pad::pressed(pad::Button::right) ||
              pad::pressed(pad::Button::cross)))
        step_field(*field, value, pad::pressed(pad::Button::left) ? -1 : 1);
    else if (pad::pressed(pad::Button::left))
        g_view.content_focus = false;
    else if (pad::pressed(pad::Button::cross))
    {
        if (field != nullptr && value != nullptr)
        {
            g_view.prompt = View::Prompt::field;
            g_view.prompt_field = row.field;
            ime::request(field_text(*field, value) == "Not set" ? std::string{}
                                                                : field_text(*field, value),
                         field->label, field->hint, ime::Mode::text);
        }
        else if (!row.actions.empty())
        {
            g_view.action_index = 0;
            g_view.mode = Mode::actions;
        }
    }
    return true;
}

void draw() noexcept
{
    constexpr int kNavX = kSafeX;
    constexpr int kNavTop = 210;
    constexpr int kNavRow = 66;
    constexpr int kX = 520;
    const int right = kContentRight;
    constexpr int kTop = 210;
    const int bottom = gfx::kHeight - 120;

    std::string message;
    bool message_is_error = false;
    bool loading = false;
    {
        std::lock_guard<std::mutex> guard(g_shared.mutex);
        message = g_shared.message;
        message_is_error = g_shared.message_is_error;
        loading = g_shared.loading;
    }

    text::draw(kNavX, kSafeY + 50, "Dashboard", 38, Weight::bold, gfx::palette::text);

    if (!g_view.content_focus && g_view.mode == Mode::browse)
        focus_pill(kNavX - 24, kNavTop + g_view.page * kNavRow, 380, 56, FocusGroup::dashboard_nav);
    for (int i = 0; i < kPageCount; ++i)
    {
        const int y = kNavTop + i * kNavRow;
        const bool current = i == g_view.page;
        control_label(kNavX, y, 330, 56, kPages[i].label, 28, Weight::medium,
                      current ? gfx::palette::text : gfx::palette::text_dim);
    }

    if (g_view.mode == Mode::log)
    {
        std::vector<std::string> lines;
        std::string name;
        bool ready = false;
        {
            std::lock_guard<std::mutex> guard(g_shared.mutex);
            lines = g_shared.log_lines;
            name = g_shared.log_name;
            ready = g_shared.log_ready;
        }
        text::draw_ellipsized(kX, kSafeY + 60, right - kX, name, 28, Weight::medium,
                              gfx::palette::text);
        if (!ready)
            text::draw(kX, kTop, "Reading the log", 26, Weight::regular, gfx::palette::text_dim);
        constexpr int kLine = 26;
        const int visible = (bottom - kTop) / kLine;
        const int last_start = std::max(0, static_cast<int>(lines.size()) - visible);
        if (g_view.log_scroll < 0 && ready)
            g_view.log_scroll = last_start;
        g_view.log_scroll = std::clamp(g_view.log_scroll, 0, last_start);
        for (int i = 0; i < visible && g_view.log_scroll + i < static_cast<int>(lines.size()); ++i)
        {
            const std::string &line = lines[static_cast<std::size_t>(g_view.log_scroll + i)];
            const bool error =
                line.find("[ERR]") != std::string::npos || line.find("[FTL]") != std::string::npos;
            const bool warning = line.find("[WRN]") != std::string::npos;
            text::draw_ellipsized(kX, kTop + i * kLine, right - kX, line, 18, Weight::regular,
                                  error     ? gfx::palette::danger
                                  : warning ? gfx::rgb(0xe0, 0xb0, 0x40)
                                            : gfx::palette::text_dim);
        }
        (void)draw_button_hints(
            kX, gfx::kHeight - 70,
            {{icons::Icon::ps_circle, "Back"}, {icons::Icon::r2, "Page", "L1 R1"}});
        return;
    }

    const auto &rows = g_view.data.rows;
    const bool showing = g_view.have_data && g_view.data_for == current_page();
    if (!showing || rows.empty())
    {
        const std::string words = !showing || loading          ? std::string{"Loading"}
                                  : !g_view.data.error.empty() ? g_view.data.error
                                                               : std::string{"Nothing here"};
        text::draw_wrapped(kX, kTop, right - kX, 3, 38, words, 28, Weight::regular,
                           g_view.data.error.empty() ? gfx::palette::text_dim
                                                     : gfx::palette::danger);
    }
    else
    {
        const auto height_of = [](const Row &row)
        {
            switch (row.kind)
            {
            case Row::Kind::header:
                return 64;
            case Row::Kind::note:
                return 58;
            case Row::Kind::field:
                return 88;
            case Row::Kind::item:
                return row.subtitle.find('\n') != std::string::npos ? 118 : 90;
            }
            return 90;
        };
        /* Keep the focused row in view. */
        int focus_top = 0;
        int focus_height = 0;
        int total = 0;
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            if (static_cast<int>(i) == g_view.row)
            {
                focus_top = total;
                focus_height = height_of(rows[i]);
            }
            total += height_of(rows[i]);
        }
        const int view_height = bottom - kTop;
        if (g_view.content_focus)
        {
            if (focus_top < g_view.scroll)
                g_view.scroll = focus_top;
            else if (focus_top + focus_height > g_view.scroll + view_height)
                g_view.scroll = focus_top + focus_height - view_height;
        }
        g_view.scroll = std::clamp(g_view.scroll, 0, std::max(0, total - view_height));

        const std::vector<Field> &fields = fields_for(g_view.data_for);
        gfx::push_clip(kX - 30, kTop - 8, right - kX + 60, view_height + 16);
        if (g_view.content_focus && focus_height > 0)
            focus_pill(kX - 24, kTop - g_view.scroll + focus_top + 4, right - kX + 48,
                       focus_height - 8, FocusGroup::dashboard_rows, g_view.page);
        int y = kTop - g_view.scroll;
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            const Row &row = rows[i];
            const int h = height_of(row);
            if (y + h < kTop - 8 || y > bottom + 8)
            {
                y += h;
                continue;
            }
            switch (row.kind)
            {
            case Row::Kind::header:
                text::draw(kX, y + 22, row.title, 26, Weight::bold, gfx::palette::accent_alt);
                break;
            case Row::Kind::note:
                text::draw_ellipsized(kX, y + 12, right - kX, row.title, 22, Weight::regular,
                                      gfx::palette::text_faint);
                break;
            case Row::Kind::item:
            case Row::Kind::field:
            {
                std::string value_text = row.right;
                if (row.kind == Row::Kind::field && row.field >= 0 &&
                    row.field < static_cast<int>(fields.size()))
                {
                    const Field &field = fields[static_cast<std::size_t>(row.field)];
                    const json::Value *value = field_value(g_view.data.config.get(), field.key);
                    value_text = field_text(field, value);
                    if (field.type == FieldType::boolean || field.type == FieldType::choice)
                        value_text = "\xe2\x80\xb9  " + value_text + "  \xe2\x80\xba";
                }
                const int value_w =
                    value_text.empty() ? 0 : text::measure(value_text, 22, Weight::medium);
                const bool single_line = row.subtitle.empty() && row.progress < 0.0f;
                if (!value_text.empty())
                    text::draw(right - value_w,
                               single_line
                                   ? text::centered_y(y + 4, h - 8, value_text, 22, Weight::medium)
                                   : y + 16,
                               value_text, 22, Weight::medium,
                               row.warning ? gfx::rgb(0xe0, 0xb0, 0x40) : gfx::palette::text);
                text::draw_ellipsized(
                    kX,
                    single_line ? text::centered_y(y + 4, h - 8, row.title, 26, Weight::medium)
                                : y + 12,
                    right - kX - value_w - 30, row.title, 26, Weight::medium, gfx::palette::text);
                std::string first = row.subtitle;
                std::string second;
                if (const std::size_t cut = first.find('\n'); cut != std::string::npos)
                {
                    second = first.substr(cut + 1);
                    first.resize(cut);
                }
                text::draw_ellipsized(kX, y + 48, right - kX, first, 20, Weight::regular,
                                      row.warning && row.kind == Row::Kind::item
                                          ? gfx::rgb(0xe0, 0xb0, 0x40)
                                          : gfx::palette::text_dim);
                if (!second.empty())
                    text::draw_ellipsized(kX, y + 76, right - kX, second, 19, Weight::regular,
                                          gfx::palette::text_faint);
                if (row.progress >= 0.0f)
                {
                    const int track = right - kX;
                    const int bar_y = y + h - 14;
                    gfx::rounded_rect(kX, bar_y, track, 5, 3, gfx::rgba(0xff, 0xff, 0xff, 0x30));
                    gfx::rounded_rect(kX, bar_y, static_cast<int>(track * row.progress), 5, 3,
                                      gfx::palette::accent);
                }
                break;
            }
            }
            y += h;
        }
        gfx::pop_clip();
    }

    if (!message.empty())
        text::draw_ellipsized(kX, gfx::kHeight - 108, right - kX, message, 22, Weight::medium,
                              message_is_error ? gfx::palette::danger : gfx::palette::success);
    if (g_view.content_focus)
        (void)draw_button_hints(kX, gfx::kHeight - 70,
                                {{icons::Icon::ps_cross, "Choose"},
                                 {icons::Icon::ps_triangle, "Refresh"},
                                 {icons::Icon::ps_circle, "Pages"}});
    else
        (void)draw_button_hints(kX, gfx::kHeight - 70,
                                {{icons::Icon::ps_cross, "Open"},
                                 {icons::Icon::ps_triangle, "Refresh"},
                                 {icons::Icon::ps_circle, "Leave"}});

    if (g_view.mode == Mode::actions && g_view.row < static_cast<int>(rows.size()))
    {
        const Row &row = rows[static_cast<std::size_t>(g_view.row)];
        constexpr int kW = 560;
        constexpr int kRow = 64;
        const int h = 110 + static_cast<int>(row.actions.size()) * kRow + 70;
        const int x = (gfx::kWidth - kW) / 2;
        const int top = (gfx::kHeight - h) / 2;
        gfx::fill_rect(0, 0, gfx::kWidth, gfx::kHeight, gfx::rgba(0x00, 0x00, 0x00, 0x88));
        panel(x, top, kW, h);
        text::draw_ellipsized(x + 32, top + 32, kW - 64, row.title, 28, Weight::bold,
                              gfx::palette::text);
        focus_pill(x + 16, top + 100 + g_view.action_index * kRow, kW - 32, kRow - 8,
                   FocusGroup::dashboard_actions, g_view.page);
        bool ruled = false;
        for (std::size_t i = 0; i < row.actions.size(); ++i)
        {
            const Action &entry = row.actions[i];
            const int ry = top + 100 + static_cast<int>(i) * kRow;
            /* One line, once, where the ordinary actions end and the ones that
               cannot be undone begin. */
            if (entry.destructive && !ruled)
            {
                ruled = true;
                gfx::fill_rect(x + 32, ry - 10, kW - 64, 1, gfx::rgba(0xff, 0xff, 0xff, 0x1c));
            }
            const bool focused = static_cast<int>(i) == g_view.action_index;
            control_label(x + 40, ry, kW - 80, kRow - 8, entry.label, 26, Weight::medium,
                          focused             ? gfx::palette::text
                          : entry.destructive ? gfx::rgb(0xd8, 0x8a, 0x8a)
                                              : gfx::palette::text_dim);
        }
        (void)draw_button_hints(
            x + 32, top + h - 50,
            {{icons::Icon::ps_cross, "Do it"}, {icons::Icon::ps_circle, "Cancel"}});
    }
    else if (g_view.mode == Mode::confirm)
    {
        constexpr int kW = 760;
        constexpr int kH = 300;
        const int x = (gfx::kWidth - kW) / 2;
        const int top = (gfx::kHeight - kH) / 2;
        gfx::fill_rect(0, 0, gfx::kWidth, gfx::kHeight, gfx::rgba(0x00, 0x00, 0x00, 0x99));
        panel(x, top, kW, kH);
        text::draw_wrapped(x + 40, top + 36, kW - 80, 3, 40, g_view.pending.confirm, 28,
                           Weight::medium, gfx::palette::text);
        const auto button = [&](int bx, const char *label, bool lit)
        {
            gfx::rounded_rect(bx, top + kH - 96, 200, 60, 30,
                              lit ? gfx::palette::focus_surface : gfx::palette::control);
            gfx::stroke_rounded_rect(bx, top + kH - 96, 200, 60, 30, lit ? 2 : 1,
                                     lit ? gfx::palette::focus_border
                                         : gfx::palette::control_border);
            text::draw(bx + (200 - text::measure(label, 26, Weight::medium)) / 2, top + kH - 80,
                       label, 26, Weight::medium, gfx::palette::text);
        };
        button(x + kW - 460, "Cancel", !g_view.confirm_yes);
        button(x + kW - 240, g_view.pending.label.c_str(), g_view.confirm_yes);
        if (g_view.pending.destructive)
        {
            /* The bar fills while the button is held, so the wait is visible
               rather than a press that seems to do nothing. */
            const int bx = x + kW - 240;
            const int by = top + kH - 30;
            gfx::rounded_rect(bx, by, 200, 6, 3, gfx::rgba(0xff, 0xff, 0xff, 0x22));
            const int filled = 200 * std::clamp(g_view.hold, 0, kHoldFrames) / kHoldFrames;
            if (filled > 0)
                gfx::rounded_rect(bx, by, filled, 6, 3, gfx::palette::danger);
            text::draw(x + 40, top + kH - 44,
                       g_view.confirm_yes ? "Hold Cross to go ahead" : "This one cannot be undone",
                       22, Weight::regular, gfx::palette::text_faint);
        }
    }
}

} // namespace slopfin::dashboard
