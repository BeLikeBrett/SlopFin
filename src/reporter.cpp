/*
 * SlopFin - playback progress reporting.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "reporter.hpp"

#include "jellyfin.hpp"
#include "images.hpp"
#include "json.hpp"
#include "trace.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <pthread.h>

extern "C"
{
    int scePthreadSetaffinity(void *thread, std::uint64_t mask);
    int sceKernelUsleep(std::uint32_t microseconds);
    int scePthreadCreate(void **thread, const void *attr, void *(*entry)(void *), void *argument,
                         const char *name);
}

namespace slopfin::reporter
{
namespace
{
constexpr double kProgressIntervalSeconds = 10.0;
constexpr std::uint32_t kTickMicroseconds = 250000;

struct Event
{
    const char *endpoint;
    std::string body;
};

std::mutex g_mutex;
std::deque<Event> g_queue;
Session g_session;
bool g_active = false;
double g_position = 0.0;
bool g_paused = false;
bool g_paused_sent = false;
double g_since_report = 0.0;
std::atomic<bool> g_started{false};

std::string body(const Session &session, double position, bool paused)
{
    const auto ticks = static_cast<long long>(position * 10000000.0);
    std::string out = "{\"ItemId\":\"" + json::escape(session.item_id) + "\"";
    out += ",\"MediaSourceId\":\"" + json::escape(session.media_source_id) + "\"";
    out += ",\"PlaySessionId\":\"" + json::escape(session.play_session) + "\"";
    out += ",\"PlayMethod\":\"" + json::escape(session.play_method) + "\"";
    out += ",\"PositionTicks\":" + std::to_string(ticks < 0 ? 0 : ticks);
    out += ",\"IsPaused\":" + std::string{paused ? "true" : "false"};
    out += ",\"CanSeek\":true";
    if (session.audio_index >= 0)
        out += ",\"AudioStreamIndex\":" + std::to_string(session.audio_index);
    out += ",\"SubtitleStreamIndex\":" + std::to_string(session.subtitle_index);
    out += "}";
    return out;
}

void *run(void *) noexcept
{
    for (;;)
    {
        (void)sceKernelUsleep(kTickMicroseconds);
        std::deque<Event> pending;
        {
            std::lock_guard<std::mutex> guard(g_mutex);
            if (g_active)
            {
                g_since_report += kTickMicroseconds / 1000000.0;
                if (g_since_report >= kProgressIntervalSeconds || g_paused != g_paused_sent)
                {
                    g_queue.push_back(
                        {"/Sessions/Playing/Progress", body(g_session, g_position, g_paused)});
                    g_since_report = 0.0;
                    g_paused_sent = g_paused;
                }
            }
            pending.swap(g_queue);
        }
        for (const Event &event : pending)
        {
            if (!jellyfin::post_json(event.endpoint, event.body))
                trace::mark(std::string{"reporter: "} + event.endpoint + " was not accepted");
        }
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
        g_started.store(false);
    else if (const auto spare = images::spare_cores(); spare != 0)
        (void)scePthreadSetaffinity(thread, spare);
    if (have)
        (void)pthread_attr_destroy(&attributes);
}
} // namespace

void begin(const Session &session, double position) noexcept
{
    ensure_thread();
    std::lock_guard<std::mutex> guard(g_mutex);
    g_session = session;
    g_active = true;
    g_position = position;
    g_paused = false;
    g_paused_sent = false;
    g_since_report = 0.0;
    g_queue.push_back({"/Sessions/Playing", body(session, position, false)});
}

void update(double position, bool paused) noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_active)
        return;
    g_position = position;
    g_paused = paused;
}

void end(double position) noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_active)
        return;
    g_active = false;
    g_queue.push_back({"/Sessions/Playing/Stopped", body(g_session, position, false)});
}

} // namespace slopfin::reporter
