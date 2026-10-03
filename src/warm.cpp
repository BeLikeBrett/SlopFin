/*
 * SlopFin - warming every poster in the library.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "warm.hpp"

#include "images.hpp"
#include "jellyfin.hpp"
#include "trace.hpp"

#include <atomic>
#include <cstdint>
#include <pthread.h>
#include <string>

extern "C"
{
    int scePthreadCreate(void **thread, const void *attr, void *(*entry)(void *), void *argument,
                         const char *name);
    int scePthreadSetaffinity(void *thread, std::uint64_t mask);
}

namespace
{
std::atomic<bool> g_started{false};
int g_poster_height = 0;
int g_season_height = 0;

/* Small pages: the catalogue is parsed on the console, and one request for
   the whole library was 630 KiB of JSON and three seconds of server time. */
constexpr int kPage = 100;

int walk(const char *types, int height) noexcept
{
    int queued = 0;
    int total = 0;
    for (int start = 0;; start += kPage)
    {
        const std::vector<slopfin::jellyfin::Item> page =
            slopfin::jellyfin::catalogue(types, start, kPage, &total);
        for (const slopfin::jellyfin::Item &item : page)
        {
            if (item.image_tag.empty())
                continue;
            slopfin::images::warm(item.id, item.image_tag, height);
            ++queued;
        }
        if (page.empty() || start + kPage >= total)
            break;
    }
    return queued;
}

void *run(void *) noexcept
{
    /*
     * Films and shows first, in the order a library lists them, so the first
     * screens of every category are the first to be warm. Seasons after:
     * reaching one takes opening a show first, which is time enough.
     */
    const int posters = walk("Movie,Series", g_poster_height);
    const int seasons = walk("Season", g_season_height);
    slopfin::trace::mark("warm: queued " + std::to_string(posters) + " film and show posters, " +
                         std::to_string(seasons) + " season posters");
    return nullptr;
}
} // namespace

namespace slopfin::warm
{

void begin(int poster_height, int season_height) noexcept
{
    if (g_started.exchange(true))
        return;
    g_poster_height = poster_height;
    g_season_height = season_height;

    pthread_attr_t attributes;
    const bool have_attributes = pthread_attr_init(&attributes) == 0;
    if (have_attributes)
        (void)pthread_attr_setstacksize(&attributes, 4u * 1024u * 1024u);
    void *thread = nullptr;
    const bool created = scePthreadCreate(&thread, have_attributes ? &attributes : nullptr, run,
                                          nullptr, "slopfin-warm") == 0;
    if (have_attributes)
        (void)pthread_attr_destroy(&attributes);
    if (!created)
    {
        g_started.store(false);
        trace::mark("warm: thread FAILED");
        return;
    }
    /* Created from the data worker, which may itself have inherited the render
       core; this does nothing but wait on the network and parse JSON, so it
       gets the spare cores too. */
    if (const std::uint64_t spare = images::spare_cores(); spare != 0)
        (void)scePthreadSetaffinity(thread, spare);
}

} // namespace slopfin::warm
