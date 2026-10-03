/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
// Exercise production cache scheduling; failed fetches must not become permanent.
#include "../src/images.cpp"
#include <cassert>
namespace slopfin::gfx
{
int to_physical_size(int value) noexcept
{
    return value;
}
} // namespace slopfin::gfx
// Eviction traces and frees; neither is under test, and the pixels below are not mapped.
namespace slopfin::trace
{
void mark(std::string_view) noexcept
{
}
} // namespace slopfin::trace
namespace slopfin::bigalloc
{
void release(void *) noexcept
{
}
} // namespace slopfin::bigalloc
int main()
{
    using namespace slopfin;
    const auto get = [] { return images::acquire("item", "tag", images::Kind::primary, 480); };
    assert(get() == nullptr && g_queue.size() == 1);
    const Job job = g_queue.front();
    g_queue.clear();
    assert(get() == nullptr && g_queue.empty()); // in flight: no duplicates
    complete_job(job, {}, 503);
    assert(g_entries.at(job.key).failed);
    assert(get() == nullptr && g_queue.empty()); // respect retry delay
    g_entries.at(job.key).retry_after = {};
    assert(get() == nullptr && g_queue.size() == 1);
    assert(get() == nullptr && g_queue.size() == 1);
    g_queue.clear();
    complete_job(job, {}, 503);
    assert(g_entries.at(job.key).failures == 2);
    g_entries.at(job.key).retry_after = {};
    get();
    g_queue.clear();
    std::uint32_t pixel = 0xff00ff00;
    gfx::Bitmap bitmap;
    bitmap.pixels = &pixel;
    bitmap.width = bitmap.height = 1;
    complete_job(job, bitmap, 200);
    assert(get() && get()->pixels == &pixel);
    assert(g_entries.at(job.key).failures == 0 && g_queue.empty());
    g_entries.clear();
    get();
    g_queue.clear();
    complete_job(job, {}, 404);
    assert(g_entries.at(job.key).retry_after - std::chrono::steady_clock::now() >
           std::chrono::seconds(290));
    assert(get() == nullptr && g_queue.empty());

    g_entries.clear();
    g_queue.clear();
    g_held_bytes = 0;

    // Warming never shadows what is on screen: a warmed key is not an entry
    // until a worker takes it, so asking for it properly goes straight to the
    // demand queue rather than waiting behind the whole library.
    images::warm("w", "t", 480);
    assert(g_warm_queue.size() == 1 && g_entries.count("w|p") == 0);
    assert(images::acquire("w", "t", images::Kind::primary, 480) == nullptr);
    assert(g_queue.size() == 1 && g_entries.at("w|p").pending);
    images::warm("w", "t", 480); // already queued: not queued twice
    assert(g_warm_queue.size() == 1);
    g_queue.clear();
    g_entries.clear();
    images::warm("x", "t", 480, true); // soon goes ahead of the library
    assert(g_warm_queue.front().item_id == "x");
    images::warm("", "t", 480);
    images::warm("y", "", 480); // nothing to fetch
    assert(g_warm_queue.size() == 2);

    // Warming waits for room, for a free slot, and for playback to end.
    g_free_mib = 2000;
    g_playing = false;
    g_warm_busy = 0;
    assert(warm_ready_locked());
    g_playing = true;
    assert(!warm_ready_locked());
    g_playing = false;
    g_warm_busy = kWarmWorkers;
    assert(!warm_ready_locked());
    g_warm_busy = 0;
    g_free_mib = kFloorIdleMiB + kWarmMarginMiB;
    assert(!warm_ready_locked());
    g_warm_queue.clear();
    g_warm_queued.clear();

    // Eviction releases just enough, counted in bytes. A poster is under a MiB,
    // and crediting releases in whole MiB would credit nothing and empty the
    // cache to cover a two-MiB shortfall.
    static std::uint32_t pixels[1];
    const auto hold = [](const std::string &key, std::uint64_t used, int width = 600)
    {
        Entry &entry = g_entries[key];
        entry.bitmap.pixels = pixels;
        entry.bitmap.width = width;
        entry.bitmap.height = 256;
        entry.last_used = used;
        g_held_bytes += weight(entry); // 614,400 bytes
    };
    for (int i = 0; i < 10; ++i)
        hold("p" + std::to_string(i), 1);
    g_trim_clock = 1;
    evict_locked(kFloorIdleMiB - 2);
    assert(g_entries.size() == 6); // ceil(2 MiB / 614,400) = 4
    assert(g_held_bytes == 6u * 614400u);

    // Room means nothing is released at all.
    evict_locked(1500);
    assert(g_entries.size() == 6);

    // Oldest first, unseen warming before anything that was on screen, and
    // never what has been drawn since the last trim.
    g_entries.clear();
    g_held_bytes = 0;
    // 2 MiB apiece, so a 1 MiB shortfall is exactly one release.
    hold("unseen", 0, 2048);
    hold("seen", 1, 2048);
    hold("on-screen", 9, 2048);
    g_trim_clock = 5;
    evict_locked(kFloorIdleMiB - 1);
    assert(g_entries.count("unseen") == 0 && g_entries.count("seen") == 1);
    evict_locked(0);
    assert(g_entries.size() == 1 && g_entries.count("on-screen") == 1);

    // Playback raises the floor: the same free memory is room when idle and a
    // shortfall while a stream plays.
    g_entries.clear();
    g_held_bytes = 0;
    hold("a", 1);
    hold("b", 1);
    g_playing = false;
    evict_locked(kFloorPlayingMiB - 1);
    assert(g_entries.size() == 2);
    g_playing = true;
    evict_locked(kFloorPlayingMiB - 1);
    assert(g_entries.size() == 0);
}
