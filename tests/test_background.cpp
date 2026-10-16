/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "background.hpp"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <string_view>
#include <unistd.h>
namespace slopfin::trace
{
void mark(std::string_view) noexcept
{
}
} // namespace slopfin::trace
namespace slopfin::images
{
std::uint64_t spare_cores() noexcept
{
    return 0;
}
} // namespace slopfin::images
extern "C" int scePthreadCreate(void **handle, const void *, void *(*entry)(void *), void *argument,
                                const char *)
{
    std::thread([=] { entry(argument); }).detach();
    *handle = nullptr;
    return 0;
}
extern "C" int scePthreadSetaffinity(void *, std::uint64_t)
{
    return 0;
}
int main()
{
    using namespace slopfin::background;
    std::mutex mutex;
    std::condition_variable event;
    bool blocked = false, released = false, updated = false, general = false, second = false;
    assert(run(
        [&]
        {
            std::unique_lock lock(mutex);
            blocked = true;
            event.notify_all();
            event.wait(lock, [&] { return released; });
        },
        Lane::requests));
    assert(run(
        [&]
        {
            std::lock_guard lock(mutex);
            second = true;
            event.notify_all();
        },
        Lane::requests));
    assert(run(
        [&]
        {
            std::lock_guard lock(mutex);
            updated = true;
            event.notify_all();
        },
        Lane::updates));
    assert(run(
        [&]
        {
            std::lock_guard lock(mutex);
            general = true;
            event.notify_all();
        }));
    std::unique_lock lock(mutex);
    assert(event.wait_for(lock, std::chrono::seconds(3),
                          [&] { return blocked && updated && general; }));
    assert(!second);
    released = true;
    event.notify_all();
    assert(event.wait_for(lock, std::chrono::seconds(3), [&] { return second; }));
    // Native lanes live until process termination; avoid static teardown beneath their waits.
    _exit(0);
}
