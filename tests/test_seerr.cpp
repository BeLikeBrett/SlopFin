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
    assert(request_action_label(title, false) == std::string("Open in Jellyfin"));
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
}
