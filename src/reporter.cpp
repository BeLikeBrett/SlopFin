/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "reporter.hpp"
#include "config.hpp"
#include "http.hpp"
#include "images.hpp"
#include "json.hpp"
#include "trace.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <pthread.h>

extern "C"
{
    int scePthreadSetaffinity(void *, std::uint64_t);
    int scePthreadCreate(void **, const void *, void *(*)(void *), void *, const char *);
}
namespace slopfin::reporter
{
namespace
{
using Clock = std::chrono::steady_clock;
constexpr auto kProgressInterval = std::chrono::seconds(10);
constexpr std::size_t kMaxEvents = 64;
struct Identity
{
    std::string host, token, user, device;
    int port = 0;
    bool operator==(const Identity &) const = default;
};
Identity identity()
{
    const auto &s = config::current();
    return {s.host, s.token, s.user_id, s.device_id, s.port};
}
bool matches_current(const Identity &account)
{
    const auto &s = config::current();
    return account.host == s.host && account.token == s.token && account.user == s.user_id &&
           account.device == s.device_id && account.port == s.port;
}
struct Event
{
    const char *endpoint;
    std::string body;
    Identity account;
    std::uint64_t sequence, generation;
    bool disposable = false;
};
std::mutex g_mutex;
std::condition_variable g_changed;
std::deque<Event> g_queue;
Session g_session;
Identity g_identity;
bool g_active = false, g_paused = false, g_inflight = false;
double g_position = 0;
Clock::time_point g_next_progress;
std::uint64_t g_next_sequence = 1, g_settled = 0, g_failed = 0, g_generation = 0;
std::atomic<bool> g_started{false};
bool g_stop = false;

std::string body(const Session &session, double position, bool paused)
{
    long long ticks = 0;
    if (std::isfinite(position) && position > 0)
    {
        const long double scaled = static_cast<long double>(position) * 10000000.0L;
        ticks = scaled >= static_cast<long double>(std::numeric_limits<long long>::max())
                    ? std::numeric_limits<long long>::max()
                    : std::llround(scaled);
    }
    std::string out = "{\"ItemId\":\"" + json::escape(session.item_id) + "\"";
    out += ",\"MediaSourceId\":\"" + json::escape(session.media_source_id) + "\"";
    out += ",\"PlaySessionId\":\"" + json::escape(session.play_session) + "\"";
    out += ",\"PlayMethod\":\"" + json::escape(session.play_method) + "\"";
    out += ",\"PositionTicks\":" + std::to_string(ticks);
    out += ",\"IsPaused\":" + std::string(paused ? "true" : "false");
    out += ",\"CanSeek\":true";
    if (session.audio_index >= 0)
        out += ",\"AudioStreamIndex\":" + std::to_string(session.audio_index);
    out += ",\"SubtitleStreamIndex\":" + std::to_string(session.subtitle_index) + "}";
    return out;
}
void discard_locked()
{
    ++g_generation;
    g_active = false;
    g_settled = g_next_sequence - 1;
    g_failed = g_settled;
    g_queue.clear();
    g_changed.notify_all();
}
void enqueue(const char *endpoint, double position, bool paused, bool disposable = false)
{
    // Never replace an in-flight event or cross a begin/stop boundary.
    if (disposable && g_queue.size() > (g_inflight ? 1u : 0u) && g_queue.back().disposable &&
        g_queue.back().generation == g_generation)
    {
        g_queue.back().body = body(g_session, position, paused);
        return;
    }
    if (disposable && g_queue.size() >= kMaxEvents - 1)
        return; // Every accepted begin always reserves room for its final stop.
    g_queue.push_back({endpoint, body(g_session, position, paused), g_identity, g_next_sequence++,
                       g_generation, disposable});
    g_changed.notify_all();
}
http::Response post(const Event &event)
{
    std::string auth = "Authorization: MediaBrowser Client=\"SlopFin\", Device=\"PlayStation 5\"";
    auth += ", DeviceId=\"" + event.account.device + "\", Version=\"2.00\", Token=\"" +
            event.account.token + "\"";
    return http::post(event.account.host, event.account.port, event.endpoint, {auth}, event.body,
                      "application/json");
}
bool transient(int status)
{
    return status == 0 || status == 408 || status == 425 || status == 429 || status >= 500;
}
void *run(void *) noexcept
{
    std::unique_lock<std::mutex> lock(g_mutex);
    unsigned failures = 0;
    Clock::time_point retry{};
    auto generation = g_generation;
    for (;;)
    {
        if (g_stop)
            break;
        if (generation != g_generation)
        {
            generation = g_generation;
            failures = 0;
            retry = {};
        }
        auto now = Clock::now();
        if (g_active && now >= g_next_progress)
        {
            enqueue("/Sessions/Playing/Progress", g_position, g_paused, true);
            g_next_progress = now + kProgressInterval;
        }
        if (g_queue.empty() || now < retry)
        {
            auto wake = g_active ? g_next_progress : Clock::time_point::max();
            if (!g_queue.empty())
                wake = std::min(wake, retry);
            if (wake == Clock::time_point::max())
                g_changed.wait(lock);
            else
                g_changed.wait_until(lock, wake);
            continue;
        }
        const Event event = g_queue.front();
        g_inflight = true;
        lock.unlock();
        const auto response = post(event);
        lock.lock();
        g_inflight = false;
        if (event.generation != g_generation || g_queue.empty() ||
            g_queue.front().sequence != event.sequence)
        {
            failures = 0;
            retry = {};
            g_changed.notify_all();
            continue;
        }
        if (!response.ok() && transient(response.status))
        {
            ++failures;
            retry = Clock::now() + std::chrono::milliseconds(250u << std::min(4u, failures - 1));
            if (failures == 1)
                trace::mark("reporter: retrying playback update");
        }
        else
        {
            if (!response.ok())
            {
                g_failed = event.sequence;
                trace::mark("reporter: playback update rejected (HTTP " +
                            std::to_string(response.status) + ")");
            }
            g_settled = event.sequence;
            g_queue.pop_front();
            failures = 0;
            retry = {};
        }
        g_changed.notify_all();
    }
    return nullptr;
}
void ensure_thread() noexcept
{
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true))
        return;
    pthread_attr_t attributes;
    const bool have = pthread_attr_init(&attributes) == 0;
    if (have)
        (void)pthread_attr_setstacksize(&attributes, 256u * 1024u);
    void *thread = nullptr;
    if (scePthreadCreate(&thread, have ? &attributes : nullptr, run, nullptr, "slopfin-report") !=
        0)
        g_started = false;
    else if (const auto spare = images::spare_cores(); spare != 0)
        (void)scePthreadSetaffinity(thread, spare);
    if (have)
        (void)pthread_attr_destroy(&attributes);
}
} // namespace
void invalidate() noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    discard_locked();
}
void begin(const Session &session, double position) noexcept
{
    ensure_thread();
    const auto account = identity();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!(account == g_identity))
        discard_locked();
    if (g_active)
        enqueue("/Sessions/Playing/Stopped", g_position, g_paused);
    g_active = false;
    g_identity = account;
    if (account.token.empty() || account.user.empty() || g_queue.size() > kMaxEvents - 2)
    {
        if (!account.token.empty())
            trace::mark("reporter: offline session queue is full");
        return;
    }
    g_session = session;
    g_active = true;
    g_position = position;
    g_paused = false;
    g_next_progress = Clock::now() + kProgressInterval;
    enqueue("/Sessions/Playing", position, false);
}
void update(double position, bool paused) noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_active)
        return;
    if (!matches_current(g_identity))
    {
        discard_locked();
        return;
    }
    g_position = position;
    if (g_paused != paused)
    {
        g_paused = paused;
        // Publish pause immediately, replacing a waiting heartbeat with the
        // latest pause state. A final-stop slot remains reserved throughout.
        enqueue("/Sessions/Playing/Progress", position, paused, true);
        g_next_progress = Clock::now() + kProgressInterval;
    }
}
void end(double position) noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_active)
        return;
    if (!matches_current(g_identity))
    {
        discard_locked();
        return;
    }
    g_active = false;
    enqueue("/Sessions/Playing/Stopped", position, g_paused);
}
bool flush(unsigned timeout_ms) noexcept
{
    ensure_thread();
    std::unique_lock<std::mutex> lock(g_mutex);
    const auto target = g_next_sequence - 1;
    const auto generation = g_generation;
    const auto previous = g_settled;
    g_changed.notify_all();
    const bool settled =
        g_changed.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [&] { return g_settled >= target || g_generation != generation; });
    return settled && generation == g_generation && !(g_failed > previous && g_failed <= target) &&
           (target == 0 || g_failed != target);
}
} // namespace slopfin::reporter
