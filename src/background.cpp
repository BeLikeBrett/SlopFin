/*
 * SlopFin - service-specific background queues.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "background.hpp"

#include "images.hpp"
#include "trace.hpp"

#include <array>
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
struct Worker
{
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    bool started = false;
};
std::array<Worker, static_cast<std::size_t>(slopfin::background::Lane::count)> workers;
std::atomic<int> g_pending{0};

void *loop(void *context) noexcept
{
    auto &worker = *static_cast<Worker *>(context);
    for (;;)
    {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> guard(worker.mutex);
            worker.wake.wait(guard, [&] { return !worker.jobs.empty(); });
            job = std::move(worker.jobs.front());
            worker.jobs.pop_front();
        }
        job();
        g_pending.fetch_sub(1);
    }
    return nullptr;
}

/* Called with the worker mutex held. */
void start_locked(Worker &worker) noexcept
{
    if (worker.started)
        return;
    pthread_attr_t attributes;
    const bool have = pthread_attr_init(&attributes) == 0;
    if (have)
        (void)pthread_attr_setstacksize(&attributes, 2u * 1024u * 1024u);
    void *thread = nullptr;
    const int created = scePthreadCreate(&thread, have ? &attributes : nullptr, loop, &worker,
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
    worker.started = true;
}
} // namespace

namespace slopfin::background
{

bool run(std::function<void()> job, Lane lane) noexcept
{
    const auto index = static_cast<std::size_t>(lane);
    if (index >= workers.size())
        return false;
    auto &worker = workers[index];
    std::lock_guard<std::mutex> guard(worker.mutex);
    start_locked(worker);
    if (!worker.started)
        return false;
    worker.jobs.push_back(std::move(job));
    g_pending.fetch_add(1);
    worker.wake.notify_one();
    return true;
}

int pending() noexcept
{
    return g_pending.load();
}

} // namespace slopfin::background
