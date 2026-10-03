/*
 * SlopFin - background jobs for the profile and dashboard screens.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "background.hpp"

#include "images.hpp"
#include "trace.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <pthread.h>

extern "C"
{
    int scePthreadCreate(void **thread, const void *attr, void *(*entry)(void *), void *argument,
                         const char *name);
    int scePthreadSetaffinity(void *thread, std::uint64_t mask);
}

namespace
{
std::mutex g_mutex;
std::condition_variable g_wake;
std::deque<std::function<void()>> g_jobs;
std::atomic<int> g_pending{0};
bool g_started = false;

void *loop(void *) noexcept
{
    for (;;)
    {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> guard(g_mutex);
            g_wake.wait(guard, [] { return !g_jobs.empty(); });
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        job();
        g_pending.fetch_sub(1);
    }
    return nullptr;
}

/* Called with g_mutex held. */
void start_locked() noexcept
{
    if (g_started)
        return;
    pthread_attr_t attributes;
    const bool have = pthread_attr_init(&attributes) == 0;
    if (have)
        (void)pthread_attr_setstacksize(&attributes, 2u * 1024u * 1024u);
    void *thread = nullptr;
    const int created = scePthreadCreate(&thread, have ? &attributes : nullptr, loop, nullptr,
                                         "slopfin-background");
    if (have)
        (void)pthread_attr_destroy(&attributes);
    if (created != 0)
    {
        slopfin::trace::mark("background: thread FAILED");
        return;
    }
    if (const std::uint64_t spare = slopfin::images::spare_cores(); spare != 0)
        (void)scePthreadSetaffinity(thread, spare);
    g_started = true;
}
} // namespace

namespace slopfin::background
{

void run(std::function<void()> job) noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    start_locked();
    if (!g_started)
        return;
    g_jobs.push_back(std::move(job));
    g_pending.fetch_add(1);
    g_wake.notify_one();
}

int pending() noexcept
{
    return g_pending.load();
}

} // namespace slopfin::background
