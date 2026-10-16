/*
 * SlopFin - Discover: a Seerr/Jellyseerr/Overseerr request client inside the
 * Jellyfin app.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "seerr.hpp"
#include "seerr_request.hpp"
#include "motion.hpp"

#include "background.hpp"
#include "config.hpp"
#include "gfx.hpp"
#include "http.hpp"
#include "icons.hpp"
#include "images.hpp"
#include "ime.hpp"
#include "json.hpp"
#include "pad.hpp"
#include "server_address.hpp"
#include "text.hpp"
#include "ui_common.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <unordered_set>
#include <span>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace slopfin::seerr
{
namespace
{

using text::Weight;

/* --------------------------------------------------------------- the model */
/* Written by background jobs, read by the render thread, under g_mutex. */

std::mutex g_mutex;

struct Connection
{
    std::string host;
    int port = 443;
    int auth = 0;
    std::string api_key;
    std::string cookie;
    unsigned long generation = 0;
};
Connection g_connection;

struct Model
{
    std::vector<Title> results;
    long search_epoch = 0;  /* bumped whenever the results change */
    long search_wanted = 0; /* the newest query asked for */
    bool search_loading = false;
    std::string search_error;

    std::vector<Request> requests;
    long requests_epoch = 0;
    bool requests_loading = false;
    std::string requests_error;

    std::string server_title;
    std::string server_version;
    bool reachable = false;
    bool authenticated = false;
    int user_id = 0;
    std::uint32_t permissions = 0;
    bool partial_requests = true;
    request::Options options;
    unsigned long options_wanted = 0;
    unsigned long options_epoch = 0;
    bool refreshing = false;
    bool submitting = false;
    bool signing_in = false;
    std::string connection_error;

    /* A short message for the viewer: a toast on Discover, a notice here. */
    std::string notice;
    long notice_epoch = 0;

    /* A session captured by a sign-in, applied to the config on the render
       thread so the file is only ever written from one place. */
    std::string pending_cookie;
    std::string pending_user;

    /* Titles seen once, so a request's name and poster can be filled in.
       Only the background thread touches this, always under the lock. */
    std::unordered_map<std::string, Title> known_titles;
    std::unordered_set<std::string> detail_pending;
};

Model g_model;

/* Download rates are estimated from one refresh to the next; worker only. */
struct RateSample
{
    long long remaining = 0;
    std::chrono::steady_clock::time_point at{};
};
std::unordered_map<int, RateSample> g_rate;

/* ---------------------------------------------------------------- the view */
/* Render thread only. */

struct View
{

    int mode = 0; /* 0 search, 1 requests */
    bool field_focused = true;
    std::string query;
    int grid_index = 0;
    int grid_scroll = 0;
    int request_index = 0;
    int request_scroll = 0;
    int filter = 0;
    int media_filter = 0;
    int availability_filter = 0;
    bool filter_focused = false;
    bool filters_open = false;
    int filter_row = 0;
    int draft_media = 0;
    int draft_status = 0;
    motion::Spring tab_position;
    std::uint32_t permissions = 0;
    request::Options options;
    unsigned long options_seen = 0;
    bool options_loading = false;
    bool is4k = false;
    int server_index = -1;
    int profile_index = -1;
    int sheet_row = 0;
    int season_scroll = 0;
    std::vector<int> seasons;
    std::string sheet_error;

    bool sheet_open = false;
    bool sheet_from_requests = false;
    Title sheet;
    int sheet_approval = 0;
    int sheet_request_id = 0;

    std::string toast;
    long toast_frame = -1000;
    std::string settings_notice;

    long frames = 0;
    int ime_guard = 0;
    std::chrono::steady_clock::time_point refresh_at{};

    long seen_search_epoch = -1;
    long seen_requests_epoch = -1;
    long seen_notice_epoch = -1;
    std::vector<Title> results;
    std::vector<Request> requests;
    bool search_loading = false;
    std::string search_error;
    bool requests_loading = false;
    std::string requests_error;
    std::string server_title;
    std::string server_version;
    bool reachable = false;
    bool authenticated = false;
    bool refreshing = false;
    bool submitting = false;
    bool signing_in = false;
    std::string connection_error;

    /* Settings page. */
    int settings_row = 0;
    std::string account;
    std::string password;
};

View g_view;

/* Which text field the console keyboard was opened for. */
enum class Field : unsigned char
{
    none = 0,
    query,
    host,
    api_key,
    account,
    password,
};

Field g_field = Field::none;
bool g_wants_settings = false;
std::string g_open_item;

/* ------------------------------------------------------------- small parts */

constexpr int kHeaderY = ui::kSafeY + 66; /* the screen title's top line */
constexpr int kFieldY = ui::kSafeY + 164;
constexpr int kGridY = ui::kSafeY + 284;
constexpr int kListY = kGridY;

constexpr int kCardW = 186;
constexpr int kCardH = 278;
constexpr int kCardStepX = 236;
constexpr int kCardStepY = 352;

std::string format(const char *pattern, double value) noexcept
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), pattern, value);
    return buffer;
}

std::string human_bytes(long long bytes) noexcept
{
    const double value = static_cast<double>(bytes);
    if (value >= 1024.0 * 1024.0 * 1024.0)
        return format("%.1f GB", value / (1024.0 * 1024.0 * 1024.0));
    if (value >= 1024.0 * 1024.0)
        return format("%.0f MB", value / (1024.0 * 1024.0));
    return format("%.0f KB", value / 1024.0);
}

const char *availability_label(Availability value) noexcept
{
    switch (value)
    {
    case Availability::pending:
        return "Requested";
    case Availability::processing:
        return "Processing";
    case Availability::partial:
        return "Partly here";
    case Availability::available:
        return "Available";
    case Availability::unknown:
        break;
    }
    return "Not requested";
}

gfx::Color availability_colour(Availability value) noexcept
{
    switch (value)
    {
    case Availability::pending:
    case Availability::partial:
        return gfx::rgb(0xf5, 0xb0, 0x42);
    case Availability::processing:
        return gfx::palette::accent_alt;
    case Availability::available:
        return gfx::palette::success;
    case Availability::unknown:
        break;
    }
    return gfx::palette::text_faint;
}

const char *approval_label(int approval) noexcept
{
    switch (approval)
    {
    case 1:
        return "Awaiting approval";
    case 2:
        return "Approved";
    case 3:
        return "Declined";
    default:
        return "";
    }
}

int grid_columns() noexcept
{
    return std::max(1, (ui::kContentWidth + 26) / (kCardW + 26));
}

std::string key_for(const std::string &type, int id) noexcept
{
    return type + ":" + std::to_string(id);
}

/* ------------------------------------------------------------------ helpers */

bool current(const Connection &connection) noexcept
{
    return connection.generation == g_connection.generation;
}

void sync_connection() noexcept
{
    const auto &settings = config::current();
    const std::string key = settings.seerr_auth == 2 ? settings.seerr_api_key : std::string{};
    const std::string cookie = settings.seerr_auth == 2 ? std::string{} : settings.seerr_cookie;
    std::lock_guard<std::mutex> guard(g_mutex);
    if (settings.seerr_host == g_connection.host && settings.seerr_port == g_connection.port &&
        key == g_connection.api_key && cookie == g_connection.cookie &&
        settings.seerr_auth == g_connection.auth)
        return;
    g_connection = {
        settings.seerr_host,        settings.seerr_port, settings.seerr_auth, key, cookie,
        g_connection.generation + 1};
    g_model.results.clear();
    g_model.requests.clear();
    g_model.known_titles.clear();
    g_model.detail_pending.clear();
    ++g_model.search_wanted;
    ++g_model.search_epoch;
    ++g_model.requests_epoch;
    g_model.search_loading = g_model.requests_loading = g_model.refreshing = false;
    g_model.submitting = g_model.signing_in = false;
    g_model.authenticated = g_model.reachable = false;
    g_model.user_id = 0;
    g_model.permissions = 0;
    g_model.options = {};
    ++g_model.options_wanted;
    ++g_model.options_epoch;
    g_model.pending_cookie.clear();
    g_model.pending_user.clear();
    g_model.search_error.clear();
    g_model.requests_error.clear();
    g_model.connection_error.clear();
}

Connection connection_snapshot() noexcept
{
    sync_connection();
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_connection;
}

std::vector<std::string> auth_headers(const Connection &connection) noexcept
{
    if (!connection.api_key.empty())
        return {"X-Api-Key: " + connection.api_key};
    if (!connection.cookie.empty())
        return {"Cookie: connect.sid=" + connection.cookie};
    return {};
}

http::Response api_get(const Connection &connection, const std::string &path) noexcept
{
    return http::get(connection.host, connection.port, path, auth_headers(connection));
}

http::Response api_post(const Connection &connection, const std::string &path,
                        const std::string &body) noexcept
{
    return http::post(connection.host, connection.port, path, auth_headers(connection), body,
                      "application/json");
}

std::string response_error(const http::Response &response, const char *action) noexcept
{
    if (response.status == 401)
        return "Your request-server session has expired. Sign in again in Settings > Requests.";
    if (response.status == 0)
        return "The request server could not be reached. Check its address and connection.";
    if (const auto doc = json::parse(response.body))
    {
        auto message = doc->str("message");
        if (message.empty())
            message = doc->str("error");
        if (!message.empty())
            return std::string(message.substr(0, 180));
    }
    if (response.status == 403)
        return "Your request-server account does not have permission for this action.";
    return std::string(action) + " (HTTP " + std::to_string(response.status) + ").";
}

/* The connect.sid value a sign-in sets, or empty. */
std::string session_cookie(const http::Response &response) noexcept
{
    constexpr const char *kName = "connect.sid=";
    constexpr std::size_t kNameLength = 12;
    for (const std::string &line : response.headers)
    {
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name != "set-cookie")
            continue;
        std::string value = line.substr(colon + 1);
        const auto begin = value.find_first_not_of(" \t");
        if (begin == std::string::npos)
            continue;
        value.erase(0, begin);
        if (value.rfind(kName, 0) != 0)
            continue;
        const auto end = value.find(';');
        return value.substr(kNameLength,
                            (end == std::string::npos ? value.size() : end) - kNameLength);
    }
    return {};
}

int year_of(const std::string &date) noexcept
{
    if (date.size() < 4)
        return 0;
    int year = 0;
    for (int i = 0; i < 4; ++i)
    {
        if (date[static_cast<std::size_t>(i)] < '0' || date[static_cast<std::size_t>(i)] > '9')
            return 0;
        year = year * 10 + (date[static_cast<std::size_t>(i)] - '0');
    }
    return year;
}

std::string jellyfin_id(const json::Value &node, const char *key = "jellyfinMediaId") noexcept
{
    const auto id = node.str(key);
    if (id.size() != 32 && id.size() != 36)
        return {};
    std::size_t digits = 0;
    for (const auto c : id)
    {
        if (std::isxdigit(static_cast<unsigned char>(c)))
            ++digits;
        else if (c != '-')
            return {};
    }
    return digits == 32 ? std::string(id) : std::string{};
}

Availability availability_of(double status) noexcept
{
    switch (static_cast<int>(status))
    {
    case 2:
        return Availability::pending;
    case 3:
        return Availability::processing;
    case 4:
        return Availability::partial;
    case 5:
        return Availability::available;
    default:
        return Availability::unknown;
    }
}

Title read_title(const json::Value &node) noexcept
{
    Title title;
    title.tmdb_id = static_cast<int>(node.num("id", 0));
    title.media_type = std::string{node.str("mediaType")};
    if (title.media_type.empty())
        title.media_type = "movie";
    title.name = std::string{node.str("title")};
    if (title.name.empty())
        title.name = std::string{node.str("name")};
    title.overview = std::string{node.str("overview")};
    title.poster_path = std::string{node.str("posterPath")};
    title.rating = node.num("voteAverage", 0.0);
    std::string date{node.str("releaseDate")};
    if (date.empty())
        date = std::string{node.str("firstAirDate")};
    title.year = year_of(date);
    if (const json::Value *info = node.find("mediaInfo"); info != nullptr)
    {
        const int status = static_cast<int>(info->num("status", 0));
        title.availability = availability_of(status);
        title.requested = status >= 2 && status <= 5;
        title.jellyfin_id = jellyfin_id(*info);
    }
    return title;
}

Request read_request(const json::Value &node) noexcept
{
    Request request;
    request.id = static_cast<int>(node.num("id", 0));
    request.approval = static_cast<int>(node.num("status", 0));
    request.profile_name = std::string(node.str("profileName"));
    if (const auto *seasons = node.find("seasons"))
        for (std::size_t i = 0; i < std::min<std::size_t>(seasons->size(), 500); ++i)
            if (const auto *season = seasons->at(i); season && season->num("seasonNumber", -1) >= 0)
                request.seasons.push_back(static_cast<int>(season->num("seasonNumber")));
    if (const json::Value *by = node.find("requestedBy"); by != nullptr)
        request.requested_by = std::string{by->str("displayName")};
    const json::Value *media = node.find("media");
    if (media != nullptr)
    {
        request.tmdb_id = static_cast<int>(media->num("tmdbId", 0));
        request.media_type = std::string{media->str("mediaType")};
        const bool is4k = node.flag("is4k");
        request.is4k = is4k;
        request.availability = availability_of(media->num(is4k ? "status4k" : "status", 0));
        request.jellyfin_id = jellyfin_id(*media, is4k ? "jellyfinMediaId4k" : "jellyfinMediaId");
        if (const json::Value *downloads =
                media->find(is4k ? "downloadStatus4k" : "downloadStatus");
            downloads != nullptr && downloads->size() > 0)
        {
            long long size = 0;
            long long left = 0;
            for (std::size_t i = 0; i < downloads->size(); ++i)
            {
                const json::Value *entry = downloads->at(i);
                if (entry == nullptr)
                    continue;
                size += static_cast<long long>(entry->num("size", 0.0));
                left += static_cast<long long>(entry->num("sizeLeft", 0.0));
                const std::string time{entry->str("timeLeft")};
                if (!time.empty())
                    request.eta = time;
            }
            request.size_bytes = size;
            request.remaining_bytes = left;
            request.downloading = size > 0 && left > 0 && left <= size;
            if (size > 0)
                request.progress = std::clamp(
                    1.0 - static_cast<double>(left) / static_cast<double>(size), 0.0, 1.0);
        }
    }
    if (request.media_type.empty())
        request.media_type = "movie";
    return request;
}

/* Call with g_mutex held. */
void remember(const Title &title) noexcept
{
    if (title.tmdb_id != 0 && !title.media_type.empty())
    {
        if (g_model.known_titles.size() >= 256)
            g_model.known_titles.clear();
        g_model.known_titles[key_for(title.media_type, title.tmdb_id)] = title;
    }
}

/* ------------------------------------------------------------------- jobs */

void job_refresh_server(Connection connection) noexcept
{
    std::string title, version, error;
    bool reachable = false;
    int user_id = 0;
    std::uint32_t permissions = 0;
    bool partial = true;
    if (connection.host.empty())
        error = "Enter your request-server address in Settings > Requests.";
    else
    {
        const auto status = http::get(connection.host, connection.port, "/api/v1/status", {});
        reachable = status.ok();
        if (const auto doc = json::parse(status.body); status.ok() && doc)
            version = std::string(doc->str("version"));
        if (!reachable)
            error = response_error(status, "Connection failed");
        else
        {
            const auto info =
                http::get(connection.host, connection.port, "/api/v1/settings/public", {});
            if (const auto doc = json::parse(info.body); info.ok() && doc)
            {
                title = std::string(doc->str("applicationTitle"));
                partial = doc->flag("partialRequestsEnabled", true);
            }
            const auto me = api_get(connection, "/api/v1/auth/me");
            if (const auto doc = json::parse(me.body); me.ok() && doc)
            {
                user_id = static_cast<int>(doc->num("id", 0));
                permissions = static_cast<std::uint32_t>(doc->num("permissions", 0));
            }
            if (user_id <= 0)
                error = me.ok() ? "The request server returned an invalid account."
                                : response_error(me, "Account check failed");
        }
    }
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!current(connection))
        return;
    g_model.refreshing = false;
    g_model.reachable = reachable;
    g_model.authenticated = user_id > 0;
    g_model.user_id = user_id;
    g_model.permissions = permissions;
    g_model.partial_requests = partial;
    g_model.server_title = title;
    g_model.server_version = version;
    g_model.connection_error = error;
}

void job_search(Connection connection, std::string term, long generation) noexcept
{
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (!current(connection) || generation != g_model.search_wanted)
            return;
    }
    const auto path = term.empty() ? "/api/v1/discover/trending?page=1"
                                   : "/api/v1/search?query=" + http::url_encode(term) + "&page=1";
    const auto response = connection.host.empty() ? http::Response{} : api_get(connection, path);
    std::vector<Title> results;
    std::string error;
    if (connection.host.empty())
        error = "Set up your request server in Settings > Requests.";
    else if (!response.ok())
        error = response_error(response, "Search failed");
    else if (const auto doc = json::parse(response.body); doc && doc->find("results"))
    {
        const auto *list = doc->find("results");
        for (std::size_t i = 0; i < std::min<std::size_t>(list->size(), 100); ++i)
            if (const auto *node = list->at(i))
            {
                const auto type = node->str("mediaType");
                if (type == "movie" || type == "tv")
                    results.push_back(read_title(*node));
            }
    }
    else
        error = "The request server returned an invalid search result.";
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!current(connection) || generation != g_model.search_wanted)
        return;
    for (const auto &title : results)
        remember(title);
    g_model.results = std::move(results);
    g_model.search_loading = false;
    g_model.search_error = error;
    ++g_model.search_epoch;
}

void job_detail(Connection connection, const std::string &type, int id) noexcept
{
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (!current(connection))
            return;
    }
    const auto response = api_get(connection, "/api/v1/" + type + "/" + std::to_string(id));
    const auto doc = json::parse(response.body);
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!current(connection))
        return;
    g_model.detail_pending.erase(key_for(type, id));
    if (!response.ok() || !doc)
        return;
    Title title = read_title(*doc);
    title.media_type = type;
    title.tmdb_id = id;
    remember(title);
    for (auto &request : g_model.requests)
        if (request.tmdb_id == id && request.media_type == type)
        {
            request.name = title.name;
            request.poster_path = title.poster_path;
        }
    ++g_model.requests_epoch;
}

bool current_options(const Connection &connection, unsigned long token) noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return current(connection) && token == g_model.options_wanted;
}

void job_options(Connection connection, Title title, unsigned long token, std::uint32_t permissions,
                 bool partial) noexcept
{
    if (!current_options(connection, token))
        return;
    request::Options options;
    options.advanced = request::advanced(permissions);
    options.partial = partial;
    const auto response =
        api_get(connection, "/api/v1/" + title.media_type + "/" + std::to_string(title.tmdb_id));
    const auto doc = json::parse(response.body);
    if (!response.ok() || !doc)
        options.error = response.ok() ? "The server returned invalid title details."
                                      : response_error(response, "Title details failed");
    else
    {
        options.seasons = request::read_seasons(*doc);
        if (const auto *info = doc->find("mediaInfo"))
        {
            options.status = static_cast<int>(info->num("status", 1));
            options.status4k = static_cast<int>(info->num("status4k", 1));
            options.jellyfin_id = jellyfin_id(*info);
            options.jellyfin_id4k = jellyfin_id(*info, "jellyfinMediaId4k");
        }
        const auto service =
            std::string("/api/v1/service/") + (title.media_type == "tv" ? "sonarr" : "radarr");
        const auto servers = api_get(connection, service);
        const auto list = json::parse(servers.body);
        if (servers.ok() && list && list->type == json::Type::array)
        {
            for (std::size_t i = 0; i < std::min<std::size_t>(list->size(), 8); ++i)
            {
                if (!current_options(connection, token))
                    return;
                const auto *entry = list->at(i);
                if (!entry)
                    continue;
                request::Server server;
                server.id = static_cast<int>(entry->num("id", -1));
                if (server.id < 0)
                    continue;
                server.name = std::string(entry->str("name"));
                server.is4k = entry->flag("is4k");
                server.is_default = entry->flag("isDefault");
                server.default_profile = static_cast<int>(entry->num("activeProfileId"));
                server.directory = std::string(entry->str("activeDirectory"));
                if (options.advanced)
                {
                    const auto profiles =
                        api_get(connection, service + "/" + std::to_string(server.id));
                    if (const auto detail = json::parse(profiles.body); profiles.ok() && detail)
                    {
                        if (const auto *values = detail->find("profiles"))
                            for (std::size_t n = 0; n < std::min<std::size_t>(values->size(), 100);
                                 ++n)
                                if (const auto *profile = values->at(n);
                                    profile && profile->num("id") > 0)
                                    server.profiles.push_back({static_cast<int>(profile->num("id")),
                                                               std::string(profile->str("name"))});
                    }
                    else
                        options.profile_error =
                            "Quality profiles are unavailable. The server default will be used.";
                }
                options.servers.push_back(std::move(server));
            }
        }
        else
            options.profile_error =
                "Download servers could not be loaded. The server default will be used.";
        options.allow4k = request::can_request(permissions, title.media_type == "tv", true) &&
                          std::any_of(options.servers.begin(), options.servers.end(),
                                      [](const auto &server) { return server.is4k; });
    }
    options.loaded = true;
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!current(connection) || token != g_model.options_wanted)
        return;
    g_model.options = std::move(options);
    ++g_model.options_epoch;
}

void job_requests(Connection connection) noexcept
{
    int user_id = 0;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (!current(connection))
            return;
        user_id = g_model.user_id;
    }
    if (user_id <= 0)
    {
        job_refresh_server(connection);
        std::lock_guard<std::mutex> guard(g_mutex);
        if (!current(connection))
            return;
        user_id = g_model.user_id;
    }
    const auto response =
        user_id > 0 ? api_get(connection, "/api/v1/request?take=60&skip=0&sort=added&requestedBy=" +
                                              std::to_string(user_id))
                    : http::Response{};
    std::vector<Request> requests;
    std::string error;
    if (user_id <= 0)
        error = "Connect your account in Settings > Requests to see your requests.";
    else if (!response.ok())
        error = response_error(response, "Requests could not be loaded");
    else if (const auto doc = json::parse(response.body); doc && doc->find("results"))
    {
        const auto *list = doc->find("results");
        for (std::size_t i = 0; i < std::min<std::size_t>(list->size(), 60); ++i)
            if (const auto *node = list->at(i))
                requests.push_back(read_request(*node));
    }
    else
        error = "The request server returned an invalid requests list.";

    const auto now = std::chrono::steady_clock::now();
    static unsigned long rate_generation = 0;
    if (rate_generation != connection.generation)
    {
        g_rate.clear();
        rate_generation = connection.generation;
    }
    std::unordered_map<int, RateSample> samples;
    for (auto &request : requests)
        if (request.downloading)
        {
            const auto sample = g_rate.find(request.id);
            if (sample != g_rate.end() && sample->second.remaining > request.remaining_bytes)
            {
                const auto seconds = std::chrono::duration<double>(now - sample->second.at).count();
                if (seconds > 0.5)
                    request.rate_bytes = static_cast<long long>(
                        (sample->second.remaining - request.remaining_bytes) / seconds);
            }
            samples[request.id] = {request.remaining_bytes, now};
        }
    g_rate = std::move(samples);
    std::vector<std::pair<std::string, int>> wanted;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (!current(connection))
            return;
        for (auto &request : requests)
        {
            const auto key = key_for(request.media_type, request.tmdb_id);
            if (const auto found = g_model.known_titles.find(key);
                found != g_model.known_titles.end())
            {
                request.name = found->second.name;
                request.poster_path = found->second.poster_path;
            }
            else if (wanted.size() < 4 && request.tmdb_id > 0 &&
                     (request.media_type == "movie" || request.media_type == "tv") &&
                     g_model.detail_pending.insert(key).second)
                wanted.emplace_back(request.media_type, request.tmdb_id);
        }
        if (error.empty())
        {
            for (auto &title : g_model.results)
                for (const auto &request : requests)
                    if (title.tmdb_id == request.tmdb_id && title.media_type == request.media_type)
                    {
                        title.availability = request.availability;
                        if (!request.jellyfin_id.empty())
                            title.jellyfin_id = request.jellyfin_id;
                    }
            ++g_model.search_epoch;
            g_model.requests = std::move(requests);
        }
        if (response.status == 401 || response.status == 403)
        {
            g_model.authenticated = false;
            g_model.user_id = 0;
        }
        g_model.requests_loading = false;
        g_model.requests_error = error;
        ++g_model.requests_epoch;
    }
    for (const auto &[type, id] : wanted)
        background::run([connection, type, id] { job_detail(connection, type, id); },
                        background::Lane::requests);
}

void job_request(Connection connection, Title title, std::string body) noexcept
{
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (!current(connection))
            return;
    }
    const auto response = api_post(connection, "/api/v1/request", body);
    std::string notice = response.ok()            ? "Requested " + title.name
                         : response.status == 409 ? title.name + " has already been requested"
                                                  : response_error(response, "Request failed");
    if (const auto doc = json::parse(response.body);
        response.ok() && doc && doc->num("status") == 1)
        notice += " — waiting for approval";
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (!current(connection))
            return;
        g_model.submitting = false;
        if (response.ok() || response.status == 409)
        {
            for (auto &result : g_model.results)
                if (result.tmdb_id == title.tmdb_id && result.media_type == title.media_type)
                {
                    result.requested = true;
                    if (result.availability == Availability::unknown)
                        result.availability = Availability::pending;
                }
            ++g_model.search_epoch;
        }
        g_model.notice = notice;
        ++g_model.notice_epoch;
    }
    if (response.ok())
        job_requests(connection);
}

void job_sign_in(Connection connection, bool jellyfin, std::string account,
                 std::string password) noexcept
{
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (!current(connection))
            return;
    }
    const std::string body = std::string(jellyfin ? "{\"username\":\"" : "{\"email\":\"") +
                             json::escape(account) + "\",\"password\":\"" + json::escape(password) +
                             "\"}";
    const auto response = http::post(connection.host, connection.port,
                                     jellyfin ? "/api/v1/auth/jellyfin" : "/api/v1/auth/local", {},
                                     body, "application/json");
    std::fill(password.begin(), password.end(), '\0');
    const auto cookie = response.ok() ? session_cookie(response) : std::string{};
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!current(connection))
        return;
    g_model.signing_in = false;
    if (!cookie.empty())
    {
        g_model.pending_cookie = cookie;
        g_model.pending_user = account;
    }
    g_model.notice =
        response.status == 401
            ? "Those sign-in details were not accepted. Check your account and password."
        : !response.ok() ? response_error(response, "Sign-in failed")
        : cookie.empty() ? "The server did not return a session. Check its proxy/cookie settings."
                         : "Signed in as " + account + ".";
    ++g_model.notice_epoch;
}

/* ------------------------------------------------------------- the screen */

void refresh_server() noexcept
{
    const auto connection = connection_snapshot();
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (g_model.refreshing)
            return;
        g_model.refreshing = true;
    }
    if (!background::run([connection] { job_refresh_server(connection); },
                         background::Lane::requests))
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_model.refreshing = false;
        g_model.connection_error = "The request worker could not start. Restart SlopFin.";
    }
}

void refresh_requests() noexcept
{
    const auto connection = connection_snapshot();
    g_view.refresh_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (g_model.requests_loading || connection.host.empty())
            return;
        g_model.requests_loading = true;
    }
    if (!background::run([connection] { job_requests(connection); }, background::Lane::requests))
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_model.requests_loading = false;
        g_model.requests_error = "The request worker could not start. Restart SlopFin.";
    }
}

void start_search(const std::string &term) noexcept
{
    const auto connection = connection_snapshot();
    long generation;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        generation = ++g_model.search_wanted;
        g_model.search_loading = !connection.host.empty();
        g_model.search_error.clear();
        g_model.results.clear();
        ++g_model.search_epoch;
    }
    if (!background::run([connection, term, generation]
                         { job_search(connection, term, generation); }, background::Lane::requests))
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_model.search_loading = false;
        g_model.search_error = "The request worker could not start. Restart SlopFin.";
    }
}

std::vector<int> visible_requests() noexcept;

void choose_default_server() noexcept;
void select_missing() noexcept;

void snapshot() noexcept
{
    sync_connection();
    std::string cookie, user;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (g_model.search_epoch != g_view.seen_search_epoch)
        {
            g_view.seen_search_epoch = g_model.search_epoch;
            g_view.results = g_model.results;
        }
        if (g_model.requests_epoch != g_view.seen_requests_epoch)
        {
            const auto previous = visible_requests();
            const int selected =
                g_view.request_index >= 0 &&
                        g_view.request_index < static_cast<int>(previous.size())
                    ? g_view
                          .requests[static_cast<std::size_t>(
                              previous[static_cast<std::size_t>(g_view.request_index)])]
                          .id
                    : 0;
            g_view.seen_requests_epoch = g_model.requests_epoch;
            g_view.requests = g_model.requests;
            const auto visible = visible_requests();
            for (std::size_t i = 0; i < visible.size(); ++i)
                if (g_view.requests[static_cast<std::size_t>(visible[i])].id == selected)
                    g_view.request_index = static_cast<int>(i);
            if (g_view.sheet_open && g_view.sheet_from_requests)
                for (const auto &request : g_view.requests)
                    if (request.id == g_view.sheet_request_id)
                    {
                        if (const auto known = g_model.known_titles.find(
                                key_for(request.media_type, request.tmdb_id));
                            known != g_model.known_titles.end())
                            g_view.sheet = known->second;
                        g_view.sheet.name = request.name;
                        g_view.sheet.poster_path = request.poster_path;
                        g_view.sheet.availability = request.availability;
                        if (!request.jellyfin_id.empty())
                            g_view.sheet.jellyfin_id = request.jellyfin_id;
                        g_view.sheet_approval = request.approval;
                    }
        }
        if (g_model.notice_epoch != g_view.seen_notice_epoch)
        {
            g_view.seen_notice_epoch = g_model.notice_epoch;
            g_view.toast = g_model.notice;
            g_view.toast_frame = g_view.frames;
            g_view.settings_notice = g_model.notice;
        }
        g_view.permissions = g_model.permissions;
        if (g_view.options_seen != g_model.options_epoch)
        {
            g_view.options_seen = g_model.options_epoch;
            g_view.options = g_model.options;
            g_view.options_loading = false;
            choose_default_server();
            select_missing();
            if (g_view.options.loaded &&
                (g_view.is4k ? g_view.options.status4k : g_view.options.status) == 5 &&
                !(g_view.is4k ? g_view.options.jellyfin_id4k : g_view.options.jellyfin_id).empty())
                g_view.sheet_row = 10000;
        }
        g_view.search_loading = g_model.search_loading;
        g_view.search_error = g_model.search_error;
        g_view.requests_loading = g_model.requests_loading;
        g_view.requests_error = g_model.requests_error;
        g_view.server_title = g_model.server_title;
        g_view.server_version = g_model.server_version;
        g_view.reachable = g_model.reachable;
        g_view.authenticated = g_model.authenticated;
        g_view.refreshing = g_model.refreshing;
        g_view.submitting = g_model.submitting;
        g_view.signing_in = g_model.signing_in;
        g_view.connection_error = g_model.connection_error;
        cookie.swap(g_model.pending_cookie);
        user.swap(g_model.pending_user);
    }
    if (!cookie.empty())
    {
        auto &settings = config::current();
        settings.seerr_cookie = std::move(cookie);
        settings.seerr_user = std::move(user);
        settings.seerr_api_key.clear();
        config::save();
        refresh_server();
    }
}

bool media_matches(const std::string &type) noexcept
{
    return g_view.media_filter == 0 || type == (g_view.media_filter == 1 ? "movie" : "tv");
}
std::vector<int> visible_results() noexcept
{
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(g_view.results.size()); ++i)
    {
        const auto &title = g_view.results[static_cast<std::size_t>(i)];
        if (!media_matches(title.media_type))
            continue;
        const bool shown =
            g_view.availability_filter == 0 ||
            (g_view.availability_filter == 1 && title.availability == Availability::unknown) ||
            (g_view.availability_filter == 2 && title.requested) ||
            (g_view.availability_filter == 3 && title.availability == Availability::available);
        if (shown)
            out.push_back(i);
    }
    return out;
}
int results_size() noexcept
{
    return static_cast<int>(visible_results().size());
}
std::vector<int> visible_requests() noexcept
{
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(g_view.requests.size()); ++i)
    {
        const auto &r = g_view.requests[static_cast<std::size_t>(i)];
        if (!media_matches(r.media_type))
            continue;
        const bool shown = g_view.filter == 0 || (g_view.filter == 1 && r.downloading) ||
                           (g_view.filter == 2 && r.availability == Availability::available) ||
                           (g_view.filter == 3 && r.approval == 1) ||
                           (g_view.filter == 4 && r.availability == Availability::processing) ||
                           (g_view.filter == 5 && r.approval == 3);
        if (shown)
            out.push_back(i);
    }
    return out;
}

bool can_request(const Title &title, bool from_requests, bool is4k = false) noexcept
{
    if (!g_view.authenticated || g_view.submitting)
        return false;
    if (title.availability == Availability::unknown ||
        (title.availability == Availability::partial && title.media_type == "tv"))
        return title.tmdb_id != 0 &&
               request::can_request(g_view.permissions, title.media_type == "tv", is4k);
    if (from_requests && g_view.sheet_approval == 3)
        return true; /* a declined request can be asked for again */
    return false;
}

const char *request_action_label(const Title &title, bool from_requests) noexcept
{
    if (title.availability == Availability::available && !title.jellyfin_id.empty())
        return "Open in Jellyfin";
    if (g_view.submitting)
        return "Sending request…";
    if (!g_view.authenticated)
        return "Sign in under Settings > Requests";
    if (can_request(title, from_requests))
        return title.media_type == "tv" ? "Choose seasons" : "Request movie";
    if (title.availability == Availability::available)
        return title.jellyfin_id.empty() ? "Available on request server" : "Open in Jellyfin";
    if (from_requests && g_view.sheet_approval != 0)
        return approval_label(g_view.sheet_approval);
    return "Requested";
}

void open_keyboard(Field field) noexcept
{
    if (g_field != Field::none || ime::busy())
        return;
    std::string initial;
    std::string title;
    std::string placeholder;
    ime::Mode mode = ime::Mode::text;
    switch (field)
    {
    case Field::query:
        initial = g_view.query;
        title = "Search the request server";
        placeholder = "Movie or series name";
        mode = ime::Mode::search;
        break;
    case Field::host:
        initial = config::current().seerr_host;
        title = "Request server address";
        placeholder = "request.example.com";
        break;
    case Field::api_key:
        initial = config::current().seerr_api_key;
        title = "Seerr API key";
        placeholder = "Settings > General in Seerr";
        break;
    case Field::account:
        initial = g_view.account;
        title = config::current().seerr_auth == 0 ? "Jellyfin username" : "E-mail address";
        placeholder = config::current().seerr_auth == 0 ? "your username" : "you@example.com";
        break;
    case Field::password:
        title = "Password";
        placeholder = "your password";
        mode = ime::Mode::password;
        break;
    case Field::none:
        return;
    }
    g_field = field;
    ime::request(initial, title, placeholder, mode, kFieldY + 64 + 16);
}

void apply_keyboard(const std::string &entered) noexcept
{
    const Field field = g_field;
    g_field = Field::none;
    switch (field)
    {
    case Field::query:
        g_view.query = entered;
        {
            g_view.mode = 0;
            g_view.field_focused = false;
            g_view.grid_index = 0;
            g_view.grid_scroll = 0;
            start_search(entered);
        }
        break;
    case Field::host:
    {
        const server_address::Address address = server_address::parse(entered, 5055);
        if (!address.valid())
        {
            g_view.settings_notice = address.error;
            g_view.toast = address.error;
            g_view.toast_frame = g_view.frames;
        }
        else
        {
            config::Settings &settings = config::current();
            if (settings.seerr_host != address.host || settings.seerr_port != address.port)
            {
                settings.seerr_cookie.clear();
                settings.seerr_api_key.clear();
                settings.seerr_user.clear();
            }
            settings.seerr_host = address.host;
            settings.seerr_port = address.port;
            config::save();
            refresh_server();
        }
        break;
    }
    case Field::api_key:
    {
        config::Settings &settings = config::current();
        if (entered.find_first_of("\r\n") != std::string::npos)
        {
            g_view.settings_notice = "The API key contains an invalid line break.";
            break;
        }
        settings.seerr_api_key = entered;
        settings.seerr_auth = 2;
        settings.seerr_cookie.clear();
        settings.seerr_user.clear();
        config::save();
        refresh_server();
        break;
    }
    case Field::account:
        g_view.account = entered;
        break;
    case Field::password:
        g_view.password = entered;
        break;
    case Field::none:
        break;
    }
}

void submit(const Title &title) noexcept
{
    if (!request::can_request(g_view.permissions, title.media_type == "tv", g_view.is4k))
    {
        g_view.sheet_error = "Your account cannot request this format.";
        return;
    }
    const auto body = request::payload(title.media_type, title.tmdb_id, g_view.is4k, g_view.options,
                                       g_view.seasons, g_view.server_index, g_view.profile_index);
    if (body.empty())
    {
        g_view.sheet_error = title.media_type == "tv" ? "Select at least one missing season."
                                                      : "Wait for request options to load.";
        return;
    }
    g_view.sheet_open = false;
    g_view.toast = "Requesting " + title.name + "\xE2\x80\xA6";
    g_view.toast_frame = g_view.frames;
    const auto connection = connection_snapshot();
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (g_model.submitting)
            return;
        g_model.submitting = true;
    }
    if (!background::run([connection, title, body] { job_request(connection, title, body); },
                         background::Lane::requests))
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_model.submitting = false;
        g_model.notice = "The request worker could not start. Restart SlopFin.";
        ++g_model.notice_epoch;
    }
}

void select_missing() noexcept
{
    g_view.seasons.clear();
    for (const auto &season : g_view.options.seasons)
        if (season.number > 0 && season.selectable(g_view.is4k))
            g_view.seasons.push_back(season.number);
}
void choose_default_profile() noexcept
{
    g_view.profile_index = -1;
    if (g_view.server_index < 0)
        return;
    const auto &server = g_view.options.servers[static_cast<std::size_t>(g_view.server_index)];
    for (std::size_t i = 0; i < server.profiles.size(); ++i)
        if (server.profiles[i].id == server.default_profile)
            g_view.profile_index = static_cast<int>(i);
}
void choose_default_server() noexcept
{
    g_view.server_index = -1;
    g_view.profile_index = -1;
    for (std::size_t i = 0; i < g_view.options.servers.size(); ++i)
    {
        const auto &server = g_view.options.servers[i];
        if (server.is4k == g_view.is4k && (g_view.server_index < 0 || server.is_default))
            g_view.server_index = static_cast<int>(i);
    }
    choose_default_profile();
}
void load_options(bool is4k = false) noexcept
{
    const auto connection = connection_snapshot();
    g_view.options = {};
    g_view.options_loading = true;
    g_view.is4k = is4k;
    g_view.seasons.clear();
    g_view.server_index = g_view.profile_index = -1;
    g_view.sheet_row = g_view.season_scroll = 0;
    g_view.sheet_error.clear();
    unsigned long token;
    bool partial;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        token = ++g_model.options_wanted;
        partial = g_model.partial_requests;
    }
    if (!background::run([connection, title = g_view.sheet, token, permissions = g_view.permissions,
                          partial] { job_options(connection, title, token, permissions, partial); },
                         background::Lane::requests))
    {
        g_view.options_loading = false;
        g_view.options.loaded = true;
        g_view.options.error = "The request worker could not start. Close this title and retry.";
    }
}
Title effective_title() noexcept
{
    auto title = g_view.sheet;
    if (g_view.options.loaded && g_view.options.error.empty())
    {
        title.availability =
            availability_of(g_view.is4k ? g_view.options.status4k : g_view.options.status);
        title.jellyfin_id = g_view.is4k ? g_view.options.jellyfin_id4k : g_view.options.jellyfin_id;
    }
    return title;
}
enum class SheetKind
{
    format,
    server,
    profile,
    all_seasons,
    season
};
struct SheetRow
{
    SheetKind kind;
    int season = 0;
};
std::vector<SheetRow> sheet_rows() noexcept
{
    std::vector<SheetRow> rows;
    if (!g_view.options.loaded || !g_view.options.error.empty())
        return rows;
    if (g_view.options.allow4k)
        rows.push_back({SheetKind::format});
    const int server_count = static_cast<int>(
        std::count_if(g_view.options.servers.begin(), g_view.options.servers.end(),
                      [](const auto &server) { return server.is4k == g_view.is4k; }));
    if (g_view.options.advanced && server_count > 1)
        rows.push_back({SheetKind::server});
    rows.push_back({SheetKind::profile});
    if (g_view.sheet.media_type == "tv")
    {
        rows.push_back({SheetKind::all_seasons});
        for (std::size_t i = 0; i < g_view.options.seasons.size(); ++i)
            rows.push_back({SheetKind::season, static_cast<int>(i)});
    }
    return rows;
}
bool selected_season(int number) noexcept
{
    return std::find(g_view.seasons.begin(), g_view.seasons.end(), number) != g_view.seasons.end();
}
void change_sheet_row(const SheetRow &row, int step) noexcept
{
    g_view.sheet_error.clear();
    switch (row.kind)
    {
    case SheetKind::format:
        g_view.is4k = !g_view.is4k;
        choose_default_server();
        select_missing();
        break;
    case SheetKind::server:
    {
        const int count = static_cast<int>(g_view.options.servers.size());
        int index = g_view.server_index;
        for (int i = 0; i < count; ++i)
        {
            index = (index + step + count) % count;
            if (g_view.options.servers[static_cast<std::size_t>(index)].is4k == g_view.is4k)
            {
                g_view.server_index = index;
                choose_default_profile();
                break;
            }
        }
        break;
    }
    case SheetKind::profile:
        if (g_view.options.advanced && g_view.server_index >= 0)
        {
            const int count = static_cast<int>(
                g_view.options.servers[static_cast<std::size_t>(g_view.server_index)]
                    .profiles.size());
            if (count > 0)
                g_view.profile_index = (g_view.profile_index + step + count) % count;
        }
        break;
    case SheetKind::all_seasons:
        if (g_view.seasons.empty() || !g_view.options.partial)
            select_missing();
        else
            g_view.seasons.clear();
        break;
    case SheetKind::season:
    {
        const auto &season = g_view.options.seasons[static_cast<std::size_t>(row.season)];
        if (!g_view.options.partial || !season.selectable(g_view.is4k))
            break;
        if (selected_season(season.number))
            std::erase(g_view.seasons, season.number);
        else
            g_view.seasons.push_back(season.number);
        break;
    }
    }
}

void open_sheet_for_result(int index) noexcept
{
    if (index < 0 || index >= results_size())
        return;
    g_view.sheet =
        g_view
            .results[static_cast<std::size_t>(visible_results()[static_cast<std::size_t>(index)])];
    g_view.sheet_from_requests = false;
    g_view.sheet_approval = 0;
    g_view.sheet_open = true;
    load_options();
}

void open_sheet_for_request(const Request &request) noexcept
{
    Title title;
    title.tmdb_id = request.tmdb_id;
    title.media_type = request.media_type;
    title.name = request.name;
    title.poster_path = request.poster_path;
    title.availability = request.availability;
    title.requested = true;
    title.jellyfin_id = request.jellyfin_id;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (const auto it = g_model.known_titles.find(key_for(title.media_type, title.tmdb_id));
            it != g_model.known_titles.end())
        {
            title.overview = it->second.overview;
            title.year = it->second.year;
            title.rating = it->second.rating;
            if (title.jellyfin_id.empty())
                title.jellyfin_id = it->second.jellyfin_id;
        }
    }
    g_view.sheet = title;
    g_view.sheet_from_requests = true;
    g_view.sheet_approval = request.approval;
    g_view.sheet_request_id = request.id;
    g_view.sheet_open = true;
    load_options(request.is4k);
}

void open_filters() noexcept
{
    g_view.filters_open = true;
    g_view.filter_row = 0;
    g_view.draft_media = g_view.media_filter;
    g_view.draft_status = g_view.mode == 0 ? g_view.availability_filter : g_view.filter;
}
bool filters_input() noexcept
{
    if (pad::pressed(pad::Button::circle))
    {
        g_view.filters_open = false;
        return true;
    }
    if (pad::pressed(pad::Button::up))
        g_view.filter_row = std::max(0, g_view.filter_row - 1);
    if (pad::pressed(pad::Button::down))
        g_view.filter_row = std::min(3, g_view.filter_row + 1);
    const bool confirm = pad::pressed(pad::Button::cross);
    const int step = pad::pressed(pad::Button::left)                 ? -1
                     : (pad::pressed(pad::Button::right) || confirm) ? 1
                                                                     : 0;
    if (step && g_view.filter_row == 0)
        g_view.draft_media = (g_view.draft_media + step + 3) % 3;
    if (step && g_view.filter_row == 1)
    {
        const int count = g_view.mode == 0 ? 4 : 6;
        g_view.draft_status = (g_view.draft_status + step + count) % count;
    }
    if (confirm && g_view.filter_row == 2)
        g_view.draft_media = g_view.draft_status = 0;
    if ((confirm && g_view.filter_row == 3) || pad::pressed(pad::Button::r3))
    {
        g_view.media_filter = g_view.draft_media;
        if (g_view.mode == 0)
            g_view.availability_filter = g_view.draft_status;
        else
            g_view.filter = g_view.draft_status;
        g_view.grid_index = g_view.grid_scroll = g_view.request_index = g_view.request_scroll = 0;
        g_view.filters_open = false;
    }
    return true;
}

bool search_input() noexcept
{
    const int count = results_size();
    if (pad::pressed(pad::Button::triangle))
    {
        open_keyboard(Field::query);
        return true;
    }
    if (g_view.field_focused)
    {
        if (pad::pressed(pad::Button::cross))
        {
            if (g_view.filter_focused)
                open_filters();
            else
                open_keyboard(Field::query);
        }
        else if (pad::pressed(pad::Button::right))
            g_view.filter_focused = true;
        else if (pad::pressed(pad::Button::left))
        {
            if (g_view.filter_focused)
                g_view.filter_focused = false;
            else
                return false;
        }
        else if (pad::pressed(pad::Button::down) && count > 0)
            g_view.field_focused = false;
        else if (pad::pressed(pad::Button::circle))
            return false;
        return true;
    }
    const int columns = grid_columns();
    g_view.grid_index = std::clamp(g_view.grid_index, 0, std::max(0, count - 1));
    if (pad::pressed(pad::Button::right) && g_view.grid_index + 1 < count)
        ++g_view.grid_index;
    else if (pad::pressed(pad::Button::left))
    {
        if (g_view.grid_index % columns == 0)
            return false;
        --g_view.grid_index;
    }
    else if (pad::pressed(pad::Button::down) && g_view.grid_index + columns < count)
        g_view.grid_index += columns;
    else if (pad::pressed(pad::Button::up))
    {
        if (g_view.grid_index < columns)
        {
            g_view.field_focused = true;
            g_view.filter_focused = false;
        }
        else
            g_view.grid_index -= columns;
    }
    else if (pad::pressed(pad::Button::cross) && count > 0)
        open_sheet_for_result(g_view.grid_index);
    else if (pad::pressed(pad::Button::circle))
        return false;
    return true;
}
bool requests_input() noexcept
{
    if (pad::pressed(pad::Button::triangle))
    {
        refresh_requests();
        return true;
    }
    const auto visible = visible_requests();
    const int count = static_cast<int>(visible.size());
    if (g_view.field_focused)
    {
        if (pad::pressed(pad::Button::cross))
        {
            if (g_view.filter_focused)
                open_filters();
            else
                refresh_requests();
        }
        else if (pad::pressed(pad::Button::right))
            g_view.filter_focused = true;
        else if (pad::pressed(pad::Button::left))
        {
            if (g_view.filter_focused)
                g_view.filter_focused = false;
            else
                return false;
        }
        else if (pad::pressed(pad::Button::down) && count)
            g_view.field_focused = false;
        else if (pad::pressed(pad::Button::circle))
            return false;
        return true;
    }
    g_view.request_index = std::clamp(g_view.request_index, 0, std::max(0, count - 1));
    if (pad::pressed(pad::Button::up))
    {
        if (g_view.request_index > 0)
            --g_view.request_index;
        else
        {
            g_view.field_focused = true;
            g_view.filter_focused = false;
        }
    }
    else if (pad::pressed(pad::Button::down) && g_view.request_index + 1 < count)
        ++g_view.request_index;
    else if (pad::pressed(pad::Button::cross) && count)
        open_sheet_for_request(g_view.requests[static_cast<std::size_t>(
            visible[static_cast<std::size_t>(g_view.request_index)])]);
    else if (pad::pressed(pad::Button::left) || pad::pressed(pad::Button::circle))
        return false;
    return true;
}
std::string sheet_action() noexcept
{
    if (g_view.options_loading)
        return "Loading options…";
    if (!g_view.options.error.empty())
        return "Retry options";
    const auto title = effective_title();
    if (title.availability == Availability::available && !title.jellyfin_id.empty() &&
        (title.media_type != "tv" || g_view.seasons.empty()))
        return "Open in Jellyfin";
    if (!g_view.authenticated)
        return "Connect request account";
    if (!request::can_request(g_view.permissions, title.media_type == "tv", g_view.is4k))
        return "Requesting is restricted";
    if (title.media_type == "tv")
    {
        if (g_view.seasons.empty())
            return "Select seasons to request";
        return "Request " + std::to_string(g_view.seasons.size()) +
               (g_view.seasons.size() == 1 ? " season" : " seasons");
    }
    if (title.availability != Availability::unknown)
        return request_action_label(title, g_view.sheet_from_requests);
    return g_view.is4k ? "Request movie in 4K" : "Request movie";
}
bool sheet_input() noexcept
{
    if (pad::pressed(pad::Button::circle))
    {
        g_view.sheet_open = false;
        return true;
    }
    const auto rows = sheet_rows();
    const int action = static_cast<int>(rows.size());
    g_view.sheet_row = std::clamp(g_view.sheet_row, 0, action);
    if (pad::pressed(pad::Button::up))
        g_view.sheet_row = std::max(0, g_view.sheet_row - 1);
    if (pad::pressed(pad::Button::down))
        g_view.sheet_row = std::min(action, g_view.sheet_row + 1);
    if (pad::pressed(pad::Button::options))
        g_view.sheet_row = action;
    if (pad::pressed(pad::Button::square) && g_view.sheet.media_type == "tv" &&
        g_view.options.loaded)
    {
        change_sheet_row({SheetKind::all_seasons}, 1);
        return true;
    }
    const bool confirm = pad::pressed(pad::Button::cross);
    const int step = pad::pressed(pad::Button::left)    ? -1
                     : pad::pressed(pad::Button::right) ? 1
                                                        : 0;
    if (g_view.sheet_row < action &&
        (confirm ||
         (step && rows[static_cast<std::size_t>(g_view.sheet_row)].kind != SheetKind::season &&
          rows[static_cast<std::size_t>(g_view.sheet_row)].kind != SheetKind::all_seasons)))
        change_sheet_row(rows[static_cast<std::size_t>(g_view.sheet_row)], step ? step : 1);
    else if (confirm && g_view.sheet_row == action && !g_view.options_loading)
    {
        const auto title = effective_title();
        if (!g_view.options.error.empty())
            load_options(g_view.is4k);
        else if (title.availability == Availability::available && !title.jellyfin_id.empty() &&
                 (title.media_type != "tv" || g_view.seasons.empty()))
        {
            g_open_item = title.jellyfin_id;
            g_view.sheet_open = false;
        }
        else if (!g_view.authenticated)
        {
            g_wants_settings = true;
            g_view.sheet_open = false;
        }
        else if ((title.media_type == "tv" && !g_view.seasons.empty()) ||
                 can_request(title, g_view.sheet_from_requests, g_view.is4k))
            submit(title);
        else
            g_view.sheet_error = "Choose an unrequested season or another format.";
    }
    return true;
}

/* ----------------------------------------------------------------- drawing */

std::string poster_url_for(const std::string &path, int height) noexcept
{
    const char *size = height <= 100   ? "w92"
                       : height <= 200 ? "w154"
                       : height <= 400 ? "w342"
                       : height <= 600 ? "w500"
                                       : "w780";
    return std::string{"https://image.tmdb.org/t/p/"} + size + path;
}

void draw_poster(const std::string &path, int x, int y, int w, int h, int radius,
                 const std::string &name) noexcept
{
    const gfx::Bitmap *art = nullptr;
    if (!path.empty())
        art = images::acquire(poster_url_for(path, h), {}, images::Kind::remote, h + 120);
    if (art != nullptr && art->valid())
    {
        gfx::blit_cover(*art, x, y, w, h, radius, 255);
        return;
    }
    gfx::rounded_rect(x, y, w, h, radius, gfx::rgba(0xff, 0xff, 0xff, 0x0a));
    gfx::stroke_rounded_rect(x, y, w, h, radius, 1, gfx::rgba(0xff, 0xff, 0xff, 0x16));
    std::string initial = "?";
    if (!name.empty())
    {
        const unsigned char lead = static_cast<unsigned char>(name[0]);
        const std::size_t length = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
        initial = name.substr(0, std::min(length, name.size()));
    }
    const int size = std::max(30, h / 4);
    text::draw(x + (w - text::measure(initial, size, Weight::bold)) / 2, y + (h - size) / 2,
               initial, size, Weight::bold, gfx::rgba(0xff, 0xff, 0xff, 0x2e));
}

void draw_status_pill(int x, int y, Availability value, const std::string &override_text) noexcept
{
    const std::string label = override_text.empty() ? availability_label(value) : override_text;
    if (label.empty())
        return;
    const int pill_w = text::measure(label, 19, Weight::medium) + 28;
    const int pill_h = 34;
    const gfx::Color colour = availability_colour(value);
    gfx::rounded_rect(x, y, pill_w, pill_h, pill_h / 2, gfx::with_alpha(colour, 0xd8));
    text::draw(x + 14, text::centered_y(y, pill_h, label, 19, Weight::medium), label, 19,
               Weight::medium, gfx::rgb(0x10, 0x10, 0x12));
}

constexpr const char *kMediaFilters[] = {"Movies & series", "Movies", "Series"};
constexpr const char *kAvailabilityFilters[] = {"Any availability", "Not requested", "Requested",
                                                "Available"};
constexpr const char *kRequestFilters[] = {"All requests",      "Downloading", "Available",
                                           "Awaiting approval", "Processing",  "Declined"};

void draw_header() noexcept
{
    text::draw(ui::kContentX, kHeaderY, "Discover", 40, Weight::bold, gfx::palette::text);
    const std::string subtitle =
        g_view.server_title.empty()
            ? "Browse titles. Make requests. Follow downloads."
            : g_view.server_title + (config::current().seerr_user.empty()
                                         ? ""
                                         : "  ·  " + config::current().seerr_user);
    text::draw_ellipsized(ui::kContentX, kHeaderY + 54, 800, subtitle, 20, Weight::regular,
                          gfx::palette::text_dim);
    constexpr int width = 528, height = 66;
    const int x = ui::kContentRight - width;
    const int y = kHeaderY - 6;
    gfx::rounded_rect(x, y, width, height, 18, gfx::palette::control);
    gfx::stroke_rounded_rect(x, y, width, height, 18, 1, gfx::palette::control_border);
    g_view.tab_position.aim(static_cast<float>(g_view.mode));
    g_view.tab_position.step(motion::kControlFocus, 1.0f / 60.0f);
    gfx::rounded_rect(x + 6 + static_cast<int>(g_view.tab_position.value() * 258), y + 6, 258, 54,
                      13, gfx::palette::focus_surface);
    gfx::fill_rect(x + 46 + static_cast<int>(g_view.tab_position.value() * 258), y + height - 5,
                   178, 2, gfx::palette::focus_border);
    const char *names[] = {"Browse", "My requests"};
    for (int i = 0; i < 2; ++i)
    {
        const int bx = x + i * 258;
        const bool active = i == g_view.mode;
        if (!active)
            icons::draw(icons::Icon::ps_square, bx + 28, y + 21, 24, icons::button::square);
        const int label_width = text::measure(names[i], 24, Weight::medium);
        const int tx = active ? bx + (264 - label_width) / 2 : bx + 68;
        text::draw(tx, text::centered_y(y, height, names[i], 24, Weight::medium), names[i], 24,
                   Weight::medium, active ? gfx::palette::text : gfx::palette::text_dim);
    }
}
void draw_field() noexcept
{
    constexpr int width = 1048, height = 66;
    const bool focused = g_view.field_focused && !g_view.filter_focused && !g_view.sheet_open &&
                         !g_view.filters_open;
    gfx::rounded_rect(ui::kContentX, kFieldY, width, height, 15, gfx::palette::control);
    if (focused)
        ui::focus_pill(ui::kContentX, kFieldY, width, height, ui::FocusGroup::discover_toolbar);
    else
        gfx::stroke_rounded_rect(ui::kContentX, kFieldY, width, height, 15, 1,
                                 gfx::palette::control_border);
    icons::draw(icons::Icon::ps_triangle, ui::kContentX + 24, kFieldY + 21, 26,
                icons::button::triangle);
    const auto label = g_view.mode == 1       ? std::string("Refresh requests")
                       : g_view.query.empty() ? std::string("Search movies and series")
                                              : g_view.query;
    text::draw_ellipsized(ui::kContentX + 72,
                          text::centered_y(kFieldY, height, label, 26, Weight::medium), width - 100,
                          label, 26, Weight::medium, gfx::palette::text);
    constexpr int fx = ui::kContentX + width + 24;
    constexpr int fw = ui::kContentRight - fx;
    gfx::rounded_rect(fx, kFieldY, fw, height, 15, gfx::palette::control);
    if (g_view.field_focused && g_view.filter_focused && !g_view.filters_open && !g_view.sheet_open)
        ui::focus_pill(fx, kFieldY, fw, height, ui::FocusGroup::discover_toolbar);
    else
        gfx::stroke_rounded_rect(fx, kFieldY, fw, height, 15, 1, gfx::palette::control_border);
    gfx::rounded_rect(fx + 22, kFieldY + 18, 42, 30, 8, gfx::palette::surface_high);
    ui::control_label(fx + 22, kFieldY + 18, 42, 30, "R3", 18, Weight::bold, gfx::palette::text);
    const int active = (g_view.media_filter != 0) +
                       ((g_view.mode == 0 ? g_view.availability_filter : g_view.filter) != 0);
    const std::string filters = "Filters" + (active ? "  ·  " + std::to_string(active) : "");
    text::draw(fx + 82, text::centered_y(kFieldY, height, filters, 25, Weight::medium), filters, 25,
               Weight::medium, gfx::palette::text);
    icons::draw(icons::Icon::chevron_down, fx + fw - 42, kFieldY + 23, 20, gfx::palette::text_dim);
    const int count =
        g_view.mode == 0 ? results_size() : static_cast<int>(visible_requests().size());
    const bool loading = g_view.mode == 0 ? g_view.search_loading : g_view.requests_loading;
    const std::string summary =
        (g_view.mode == 0 ? (g_view.query.empty() ? "Trending" : "Search results")
                          : "Latest requests") +
        std::string("  /  ") + kMediaFilters[g_view.media_filter] + "  /  " +
        (g_view.mode == 0 ? kAvailabilityFilters[g_view.availability_filter]
                          : kRequestFilters[g_view.filter]);
    text::draw_ellipsized(ui::kContentX, kFieldY + 88, 1120, summary, 20, Weight::regular,
                          gfx::palette::text_dim);
    const std::string total =
        loading ? "Refreshing…"
                : std::to_string(count) + (g_view.mode == 0 ? " titles" : " requests");
    text::draw(ui::kContentRight - text::measure(total, 20, Weight::medium), kFieldY + 88, total,
               20, Weight::medium, gfx::palette::text_dim);
}
void draw_filters() noexcept
{
    gfx::fill_rect(0, 0, gfx::kWidth, gfx::kHeight, gfx::rgba(0, 0, 0, 0xc8));
    constexpr int x = 620, y = 240, width = 1040;
    ui::panel(x, y, width, 600);
    text::draw(x + 40, y + 34, "Filter " + std::string(g_view.mode == 0 ? "titles" : "my requests"),
               36, Weight::bold, gfx::palette::text);
    text::draw(x + 40, y + 92, "Choose with Left / Right. Apply when ready.", 22, Weight::regular,
               gfx::palette::text_dim);
    const char *labels[] = {"Media", "Status", "Reset filters", "Apply filters"};
    for (int i = 0; i < 4; ++i)
    {
        const int ry = y + 156 + i * 86;
        if (g_view.filter_row == i)
            ui::focus_pill(x + 24, ry, width - 48, 68, ui::FocusGroup::discover_filters,
                           g_view.mode);
        text::draw(x + 48, text::centered_y(ry, 68, labels[i], 25, Weight::medium), labels[i], 25,
                   Weight::medium, gfx::palette::text);
        if (i < 2)
        {
            const char *value = i == 0             ? kMediaFilters[g_view.draft_media]
                                : g_view.mode == 0 ? kAvailabilityFilters[g_view.draft_status]
                                                   : kRequestFilters[g_view.draft_status];
            text::draw_ellipsized(x + 420, text::centered_y(ry, 68, value, 24, Weight::regular),
                                  500, value, 24, Weight::regular, gfx::palette::text_dim);
            icons::draw(icons::Icon::chevron_left, x + 374, ry + 24, 20, gfx::palette::text_dim);
            icons::draw(icons::Icon::chevron_right, x + width - 68, ry + 24, 20,
                        gfx::palette::text_dim);
        }
    }
    ui::draw_button_hints(x + 48, y + 538,
                          {{icons::Icon::ps_cross, "Choose"}, {icons::Icon::ps_circle, "Cancel"}});
}

void draw_result_card(const Title &title, int x, int y, bool focused) noexcept
{
    draw_poster(title.poster_path, x, y, kCardW, kCardH, 10, title.name);
    if (focused)
        gfx::stroke_rounded_rect(x - 4, y - 4, kCardW + 8, kCardH + 8, 14, 3,
                                 gfx::palette::focus_border);
    if (title.availability != Availability::unknown)
    {
        const std::string label = availability_label(title.availability);
        const int pill_w = text::measure(label, 17, Weight::medium) + 22;
        gfx::rounded_rect(x + 10, y + 10, pill_w, 30, 15,
                          gfx::with_alpha(availability_colour(title.availability), 0xd8));
        text::draw(x + 21, text::centered_y(y + 10, 30, label, 17, Weight::medium), label, 17,
                   Weight::medium, gfx::rgb(0x10, 0x10, 0x12));
    }
    text::draw_ellipsized(x, y + kCardH + 10, kCardW, title.name, 22, Weight::medium,
                          gfx::palette::text);
    std::string subtitle = title.media_type == "tv" ? "Series" : "Movie";
    if (title.year > 0)
        subtitle += "  ·  " + std::to_string(title.year);
    if (title.rating > 0.0)
    {
        if (!subtitle.empty())
            subtitle += "  \xC2\xB7  ";
        subtitle += format("%.1f", title.rating);
    }
    if (!subtitle.empty())
        text::draw(x, y + kCardH + 40, subtitle, 18, Weight::regular, gfx::palette::text_dim);
}

void draw_results() noexcept
{
    if (config::current().seerr_host.empty())
    {
        text::draw(ui::kContentX, kGridY + 20, "Find your next movie or show", 34, Weight::bold,
                   gfx::palette::text);
        text::draw_wrapped(ui::kContentX, kGridY + 80, 920, 3, 36,
                           "Connect Seerr or Jellyseerr to browse new titles, request them and "
                           "follow downloads from your PS5.",
                           26, Weight::regular, gfx::palette::text_dim);
        ui::focus_pill(ui::kContentX, kGridY + 220, 430, 66);
        ui::control_label(ui::kContentX + 24, kGridY + 220, 382, 66, "Connect request server", 26,
                          Weight::medium, gfx::palette::text);
        return;
    }
    if (g_view.search_loading && g_view.results.empty())
    {
        text::draw(ui::kContentX, kGridY + 20, "Searching\xE2\x80\xA6", 26, Weight::regular,
                   gfx::palette::text_dim);
        return;
    }
    if (!g_view.search_error.empty() && g_view.results.empty())
    {
        text::draw(ui::kContentX, kGridY + 20, g_view.search_error, 26, Weight::regular,
                   gfx::palette::danger);
        return;
    }
    const auto visible = visible_results();
    const int count = static_cast<int>(visible.size());
    if (count == 0)
    {
        const std::string message =
            g_view.query.empty() ? "No titles match these filters. Try another filter or search."
                                 : "Nothing matched \"" + g_view.query + "\".";
        text::draw(ui::kContentX, kGridY + 20, message, 26, Weight::regular,
                   gfx::palette::text_dim);
        return;
    }

    const int columns = grid_columns();
    const int rows = (count + columns - 1) / columns;
    const int visible_rows =
        1 + std::max(0, gfx::kHeight - ui::kSafeY - kGridY - (kCardH + 58)) / kCardStepY;
    const int focused_row = g_view.grid_index / columns;
    int first = g_view.grid_scroll;
    if (focused_row < first)
        first = focused_row;
    if (focused_row >= first + visible_rows)
        first = focused_row - visible_rows + 1;
    first = std::clamp(first, 0, std::max(0, rows - visible_rows));
    g_view.grid_scroll = first;

    gfx::push_clip(ui::kContentX - 8, kGridY - 8, ui::kContentWidth + 16,
                   gfx::kHeight - kGridY - 50);
    for (int i = 0; i < count; ++i)
    {
        const int row = i / columns;
        if (row < first || row >= first + visible_rows)
            continue;
        const int column = i % columns;
        draw_result_card(
            g_view.results[static_cast<std::size_t>(visible[static_cast<std::size_t>(i)])],
            ui::kContentX + column * kCardStepX, kGridY + (row - first) * kCardStepY,
            i == g_view.grid_index && !g_view.field_focused);
    }
    /* Warm the row just below the window, so scrolling does not stall. */
    const int warm_row = first + visible_rows;
    if (warm_row < rows)
        for (int column = 0; column < columns; ++column)
        {
            const int index = warm_row * columns + column;
            if (index >= count)
                break;
            const std::string &path =
                g_view.results[static_cast<std::size_t>(visible[static_cast<std::size_t>(index)])]
                    .poster_path;
            if (!path.empty())
                images::prefetch(poster_url_for(path, kCardH), {}, images::Kind::remote,
                                 kCardH + 120);
        }
    gfx::pop_clip();
}

void draw_progress(const Request &request, int x, int y, int width) noexcept
{
    constexpr int kBarH = 10;
    gfx::rounded_rect(x, y, width, kBarH, kBarH / 2, gfx::palette::surface_high);
    const double progress = request.progress < 0.0 ? 0.0 : request.progress;
    const int filled = static_cast<int>(width * progress);
    if (filled > 0)
        gfx::rounded_rect(x, y, filled, kBarH, kBarH / 2, gfx::palette::accent_alt);
    std::string detail = format("%.0f%%", progress * 100.0);
    if (request.remaining_bytes > 0)
        detail += "  \xC2\xB7  " + human_bytes(request.remaining_bytes) + " left";
    if (request.rate_bytes > 0)
        detail += "  \xC2\xB7  " + human_bytes(request.rate_bytes) + "/s";
    if (!request.eta.empty())
        detail += "  \xC2\xB7  " + request.eta;
    text::draw(x, y + kBarH + 8, detail, 19, Weight::regular, gfx::palette::text_dim);
}

void draw_request_row(const Request &request, int y, bool focused) noexcept
{
    constexpr int kRowH = 152;
    constexpr int kThumbW = 72;
    constexpr int kThumbH = 108;
    constexpr int kPad = 16;
    const int x = ui::kContentX;
    const int w = ui::kContentWidth;
    gfx::rounded_rect(x - 8, y, w + 16, kRowH - 14, 14,
                      focused ? gfx::palette::focus_surface : gfx::palette::control);
    if (focused)
        gfx::stroke_rounded_rect(x - 8, y, w + 16, kRowH - 14, 14, 2, gfx::palette::focus_border);

    draw_poster(request.poster_path, x + kPad, y + kPad, kThumbW, kThumbH, 8,
                request.name.empty() ? "?" : request.name);

    const int tx = x + kPad + kThumbW + 26;
    const int text_right = ui::kContentRight - 40;
    const std::string name = request.name.empty() ? "Loading\xE2\x80\xA6" : request.name;
    text::draw_ellipsized(tx, y + 18, text_right - tx - 240, name, 30, Weight::medium,
                          gfx::palette::text);

    std::string meta = std::string{request.media_type == "tv" ? "Series" : "Movie"};
    if (request.is4k)
        meta += "  ·  4K";
    if (!request.seasons.empty())
    {
        meta += request.seasons.size() == 1
                    ? "  ·  Season " + std::to_string(request.seasons.front())
                    : "  ·  " + std::to_string(request.seasons.size()) + " seasons";
    }
    if (!request.profile_name.empty())
        meta += "  ·  " + request.profile_name;
    if (!request.requested_by.empty())
        meta += "  \xC2\xB7  asked for by " + request.requested_by;
    text::draw_ellipsized(tx, y + 58, text_right - tx - 30, meta, 20, Weight::regular,
                          gfx::palette::text_dim);

    if (request.downloading && request.progress >= 0.0)
        draw_progress(request, tx, y + 96, 620);
    else if (request.approval == 1)
        text::draw(tx, y + 96, "Waiting for approval", 20, Weight::regular, gfx::palette::text_dim);

    const char *badge = request.availability == Availability::unknown
                            ? approval_label(request.approval)
                            : availability_label(request.availability);
    const int pill_w = text::measure(badge, 19, Weight::medium) + 28;
    draw_status_pill(text_right - pill_w, y + 18, request.availability, std::string{badge});
}

void draw_requests() noexcept
{
    if (g_view.requests_error.empty() && g_view.requests.empty() && g_view.requests_loading)
    {
        text::draw(ui::kContentX, kListY, "Loading your requests\xE2\x80\xA6", 26, Weight::regular,
                   gfx::palette::text_dim);
        return;
    }
    if (!g_view.requests_error.empty() && g_view.requests.empty())
    {
        text::draw_ellipsized(ui::kContentX, kListY, ui::kContentWidth, g_view.requests_error, 26,
                              Weight::regular, gfx::palette::danger);
        return;
    }

    const std::vector<int> visible = visible_requests();
    const int count = static_cast<int>(visible.size());
    if (count == 0)
    {
        text::draw(ui::kContentX, kListY, "Nothing here yet.", 26, Weight::regular,
                   gfx::palette::text_dim);
        return;
    }

    constexpr int kRowH = 152;
    const int visible_rows = std::max(1, (gfx::kHeight - 40 - kListY) / kRowH);
    g_view.request_index = std::clamp(g_view.request_index, 0, count - 1);
    int first = g_view.request_scroll;
    if (g_view.request_index < first)
        first = g_view.request_index;
    if (g_view.request_index >= first + visible_rows)
        first = g_view.request_index - visible_rows + 1;
    first = std::clamp(first, 0, std::max(0, count - visible_rows));
    g_view.request_scroll = first;

    gfx::push_clip(ui::kContentX - 12, kListY - 8, ui::kContentWidth + 24,
                   gfx::kHeight - kListY - 20);
    for (int i = first; i < std::min(count, first + visible_rows); ++i)
    {
        const Request &request =
            g_view.requests[static_cast<std::size_t>(visible[static_cast<std::size_t>(i)])];
        draw_request_row(request, kListY + (i - first) * kRowH,
                         i == g_view.request_index && !g_view.field_focused);
    }
    gfx::pop_clip();
}

std::string profile_label() noexcept
{
    if (g_view.server_index < 0)
        return "Server default";
    const auto &server = g_view.options.servers[static_cast<std::size_t>(g_view.server_index)];
    if (g_view.profile_index >= 0 &&
        g_view.profile_index < static_cast<int>(server.profiles.size()))
        return server.profiles[static_cast<std::size_t>(g_view.profile_index)].name +
               (server.profiles[static_cast<std::size_t>(g_view.profile_index)].id ==
                        server.default_profile
                    ? " (default)"
                    : "");
    for (const auto &profile : server.profiles)
        if (profile.id == server.default_profile)
            return profile.name + " (default)";
    return "Server default";
}
void draw_sheet() noexcept
{
    gfx::fill_rect(0, 0, gfx::kWidth, gfx::kHeight, gfx::rgba(0, 0, 0, 0xda));
    constexpr int x = ui::kContentX - 20, y = 92, width = ui::kContentWidth + 20, height = 896;
    ui::panel(x, y, width, height);
    constexpr int tx = x + 350, tw = width - 390;
    const auto title = effective_title();
    draw_poster(title.poster_path, x + 40, y + 48, 264, 396, 14, title.name);
    text::draw_ellipsized(tx, y + 40, tw, title.name, 38, Weight::bold, gfx::palette::text);
    std::string meta = title.media_type == "tv" ? "SERIES" : "MOVIE";
    if (title.year)
        meta += "  ·  " + std::to_string(title.year);
    if (title.rating > 0)
        meta += "  ·  " + format("%.1f", title.rating) + " / 10";
    text::draw(tx, y + 98, meta, 21, Weight::medium, gfx::palette::text_dim);
    text::draw_wrapped(tx, y + 146, tw, 3, 32,
                       title.overview.empty() ? "No description available." : title.overview, 23,
                       Weight::regular, gfx::palette::text_dim);
    draw_status_pill(x + 40, y + 472, title.availability, {});
    text::draw(x + 40, y + 540, g_view.is4k ? "4K request" : "Standard request", 24, Weight::medium,
               gfx::palette::text);
    text::draw_wrapped(x + 40, y + 584, 264, 3, 30, profile_label(), 22, Weight::regular,
                       gfx::palette::text_dim);
    if (!config::current().seerr_user.empty())
    {
        text::draw(x + 40, y + 720, "Requesting as", 19, Weight::regular, gfx::palette::text_faint);
        text::draw_ellipsized(x + 40, y + 752, 264, config::current().seerr_user, 24,
                              Weight::medium, gfx::palette::text);
    }
    text::draw(tx, y + 260, "REQUEST OPTIONS", 19, Weight::bold, gfx::palette::text_faint);
    ui::draw_button_hints(tx + 520, y + 260, {{icons::Icon::r2, "Review request", "OPTIONS"}});
    const auto rows = sheet_rows();
    const int count = static_cast<int>(rows.size());
    g_view.sheet_row = std::clamp(g_view.sheet_row, 0, count);
    constexpr int row_y = y + 308, row_h = 60, visible_rows = 6;
    if (g_view.sheet_row < count)
    {
        if (g_view.sheet_row < g_view.season_scroll)
            g_view.season_scroll = g_view.sheet_row;
        if (g_view.sheet_row >= g_view.season_scroll + visible_rows)
            g_view.season_scroll = g_view.sheet_row - visible_rows + 1;
    }
    g_view.season_scroll = std::clamp(g_view.season_scroll, 0, std::max(0, count - visible_rows));
    for (int i = g_view.season_scroll; i < std::min(count, g_view.season_scroll + visible_rows);
         ++i)
    {
        const auto row = rows[static_cast<std::size_t>(i)];
        const int ry = row_y + (i - g_view.season_scroll) * row_h;
        const bool focused = g_view.sheet_row == i;
        if (focused)
            ui::focus_pill(tx - 14, ry, tw + 28, 54, ui::FocusGroup::discover_sheet, title.tmdb_id);
        std::string label, value;
        bool enabled = true;
        switch (row.kind)
        {
        case SheetKind::format:
            label = "Format";
            value = g_view.is4k ? "4K" : "Standard";
            break;
        case SheetKind::server:
            label = "Download server";
            value =
                g_view.server_index < 0
                    ? "Server default"
                    : g_view.options.servers[static_cast<std::size_t>(g_view.server_index)].name;
            break;
        case SheetKind::profile:
            label = "Quality profile";
            value = profile_label();
            enabled = g_view.options.advanced;
            break;
        case SheetKind::all_seasons:
            label = "Seasons";
            value = std::to_string(g_view.seasons.size()) + " selected";
            icons::draw(icons::Icon::ps_square, tx + tw - 30, ry + 16, 22, icons::button::square);
            break;
        case SheetKind::season:
        {
            const auto &season = g_view.options.seasons[static_cast<std::size_t>(row.season)];
            enabled = g_view.options.partial && season.selectable(g_view.is4k);
            label = season.number == 0 ? "Specials" : "Season " + std::to_string(season.number);
            const int status = g_view.is4k ? season.status4k : season.status;
            value = status == 5   ? "Available"
                    : status == 4 ? "Partly available"
                    : !season.selectable(g_view.is4k)
                        ? "Already requested"
                        : std::to_string(season.episodes) + " episodes";
            gfx::stroke_rounded_rect(tx + 8, ry + 16, 22, 22, 5, 1,
                                     enabled ? gfx::palette::text_dim
                                             : gfx::palette::control_border);
            if (selected_season(season.number))
                icons::draw(icons::Icon::check, tx + 10, ry + 18, 18, gfx::palette::accent_alt);
            break;
        }
        }
        const int inset = row.kind == SheetKind::season ? 48 : 8;
        text::draw_ellipsized(tx + inset, text::centered_y(ry, 54, label, 23, Weight::medium),
                              320 - inset, label, 23, Weight::medium,
                              enabled || focused ? gfx::palette::text : gfx::palette::text_dim);
        text::draw_ellipsized(tx + 340, text::centered_y(ry, 54, value, 23, Weight::regular),
                              tw - 390, value, 23, Weight::regular, gfx::palette::text_dim);
        if (row.kind == SheetKind::format || row.kind == SheetKind::server ||
            (row.kind == SheetKind::profile && enabled))
        {
            icons::draw(icons::Icon::chevron_left, tx + 302, ry + 18, 18, gfx::palette::text_faint);
            icons::draw(icons::Icon::chevron_right, tx + tw - 28, ry + 18, 18,
                        gfx::palette::text_faint);
        }
    }
    if (count > visible_rows)
    {
        constexpr int track = visible_rows * row_h;
        gfx::rounded_rect(tx + tw + 22, row_y, 3, track, 1, gfx::palette::control_border);
        const int bar = std::max(24, track * visible_rows / count);
        gfx::rounded_rect(tx + tw + 22,
                          row_y + (track - bar) * g_view.season_scroll / (count - visible_rows), 3,
                          bar, 1, gfx::palette::text_dim);
    }
    const std::string note =
        g_view.options_loading          ? "Loading seasons and quality profiles…"
        : !g_view.options.error.empty() ? g_view.options.error
        : !g_view.sheet_error.empty()   ? g_view.sheet_error
        : !g_view.options.partial && title.media_type == "tv"
            ? "This server requires all missing seasons to be requested together."
        : !g_view.options.advanced              ? "Your account uses the server's quality profile."
        : !g_view.options.profile_error.empty() ? g_view.options.profile_error
        : title.media_type == "tv"
            ? "Cross selects a season. Square selects missing seasons or clears your selection."
            : "Left / Right changes the quality profile before requesting.";
    text::draw_wrapped(tx, y + 690, tw, 2, 27, note, 20, Weight::regular,
                       !g_view.sheet_error.empty() || !g_view.options.error.empty()
                           ? gfx::palette::danger
                           : gfx::palette::text_dim);
    const auto action = sheet_action();
    constexpr int by = y + 768, bh = 66;
    gfx::rounded_rect(tx - 14, by, tw + 28, bh, 14, gfx::palette::control);
    if (g_view.sheet_row == count)
        ui::focus_pill(tx - 14, by, tw + 28, bh, ui::FocusGroup::discover_sheet, title.tmdb_id);
    icons::draw(icons::Icon::ps_cross, tx + 12, by + 20, 26, icons::button::cross);
    text::draw_ellipsized(tx + 58, text::centered_y(by, bh, action, 26, Weight::bold), tw - 74,
                          action, 26, Weight::bold, gfx::palette::text);
    ui::draw_button_hints(tx, y + 854, {{icons::Icon::ps_circle, "Close"}});
}

void draw_toast() noexcept
{
    if (g_view.toast.empty() || g_view.frames - g_view.toast_frame > 240)
        return;
    const int width = text::measure(g_view.toast, 24, Weight::medium) + 56;
    const int x = ui::kContentX + (ui::kContentWidth - width) / 2;
    const int y = gfx::kHeight - 150;
    gfx::rounded_rect(x, y, width, 62, 16, gfx::rgba(0x00, 0x00, 0x00, 0xd8));
    gfx::stroke_rounded_rect(x, y, width, 62, 16, 1, gfx::rgba(0xff, 0xff, 0xff, 0x20));
    text::draw(x + 28, text::centered_y(y, 62, g_view.toast, 24, Weight::medium), g_view.toast, 24,
               Weight::medium, gfx::palette::text);
}

} // namespace

/* ------------------------------------------------------------- the module */

void open() noexcept
{
    g_view.mode = 0;
    g_view.field_focused = true;
    g_view.sheet_open = false;
    g_view.grid_index = 0;
    g_view.grid_scroll = 0;
    g_view.request_index = 0;
    g_view.request_scroll = 0;
    g_view.filter = 0;
    g_view.refresh_at = std::chrono::steady_clock::now();
    refresh_server();
    refresh_requests();
    start_search(g_view.query);
}

#ifdef SLOPFIN_HOST
void preview_search(const std::string &query) noexcept
{
    g_view.query = query;
    g_view.mode = 0;
    g_view.field_focused = false;
    g_view.grid_index = g_view.grid_scroll = 0;
    start_search(query);
}
#endif

bool modal_open() noexcept
{
    return g_view.sheet_open || g_view.filters_open || g_field != Field::none ||
           g_view.ime_guard > 0;
}

std::string take_open_item() noexcept
{
    std::string id;
    id.swap(g_open_item);
    return id;
}

bool take_settings_request() noexcept
{
    const bool requested = g_wants_settings;
    g_wants_settings = false;
    return requested;
}

void close_settings() noexcept
{
    std::fill(g_view.password.begin(), g_view.password.end(), '\0');
    g_view.password.clear();
}

bool handle_input() noexcept
{
    if (g_connection.host.empty())
    {
        if (pad::pressed(pad::Button::cross) || pad::pressed(pad::Button::triangle))
            g_wants_settings = true;
        return !pad::pressed(pad::Button::circle) && !pad::pressed(pad::Button::left);
    }
    if (g_view.ime_guard > 0)
        return true;
    if (g_view.filters_open)
        return filters_input();
    if (g_view.sheet_open)
        return sheet_input();
    if (pad::pressed(pad::Button::r3))
    {
        open_filters();
        return true;
    }
    if (pad::pressed(pad::Button::square) || pad::pressed(pad::Button::l1) ||
        pad::pressed(pad::Button::r1))
    {
        g_view.mode = g_view.mode == 0 ? 1 : 0;
        g_view.sheet_open = false;
        g_view.field_focused = true;
        g_view.filter_focused = false;
        if (g_view.mode == 1)
            refresh_requests();
        return true;
    }
    if (g_view.mode == 0)
        return search_input();
    return requests_input();
}

void draw() noexcept
{
    ++g_view.frames;
    if (g_view.mode == 1 && std::chrono::steady_clock::now() >= g_view.refresh_at)
    {
        refresh_requests();
    }

    draw_header();
    draw_field();
    if (g_view.mode == 0)
    {
        if (!ime::busy())
            draw_results();
    }
    else
        draw_requests();
    if (g_view.sheet_open)
        draw_sheet();
    if (g_view.filters_open)
        draw_filters();
    draw_toast();
}

void poll() noexcept
{
    if (g_view.ime_guard > 0)
        --g_view.ime_guard;
    if (g_field != Field::none)
    {
        std::string entered;
        if (ime::take_result(entered))
        {
            g_view.ime_guard = 2;
            apply_keyboard(entered);
        }
        else if (ime::take_cancelled())
        {
            g_field = Field::none;
            g_view.ime_guard = 2;
        }
    }
    snapshot();
}

enum class Setting
{
    server,
    method,
    key,
    account,
    password,
    connect,
    disconnect
};

std::span<const Setting> settings_rows() noexcept
{
    static constexpr Setting key[] = {Setting::server, Setting::method, Setting::key,
                                      Setting::connect, Setting::disconnect};
    static constexpr Setting account[] = {Setting::server,   Setting::method,  Setting::account,
                                          Setting::password, Setting::connect, Setting::disconnect};
    return config::current().seerr_auth == 2 ? std::span<const Setting>(key)
                                             : std::span<const Setting>(account);
}

const char *settings_row_label(Setting row) noexcept
{
    switch (row)
    {
    case Setting::server:
        return "Server address";
    case Setting::method:
        return "Connect using";
    case Setting::key:
        return "API key";
    case Setting::account:
        return config::current().seerr_auth == 0 ? "Jellyfin username" : "Email address";
    case Setting::password:
        return "Password";
    case Setting::connect:
        return "Connect";
    case Setting::disconnect:
        return "Disconnect";
    }
    return "";
}

std::string settings_row_value(Setting row) noexcept
{
    const auto &settings = config::current();
    switch (row)
    {
    case Setting::server:
        return settings.seerr_host.empty()
                   ? "Not set"
                   : server_address::origin(settings.seerr_host, settings.seerr_port);
    case Setting::method:
        return settings.seerr_auth == 2   ? "API key"
               : settings.seerr_auth == 0 ? "Jellyfin account"
                                          : "Request-server account";
    case Setting::key:
        return settings.seerr_api_key.empty() ? "Not set" : "Saved";
    case Setting::account:
        return g_view.account.empty() ? "Enter…" : g_view.account;
    case Setting::password:
        return g_view.password.empty() ? "Enter…" : "••••••";
    case Setting::connect:
        return g_view.signing_in || g_view.refreshing ? "Connecting…"
               : g_view.authenticated                 ? "Connected"
                                                      : "";
    case Setting::disconnect:
        return "";
    }
    return "";
}

const char *settings_row_hint(Setting row) noexcept
{
    switch (row)
    {
    case Setting::server:
        return "Use a hostname, such as request.example.com, or a local IP and port.";
    case Setting::method:
        return "Use your Jellyfin account, a request-server account, or an API key.";
    case Setting::key:
        return "From Settings > General on your request server. Requests use the server owner's "
               "account.";
    case Setting::account:
        return "The account must be allowed to sign in to your request server.";
    case Setting::password:
        return "Your password is used for sign-in and is never saved.";
    case Setting::connect:
        return "Connect to browse titles and track requests in Discover.";
    case Setting::disconnect:
        return "Remove saved request-server credentials. Your Jellyfin login stays signed in.";
    }
    return "";
}

void draw_settings() noexcept
{
    snapshot();
    text::draw(ui::kContentX, kHeaderY, "Requests", 38, Weight::bold, gfx::palette::text);
    const auto &settings = config::current();
    std::string state;
    gfx::Color colour = gfx::palette::text_dim;
    if (settings.seerr_host.empty())
        state = "Connect Seerr or Jellyseerr to discover and request movies and shows.";
    else if (g_view.signing_in || g_view.refreshing)
        state = "Connecting to your request server…";
    else if (g_view.authenticated)
    {
        state = "Connected to " + (g_view.server_title.empty() ? std::string{"your request server"}
                                                               : g_view.server_title);
        if (!g_view.server_version.empty())
            state += " " + g_view.server_version;
        colour = gfx::palette::success;
    }
    else if (!g_view.connection_error.empty() &&
             (!settings.seerr_api_key.empty() || !settings.seerr_cookie.empty()))
    {
        state = g_view.connection_error;
        colour = gfx::palette::danger;
    }
    else
        state = "Choose how to connect, then enter your details below.";
    text::draw_ellipsized(ui::kContentX, kGridY - 46, ui::kContentWidth, state, 24, Weight::regular,
                          colour);
    const auto rows = settings_rows();
    g_view.settings_row = std::clamp(g_view.settings_row, 0, static_cast<int>(rows.size()) - 1);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
    {
        const int y = kGridY + i * 76;
        const bool focused = i == g_view.settings_row;
        if (focused)
            ui::focus_pill(ui::kContentX - 16, y, 1120, 62, ui::FocusGroup::settings,
                           90 + static_cast<int>(rows.size()));
        const auto ink = focused ? gfx::palette::text : gfx::palette::text_dim;
        ui::control_label(ui::kContentX + 8, y, 560, 62, settings_row_label(rows[i]), 28,
                          Weight::medium, ink);
        const auto value = settings_row_value(rows[i]);
        text::draw_ellipsized(ui::kContentX + 620,
                              text::centered_y(y, 62, value, 24, Weight::regular), 480, value, 24,
                              Weight::regular, ink);
    }
    text::draw_wrapped(ui::kContentX, kGridY + static_cast<int>(rows.size()) * 76 + 20, 1100, 2, 30,
                       settings_row_hint(rows[static_cast<std::size_t>(g_view.settings_row)]), 22,
                       Weight::regular, gfx::palette::text_faint);
    if (!g_view.settings_notice.empty())
        text::draw_ellipsized(ui::kContentX, kGridY + static_cast<int>(rows.size()) * 76 + 102,
                              1100, g_view.settings_notice, 24, Weight::medium,
                              gfx::palette::accent_alt);
    ui::draw_button_hints(ui::kContentRight - 320, kGridY - 52,
                          {{icons::Icon::ps_cross, "Choose"}, {icons::Icon::ps_circle, "Back"}});
}

void handle_settings_input() noexcept
{
    if (g_view.ime_guard > 0)
        return;
    auto &settings = config::current();
    const auto rows = settings_rows();
    g_view.settings_row = std::clamp(g_view.settings_row, 0, static_cast<int>(rows.size()) - 1);
    if (pad::pressed(pad::Button::down))
        g_view.settings_row = std::min(g_view.settings_row + 1, static_cast<int>(rows.size()) - 1);
    else if (pad::pressed(pad::Button::up))
        g_view.settings_row = std::max(g_view.settings_row - 1, 0);
    else
    {
        const auto row = rows[static_cast<std::size_t>(g_view.settings_row)];
        const bool cycle = row == Setting::method &&
                           (pad::pressed(pad::Button::left) || pad::pressed(pad::Button::right));
        if (!pad::pressed(pad::Button::cross) && !cycle)
            return;
        switch (row)
        {
        case Setting::server:
            open_keyboard(Field::host);
            break;
        case Setting::key:
            open_keyboard(Field::api_key);
            break;
        case Setting::account:
            open_keyboard(Field::account);
            break;
        case Setting::password:
            open_keyboard(Field::password);
            break;
        case Setting::method:
            settings.seerr_auth =
                (settings.seerr_auth + (pad::pressed(pad::Button::left) ? 2 : 1)) % 3;
            settings.seerr_cookie.clear();
            settings.seerr_user.clear();
            if (settings.seerr_auth != 2)
                settings.seerr_api_key.clear();
            g_view.account.clear();
            close_settings();
            g_view.settings_notice.clear();
            config::save();
            break;
        case Setting::connect:
        {
            if (g_view.signing_in || g_view.refreshing)
                return;
            if (settings.seerr_host.empty())
            {
                g_view.settings_notice = "Enter your request-server address first.";
                return;
            }
            if (settings.seerr_auth == 2 && settings.seerr_api_key.empty())
            {
                g_view.settings_notice = "Enter your API key first.";
                return;
            }
            if (settings.seerr_auth == 2 ||
                (g_view.account.empty() && !settings.seerr_cookie.empty()))
            {
                refresh_server();
                return;
            }
            if (g_view.account.empty())
            {
                g_view.settings_notice = "Enter your account name first.";
                return;
            }
            const auto connection = connection_snapshot();
            {
                std::lock_guard<std::mutex> guard(g_mutex);
                g_model.signing_in = true;
            }
            const bool jellyfin = settings.seerr_auth == 0;
            const auto account = g_view.account;
            const auto password = g_view.password;
            if (!background::run([connection, jellyfin, account, password]
                                 { job_sign_in(connection, jellyfin, account, password); },
                                 background::Lane::requests))
            {
                std::lock_guard<std::mutex> guard(g_mutex);
                g_model.signing_in = false;
                g_model.notice = "The request worker could not start. Restart SlopFin.";
                ++g_model.notice_epoch;
            }
            close_settings();
            break;
        }
        case Setting::disconnect:
            settings.seerr_cookie.clear();
            settings.seerr_api_key.clear();
            settings.seerr_user.clear();
            close_settings();
            config::save();
            sync_connection();
            g_view.settings_notice = "Disconnected from the request server.";
            break;
        }
    }
}

} // namespace slopfin::seerr
