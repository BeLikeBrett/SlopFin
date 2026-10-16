/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <cassert>
#include <deque>
#include <functional>
#include "../src/seerr.cpp"

namespace
{
slopfin::config::Settings settings;
std::deque<std::function<void()>> jobs;
int saved = 0, calls = 0;
bool cancelled = false;
slopfin::pad::Button pressed_button = slopfin::pad::Button::count;
std::string last_post;
std::function<slopfin::http::Response(std::string_view, std::string_view,
                                      const std::vector<std::string> &, std::string_view)>
    handler;
void drain()
{
    int count = 0;
    while (!jobs.empty())
    {
        assert(++count < 50);
        auto job = std::move(jobs.front());
        jobs.pop_front();
        job();
    }
}
} // namespace
namespace slopfin::config
{
Settings &current() noexcept
{
    return settings;
}
void save() noexcept
{
    ++saved;
}
} // namespace slopfin::config
namespace slopfin::background
{
bool run(std::function<void()> job, Lane lane) noexcept
{
    assert(lane == Lane::requests);
    jobs.push_back(std::move(job));
    return true;
}
} // namespace slopfin::background
namespace slopfin::ime
{
bool busy() noexcept
{
    return false;
}
void request(const std::string &, const std::string &, const std::string &, Mode, int) noexcept
{
    assert(false && "Unexpected keyboard request");
}
bool take_result(std::string &) noexcept
{
    return false;
}
bool take_cancelled() noexcept
{
    bool out = cancelled;
    cancelled = false;
    return out;
}
} // namespace slopfin::ime
namespace slopfin::pad
{
bool pressed(Button button) noexcept
{
    return button == pressed_button;
}
} // namespace slopfin::pad
namespace slopfin::http
{
Response get(std::string_view host, int, std::string_view path,
             const std::vector<std::string> &headers) noexcept
{
    assert(host == settings.seerr_host);
    ++calls;
    return handler("GET", path, headers, {});
}
Response post(std::string_view host, int, std::string_view path,
              const std::vector<std::string> &headers, std::string_view body,
              std::string_view) noexcept
{
    assert(host == settings.seerr_host);
    ++calls;
    last_post = body;
    return handler("POST", path, headers, body);
}
Response send(std::string_view method, std::string_view host, int, std::string_view path,
              const std::vector<std::string> &headers, std::string_view body,
              std::string_view) noexcept
{
    assert(host == settings.seerr_host);
    ++calls;
    return handler(method, path, headers, body);
}
std::string url_encode(std::string_view s) noexcept
{
    return std::string(s);
}
} // namespace slopfin::http
int main()
{
    using namespace slopfin;
    using namespace slopfin::seerr;
    assert(server_address::parse("192.168.1.20", 5055).port == 5055);
    assert(server_address::parse("request.example.com", 5055).port == 443);
    assert(server_address::parse("192.168.1.20:8080", 5055).port == 8080);
    settings.seerr_host = "https://requests.example.com";
    settings.seerr_auth = 2;
    settings.seerr_api_key = "test-key";
    bool signed_in = false;
    handler = [&](std::string_view method, std::string_view path,
                  const std::vector<std::string> &headers, std::string_view) -> http::Response
    {
        if (method == "POST")
        {
            assert(path == "/api/v1/auth/jellyfin" && headers.empty());
            signed_in = true;
            return {200,
                    "{\"id\":7}",
                    {"Set-Cookie: other=ignored",
                     "set-cookie: connect.sid=s%3Atest; Path=/; HttpOnly"}};
        }
        if (path == "/api/v1/status")
            return {200, "{\"version\":\"3.4.1\"}", {}};
        if (path == "/api/v1/settings/public")
            return {200, "{\"applicationTitle\":\"Requests\"}", {}};
        assert(headers == (signed_in ? std::vector<std::string>{"Cookie: connect.sid=s%3Atest"}
                                     : std::vector<std::string>{"X-Api-Key: test-key"}));
        if (path == "/api/v1/auth/me")
            return {200, "{\"id\":7}", {}};
        if (path.starts_with("/api/v1/request?"))
        {
            assert(path.find("requestedBy=7") != std::string_view::npos);
            return {
                200,
                R"({"results":[{"id":1,"status":2,"media":{"tmdbId":10,"mediaType":"tv","status":4,"downloadStatus":[{"size":2000,"sizeLeft":1000}]}},{"id":2,"status":2,"media":{"tmdbId":11,"mediaType":"movie","status":3,"downloadStatus":[]}}]})",
                {}};
        }
        if (path.starts_with("/api/v1/tv/") || path.starts_with("/api/v1/movie/"))
            return {200, R"({"name":"A title","posterPath":"/poster.jpg"})", {}};
        if (path.starts_with("/api/v1/search?"))
            return {200, R"({"results":[{"id":10,"name":"A show","mediaType":"tv"}]})", {}};
        assert(false);
        return {};
    };
    refresh_server();
    drain();
    snapshot();
    assert(g_view.authenticated && g_model.user_id == 7);
    refresh_requests();
    refresh_requests();
    assert(jobs.size() == 1);
    drain();
    snapshot();
    assert(g_view.requests.size() == 2);
    assert(g_view.requests[0].downloading && g_view.requests[0].progress == 0.5);
    assert(!g_view.requests[1].downloading && g_view.requests[0].name == "A title");
    settings.seerr_auth = 0;
    auto connection = connection_snapshot();
    auto before = calls;
    job_sign_in(connection, true, "viewer", "secret");
    assert(calls == before + 1 && settings.seerr_cookie.empty());
    assert(last_post == R"({"username":"viewer","password":"secret"})");
    snapshot();
    assert(saved == 1 && settings.seerr_cookie == "s%3Atest" && settings.seerr_api_key.empty());
    drain();
    snapshot();
    assert(g_view.authenticated);
    // A queued query for another server must not run or populate the new account.
    start_search("old");
    before = calls;
    settings.seerr_host = "https://another.example.com";
    sync_connection();
    drain();
    assert(calls == before && g_model.results.empty());
    // A completed old sign-in must not be applied after changing servers.
    g_model.pending_cookie = "stale";
    g_model.pending_user = "stale";
    settings.seerr_host = "https://third.example.com";
    snapshot();
    assert(settings.seerr_cookie == "s%3Atest" && g_model.pending_cookie.empty());
    g_field = Field::password;
    cancelled = true;
    poll();
    assert(g_field == Field::none && g_view.ime_guard == 2);
    const auto available = json::parse(
        R"({"id":12,"name":"A show","mediaType":"tv","mediaInfo":{"status":5,"jellyfinMediaId":"0123456789abcdef0123456789abcdef"}})");
    const auto title = read_title(*available);
    assert(title.jellyfin_id == "0123456789abcdef0123456789abcdef");
    const auto invalid = json::parse(R"({"jellyfinMediaId":"../../outside"})");
    assert(jellyfin_id(*invalid).empty());
    settings.seerr_auth = 2;
    assert(settings_rows().size() == 5);
    settings.seerr_auth = 0;
    assert(settings_rows().size() == 6);
    // Server errors keep a recovery action and reject invalid session responses.
    assert(response_error({401, {}, {}}, "Search").find("Sign in again") != std::string::npos);
    const auto doc = json::parse(
        R"({"id":3,"media":{"mediaType":"tv","status":4,"downloadStatus":[{"size":100,"sizeLeft":150}]}})");
    assert(!read_request(*doc).downloading);
    // Actual season state and active requests determine what can be submitted.
    const auto detail = json::parse(R"({"seasons":[
        {"seasonNumber":0,"episodeCount":4,"name":"Specials"},
        {"seasonNumber":1,"episodeCount":10},{"seasonNumber":2,"episodeCount":8},
        {"seasonNumber":3,"episodeCount":6},{"seasonNumber":4,"episodeCount":0}],
        "mediaInfo":{"seasons":[{"seasonNumber":1,"status":5,"status4k":1}],
        "requests":[{"status":1,"is4k":false,"seasons":[{"seasonNumber":2,"status":1}]},
                    {"status":2,"is4k":true,"seasons":[{"seasonNumber":1,"status":2}]}]}})");
    request::Options options;
    options.loaded = options.advanced = options.allow4k = true;
    options.seasons = request::read_seasons(*detail);
    assert(options.seasons.size() == 4);
    assert(!options.seasons[1].selectable(false) && !options.seasons[2].selectable(false));
    assert(options.seasons[3].selectable(false) && options.seasons[0].selectable(false));
    assert(!options.seasons[1].selectable(true) && options.seasons[2].selectable(true));
    options.servers = {{9, "TV", false, true, 4, "/tv", {{4, "1080p"}, {8, "Any"}}},
                       {10, "4K TV", true, true, 19, "/tv4k", {{19, "2160p"}}}};
    const auto body = json::parse(request::payload("tv", 123, false, options, {1, 2, 3}, 0, 0));
    assert(body && body->num("serverId") == 9 && body->num("profileId") == 4);
    assert(body->str("rootFolder") == "/tv" && body->find("seasons")->size() == 1);
    assert(body->find("seasons")->at(0)->number == 3);
    assert(request::payload("tv", 123, false, options, {}, 0, 0).empty());
    assert(request::payload("tv", 123, true, options, {3}, 0, 0).empty());
    const auto fourk = json::parse(request::payload("tv", 123, true, options, {2, 3}, 1, 0));
    assert(fourk && fourk->flag("is4k") && fourk->num("profileId") == 19);
    options.advanced = false;
    const auto limited = json::parse(request::payload("movie", 12, false, options, {}, 0, 0));
    assert(limited && !limited->find("profileId") && !limited->find("serverId"));
    options.partial = false;
    const auto all = json::parse(request::payload("tv", 123, false, options, {3}, 0, 0));
    assert(all->str("seasons") == "all");
    assert(request::advanced(2) && request::advanced(8192) && !request::advanced(32));
    assert(request::can_request(524288, false, false) == false);
    assert(request::can_request(524288, true, false) && !request::can_request(524288, true, true));
    assert(request::can_request(4096, true, true) && !request::can_request(4096, false, true));
    // Media selection applies to catalog results; requests remain unfiltered.
    Title movie, series;
    movie.media_type = "movie";
    movie.name = "Movie";
    series.media_type = "tv";
    series.name = "Series";
    series.availability = Availability::available;
    series.requested = true;
    g_view.results = {movie, series};
    g_view.media_filter = 2;
    assert(visible_results() == std::vector<int>{1});
    g_view.media_filter = 0;
    assert(visible_results() == (std::vector<int>{0, 1}));
    g_view.media_filter = 0;
    settings.seerr_auth = 2;
    settings.seerr_api_key = "test-key";
    sync_connection();
    g_model.permissions = g_view.permissions = 2;
    g_model.user_id = 7;
    g_model.authenticated = g_view.authenticated = true;
    handler = [](std::string_view method, std::string_view path, const auto &,
                 std::string_view) -> http::Response
    {
        if (path == "/api/v1/tv/123")
            return {
                200,
                R"({"seasons":[{"seasonNumber":1,"episodeCount":8},{"seasonNumber":2,"episodeCount":10}],"mediaInfo":{"seasons":[{"seasonNumber":1,"status":5}]}})",
                {}};
        if (path == "/api/v1/service/sonarr")
            return {
                200,
                R"([{"id":0,"name":"TV","is4k":false,"isDefault":true,"activeProfileId":4,"activeDirectory":"/tv"}])",
                {}};
        if (path == "/api/v1/service/sonarr/0")
            return {200, R"({"profiles":[{"id":4,"name":"1080p"},{"id":8,"name":"HEVC"}]})", {}};
        if (method == "POST" && path == "/api/v1/request")
            return {201, R"({"status":1})", {}};
        if (path.starts_with("/api/v1/request?"))
            return {200, R"({"results":[]})", {}};
        return {404, {}, {}};
    };
    g_view.sheet = {};
    g_view.sheet.tmdb_id = 123;
    g_view.sheet.media_type = "tv";
    g_view.sheet.name = "A show";
    load_options();
    drain();
    snapshot();
    assert(g_view.options.loaded && g_view.options.error.empty());
    assert(g_view.server_index == 0 && g_view.seasons == std::vector<int>{2});
    assert(g_view.profile_index == 0 && profile_label() == "1080p (default)");
    change_sheet_row({SheetKind::profile}, 1);
    assert(g_view.profile_index == 1 && profile_label() == "HEVC");
    change_sheet_row({SheetKind::profile}, 1);
    assert(g_view.profile_index == 0);
    change_sheet_row({SheetKind::profile}, -1);
    assert(g_view.profile_index == 1);
    change_sheet_row({SheetKind::season, 0}, 1);
    assert(g_view.seasons == std::vector<int>{2});
    change_sheet_row({SheetKind::season, 1}, 1);
    assert(g_view.seasons.empty());
    change_sheet_row({SheetKind::season, 1}, 1);
    submit(g_view.sheet);
    drain();
    const auto submitted = json::parse(last_post);
    assert(submitted && submitted->num("serverId", -1) == 0 && submitted->num("profileId") == 8);
    assert(submitted->find("seasons")->size() == 1 &&
           submitted->find("seasons")->at(0)->number == 2);
    assert(!g_model.submitting);
    // A late options response must not replace a newer title's selection.
    const auto epoch = g_model.options_epoch;
    job_options(connection_snapshot(), g_view.sheet, g_model.options_wanted - 1, 2, true);
    assert(g_model.options_epoch == epoch);
    assert(catalog_path("", 1, 2, 3).find("sortBy=vote_average.desc&voteCountGte=200") !=
           std::string::npos);
    assert(catalog_path("", 2, 3, 1).find("firstAirDateLte=") != std::string::npos);
    assert(catalog_path("", 2, 4, 1) == "/api/v1/discover/tv/upcoming?page=1");
    assert(catalog_path("term", 0, 0, 4) == "/api/v1/search?query=term&page=4");
    Request own;
    own.id = 10;
    own.requested_by_id = 7;
    own.approval = 1;
    g_view.requests = {own};
    g_view.sheet_from_requests = true;
    g_view.sheet_request_id = 10;
    g_view.permissions = 32;
    g_view.user_id = 7;
    assert(can_cancel());
    g_view.requests[0].approval = 2;
    assert(!can_cancel());
    g_view.permissions = 16;
    assert(can_cancel());
    g_view.media_filter = 2;
    assert(visible_requests() == std::vector<int>{0});
    bool deleted = false;
    handler = [&](std::string_view method, std::string_view path, const auto &,
                  std::string_view) -> http::Response
    {
        if (method == "DELETE")
        {
            assert(path == "/api/v1/request/10");
            deleted = true;
            return {204, {}, {}};
        }
        if (path == "/api/v1/tv/123/season/2")
            return {
                200,
                R"({"episodes":[{"episodeNumber":1,"name":"Pilot","overview":"Description","airDate":"2026-01-01"}]})",
                {}};
        if (path.starts_with("/api/v1/request?"))
            return {200, R"({"results":[]})", {}};
        return {404, {}, {}};
    };
    cancel_request();
    drain();
    snapshot();
    assert(deleted && !g_view.submitting && g_view.requests.empty());
    open_episodes(2);
    drain();
    snapshot();
    assert(!g_view.episodes.loading && g_view.episodes.items.size() == 1);
    assert(g_view.episodes.items[0].name == "Pilot");
    // Switching seasons before a queued result arrives keeps only the latest response.
    open_episodes(1);
    open_episodes(2);
    drain();
    snapshot();
    assert(g_view.episodes.error.empty() && g_view.episodes.items[0].number == 1);
    handler = [](std::string_view, std::string_view path, const auto &,
                 std::string_view) -> http::Response
    {
        if (path == "/api/v1/request")
            return {201, R"({"status":1})", {}};
        return {200, R"({"results":[]})", {}};
    };
    const auto fourk_only = json::parse(
        R"({"id":88,"mediaType":"movie","title":"4K title","mediaInfo":{"status":1,"status4k":5}})");
    const auto fourk_title = read_title(*fourk_only);
    assert(fourk_title.availability == Availability::unknown &&
           fourk_title.availability4k == Availability::available && fourk_title.requested);
    g_view.results = {fourk_title};
    g_view.media_filter = 0;
    assert(visible_results() == std::vector<int>{0});
    Title requested4k;
    requested4k.tmdb_id = 123;
    requested4k.media_type = "tv";
    g_model.results = {requested4k};
    job_request(connection_snapshot(), requested4k,
                R"({"mediaType":"tv","mediaId":123,"is4k":true,"seasons":[2]})", true);
    assert(g_model.results[0].availability == Availability::unknown);
    assert(g_model.results[0].availability4k == Availability::pending);
    // Existing requests expose their saved profile and a single contextual action.
    g_view = {};
    g_view.authenticated = true;
    g_view.permissions = 2;
    g_view.user_id = 7;
    g_view.sheet.tmdb_id = 44;
    g_view.sheet.media_type = "movie";
    g_view.sheet_open = true;
    g_view.options.loaded = true;
    g_view.options.status = 3;
    Request active;
    active.id = 77;
    active.tmdb_id = 44;
    active.media_type = "movie";
    active.approval = 2;
    active.requested_by_id = 7;
    active.profile_name = "HD HEVC";
    g_view.requests = {active};
    assert(sheet_locked() && sheet_rows().empty());
    assert(profile_label() == "HD HEVC" && sheet_action() == "Cancel request");
    const int profile_before = g_view.profile_index;
    change_sheet_row({SheetKind::profile}, 1);
    assert(g_view.profile_index == profile_before);
    assert(!sheet_can_submit());
    assert(request::payload("movie", 44, false, g_view.options, {}, -1, -1).empty());
    pressed_button = pad::Button::cross;
    const int before_calls = calls;
    sheet_input();
    assert(g_view.cancel_confirm && calls == before_calls);
    g_view.cancel_confirm = false;
    g_view.permissions = 32;
    assert(sheet_action() == "Requested" && !sheet_action_enabled());
    sheet_input();
    assert(g_view.sheet_error.empty() && calls == before_calls);
    g_view.options.status = 5;
    g_view.options.jellyfin_id = std::string(32, 'a');
    assert(sheet_action() == "Open in Jellyfin" && sheet_action_enabled());
    sheet_input();
    assert(g_open_item == std::string(32, 'a') && calls == before_calls);
    g_view.sheet_from_requests = true;
    g_view.sheet_request_id = active.id;
    g_view.requests[0].approval = 4;
    assert(profile_label() == "HD HEVC");
    g_view.sheet.jellyfin_id = std::string(32, 'b');
    g_view.options.jellyfin_id.clear();
    assert(effective_title().jellyfin_id == std::string(32, 'b'));
    g_view.is4k = true;
    assert(effective_title().jellyfin_id.empty());
    pressed_button = pad::Button::count;
    // Clear Data uses Seerr's internal media ID and never its file-deletion route.
    g_view = {};
    g_view.authenticated = true;
    g_view.options.loaded = true;
    g_view.options.media_id = 901;
    g_view.options.status = 3;
    g_view.sheet.tmdb_id = 44;
    g_view.sheet.media_type = "movie";
    g_view.sheet_open = true;
    g_view.permissions = 32;
    assert(!can_clear_data());
    g_view.permissions = 8192;
    assert(!can_clear_data());
    g_view.permissions = 16;
    assert(can_clear_data());
    g_view.requests = {active};
    g_view.requests[0].requested_by = "Fixture user";
    g_view.requests[0].approval = 4;
    assert(recorded_request()->requested_by == "Fixture user");
    assert(profile_label() == "HD HEVC");
    int clear_calls = 0;
    handler = [&](std::string_view method, std::string_view path, const auto &,
                  std::string_view) -> http::Response
    {
        assert(path.find("/file") == std::string_view::npos);
        if (method == "DELETE")
        {
            assert(path == "/api/v1/media/901");
            ++clear_calls;
            return {204, {}, {}};
        }
        return {200, R"({"totalPages":1,"results":[]})", {}};
    };
    pressed_button = pad::Button::right;
    sheet_input();
    assert(g_view.clear_focused && clear_calls == 0);
    pressed_button = pad::Button::cross;
    sheet_input();
    assert(g_view.clear_confirm && clear_calls == 0 && jobs.empty());
    pressed_button = pad::Button::circle;
    sheet_input();
    assert(!g_view.clear_confirm && clear_calls == 0);
    pressed_button = pad::Button::cross;
    sheet_input();
    assert(g_view.clear_confirm);
    sheet_input();
    assert(!g_view.sheet_open && clear_calls == 0 && !jobs.empty());
    drain();
    snapshot();
    assert(clear_calls == 1 && !g_view.submitting && g_view.requests.empty());
    g_view.options.media_id = 0;
    assert(!can_clear_data());
    pressed_button = pad::Button::count;
    // Five upstream pages form a longer browsable page, without duplicate cards.
    handler = [](std::string_view, std::string_view path, const auto &,
                 std::string_view) -> http::Response
    {
        const auto at = path.find("page=");
        const int page =
            at == std::string_view::npos ? 1 : std::atoi(std::string(path.substr(at + 5)).c_str());
        return {200,
                "{\"totalPages\":12,\"results\":[{\"id\":" + std::to_string(page * 10) +
                    ",\"mediaType\":\"movie\",\"title\":\"Title\"},{\"id\":7777,\"mediaType\":"
                    "\"movie\",\"title\":\"Repeated\"}]}",
                {}};
    };
    long generation = ++g_model.search_wanted;
    int catalog_calls = calls;
    job_search(connection_snapshot(), "", generation, 1, 1, 1);
    assert(calls == catalog_calls + 1 && g_model.search_loading && !jobs.empty());
    drain();
    assert(calls == catalog_calls + 5 && g_model.results.size() == 6);
    assert(g_model.page == 1 && g_model.pages == 3 && !g_model.search_loading);
    catalog_calls = calls;
    generation = ++g_model.search_wanted;
    job_search(connection_snapshot(), "", generation, 1, 1, 3);
    drain();
    assert(calls == catalog_calls + 2 && g_model.results.size() == 3 && g_model.page == 3);
    g_view.sheet_open = false;
    g_view.ime_guard = 0;
    g_view.mode = 0;
    g_view.media_filter = g_view.sort = 0;
    pressed_button = pad::Button::l3;
    handle_input();
    assert(g_view.media_filter == 1 && g_view.toolbar == 1);
    pressed_button = pad::Button::r3;
    handle_input();
    assert(g_view.sort == 1 && g_view.toolbar == 2);
    drain();
    pressed_button = pad::Button::count;
    g_view.mode = 1;
    const int sort_before = g_view.sort;
    pressed_button = pad::Button::r3;
    handle_input();
    assert(g_view.sort == sort_before && jobs.empty());
    pressed_button = pad::Button::count;
    g_view.mode = 0;
    g_view.media_filter = 1;
    g_view.field_focused = false;
    Title focus;
    focus.tmdb_id = 20;
    focus.media_type = "movie";
    g_view.results = {focus};
    g_view.grid_index = 0;
    Title inserted = focus;
    inserted.tmdb_id = 10;
    g_model.results = {inserted, focus};
    ++g_model.search_epoch;
    snapshot();
    assert(g_view.grid_index == 1);
}
