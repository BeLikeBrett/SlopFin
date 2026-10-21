/*
 * SlopFin - artwork fetching, decoding and caching.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "images.hpp"

#include "bigalloc.hpp"
#include "gfx.hpp"
#include "config.hpp"
#include "http.hpp"
#include "image_cache.hpp"
#include "jellyfin.hpp"
#include "trace.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
 * stb_image's only thread-local state is the failure string, which is compiled
 * out below, and the vertical-flip flag, which nothing here ever sets. Making
 * them ordinary globals avoids pulling in an emulated-TLS runtime the SDK does
 * not ship.
 */
#define STBI_NO_THREAD_LOCALS

/* Route stb's allocations away from the small process heap. */
#define STBI_MALLOC(size) slopfin::bigalloc::allocate(size)
#define STBI_REALLOC(pointer, size) slopfin::bigalloc::reallocate(pointer, size)
#define STBI_FREE(pointer) slopfin::bigalloc::release(pointer)

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#include "../vendor/stb_image.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C"
{
    int scePthreadCreate(void **thread, const void *attr, void *(*entry)(void *), void *argument,
                         const char *name);
    int sceKernelAvailableFlexibleMemorySize(std::size_t *size);
    void *scePthreadSelf();
    int scePthreadGetaffinity(void *thread, std::uint64_t *mask);
    int scePthreadSetaffinity(void *thread, std::uint64_t mask);
}

namespace
{
/* Flexible memory is where malloc comes from; report it in MiB. */
long available_memory_mib() noexcept
{
    std::size_t bytes = 0;
    if (sceKernelAvailableFlexibleMemorySize(&bytes) < 0)
        return -1;
    return static_cast<long>(bytes / (1024u * 1024u));
}
} // namespace

namespace
{
/* Bound decoded artwork by memory and reserve space for playback surfaces; entry count is not a
 * useful byte budget. */
constexpr std::size_t kMaxBytes = 640u * 1024u * 1024u;
constexpr long kFloorIdleMiB = 320;
constexpr long kFloorPlayingMiB = 768;
/* Warming stops this far above the floor, so it never fills memory right up
   to the line and leaves the next thing that allocates to trip over it. */
constexpr long kWarmMarginMiB = 128;
constexpr std::size_t kWorkerStackBytes = 4u * 1024u * 1024u;
constexpr std::size_t kMaxQueued = 64;

/*
 * Three workers, and at most two of them warming at once. Whatever is on
 * screen always has a worker that is not busy with something nobody asked
 * for, so warming a whole library never makes the card being looked at wait
 * behind it.
 */
constexpr int kWorkers = 3;
constexpr int kWarmWorkers = 2;

/*
 * The cores the video decoder is told to use (player.cpp, cpu_affinity 0x3f),
 * which also covers the render thread and the three row workers gfx pins to
 * the lowest cores. Artwork decoding stays off all of them.
 */
constexpr std::uint64_t kDecoderCores = 0x3f;

struct Entry
{
    slopfin::gfx::Bitmap bitmap;
    std::uint64_t last_used = 0;
    bool pending = false;
    bool prefetched = false;
    bool failed = false;
    unsigned failures = 0;
    std::chrono::steady_clock::time_point retry_after{};
};

struct Job
{
    std::string key;
    std::string item_id;
    std::string tag;
    const char *kind = "Primary";
    int height = 0;
    bool warming = false;
    bool speculative = false;
};

std::mutex g_mutex;
/*
 * Workers sleep on this rather than polling. They used to wake every 25 ms to
 * look at an empty queue, which is both a busy loop and a delay: a poster
 * asked for just after a worker went back to sleep waited a frame and a half
 * before anything started fetching it. Waking on submit costs nothing while
 * nothing is happening and starts the fetch immediately when something is.
 */
std::condition_variable g_wake;
std::unordered_map<std::string, Entry> g_entries;
/* What is on screen or about to be: served newest first. */
std::deque<Job> g_queue;
std::deque<Job> g_prefetch_queue;
/* What nobody has asked for yet: served in order, only when the above is empty. */
std::deque<Job> g_warm_queue;
std::unordered_set<std::string> g_warm_queued;
int g_warm_busy = 0;
int g_prefetch_busy = 0;
bool g_playing = false;
std::uint64_t g_clock = 0;
/* g_clock when trim last ran: anything touched since is on screen. */
std::uint64_t g_trim_clock = 0;
/* A running total, so trim is not a walk over every entry. */
std::size_t g_held_bytes = 0;
std::atomic<long> g_free_mib{-1};
bool g_running = false;

/* Warming progress, for the one line that says how it went. */
std::chrono::steady_clock::time_point g_warm_began{};
bool g_warm_reported = true;
long g_warm_loaded = 0;
long g_warm_failed = 0;
long g_warm_disk = 0; /* of those loaded, how many came from the SSD */
/* Microseconds: a poster decodes in well under a millisecond, so summing
   whole milliseconds per job summed to zero. */
std::uint64_t g_warm_fetch_us = 0;
std::uint64_t g_warm_decode_us = 0;

std::size_t weight(const Entry &entry) noexcept
{
    return entry.bitmap.valid()
               ? static_cast<std::size_t>(entry.bitmap.width) *
                     static_cast<std::size_t>(entry.bitmap.height) * sizeof(std::uint32_t)
               : 0u;
}

long floor_mib() noexcept
{
    return g_playing ? kFloorPlayingMiB : kFloorIdleMiB;
}

std::string hex(std::uint64_t value)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    do
    {
        out.insert(out.begin(), kDigits[value & 0xf]);
        value >>= 4;
    } while (value != 0);
    return "0x" + out;
}

std::string make_key(const std::string &item_id, slopfin::images::Kind kind,
                     const std::string &tag = {}) noexcept
{
    switch (kind)
    {
    case slopfin::images::Kind::primary:
        return item_id + "|p";
    case slopfin::images::Kind::backdrop:
        return item_id + "|b";
    case slopfin::images::Kind::logo:
        return item_id + "|l";
    case slopfin::images::Kind::user:
        return item_id + "|u" + tag;
    case slopfin::images::Kind::file:
        return item_id + "|f" + tag;
    case slopfin::images::Kind::remote:
        /* Sized on arrival, so each drawn size is its own entry. */
        return item_id + "|r" + tag;
    }
    return item_id;
}

const char *kind_name(slopfin::images::Kind kind) noexcept
{
    switch (kind)
    {
    case slopfin::images::Kind::primary:
        return "Primary";
    case slopfin::images::Kind::logo:
        return "Logo";
    case slopfin::images::Kind::backdrop:
        return "Backdrop";
    case slopfin::images::Kind::user:
        return "User";
    case slopfin::images::Kind::file:
        return "File";
    case slopfin::images::Kind::remote:
        return "Remote";
    }
    return "Primary";
}

/*
 * A picture on the console's own storage, read into flexible memory: a 4K PNG
 * screenshot is several megabytes and the process heap holds about ten.
 *
 */
unsigned char *read_local_file(const std::string &path, std::size_t &size) noexcept
{
    constexpr std::size_t kLimit = 24u * 1024u * 1024u;
    size = 0;
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return nullptr;
    struct stat st = {};
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || static_cast<std::size_t>(st.st_size) > kLimit)
    {
        (void)close(fd);
        return nullptr;
    }
    auto *bytes = static_cast<unsigned char *>(
        slopfin::bigalloc::allocate(static_cast<std::size_t>(st.st_size)));
    std::size_t have = 0;
    while (bytes != nullptr && have < static_cast<std::size_t>(st.st_size))
    {
        const ssize_t got = read(fd, bytes + have, static_cast<std::size_t>(st.st_size) - have);
        if (got <= 0)
            break;
        have += static_cast<std::size_t>(got);
    }
    (void)close(fd);
    if (bytes != nullptr && have != static_cast<std::size_t>(st.st_size))
    {
        slopfin::bigalloc::release(bytes);
        return nullptr;
    }
    size = have;
    return bytes;
}

/*
 * A request server on the local network can hand out TMDB artwork itself
 * (Seerr's /imageproxy). Over plain HTTP on the LAN that is far quicker than a
 * fresh TLS connection to TMDB for every poster. Set from the render thread,
 * read by the workers, so it lives under g_mutex.
 */
std::string g_proxy_host;
int g_proxy_port = 0;
int g_proxy_failures = 0;

unsigned char *fetch_through_proxy(const std::string &path, std::size_t &size) noexcept
{
    std::string host;
    int port = 0;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        /* A server that keeps refusing is not asked again this session. */
        if (g_proxy_host.empty() || g_proxy_failures >= 3)
            return nullptr;
        host = g_proxy_host;
        port = g_proxy_port;
    }
    const auto response = slopfin::http::get(host, port, "/imageproxy/tmdb/t/p/" + path, {});
    const bool ok =
        response.ok() && response.body.size() > 2 && response.body.size() <= 4u * 1024u * 1024u;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_proxy_failures = ok ? 0 : g_proxy_failures + 1;
    }
    if (!ok)
        return nullptr;
    auto *bytes = static_cast<unsigned char *>(slopfin::bigalloc::allocate(response.body.size()));
    if (bytes == nullptr)
        return nullptr;
    std::memcpy(bytes, response.body.data(), response.body.size());
    size = response.body.size();
    return bytes;
}

unsigned char *fetch_remote(const std::string &url, std::size_t &size) noexcept
{
    size = 0;
    constexpr std::string_view kTmdb = "https://image.tmdb.org/t/p/";
    if (!url.starts_with(kTmdb))
        return nullptr;
    static std::atomic<int> reported{0};
    const auto began = std::chrono::steady_clock::now();
    const auto report = [&](const char *source)
    {
        if (reported.fetch_add(1) < 24)
            slopfin::trace::mark(
                std::string("img remote ") + source + " " +
                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - began)
                                   .count()) +
                "ms " + std::to_string(size) + " bytes");
    };
    if (auto *proxied = fetch_through_proxy(url.substr(kTmdb.size()), size))
    {
        report("proxy");
        return proxied;
    }
    size = 0;
    slopfin::http::WebRequest request;
    if (!request.open(url, "GET", {}, {}, {}) || request.status() != 200)
        return nullptr;
    constexpr std::size_t limit = 4u * 1024u * 1024u;
    std::size_t capacity = 256u * 1024u;
    auto *bytes = static_cast<unsigned char *>(slopfin::bigalloc::allocate(capacity));
    if (bytes == nullptr)
        return nullptr;
    char chunk[16384];
    long count = 0;
    while ((count = request.read(chunk, sizeof(chunk))) > 0)
    {
        if (static_cast<std::size_t>(count) > limit - size)
        {
            count = -1;
            break;
        }
        if (size + static_cast<std::size_t>(count) > capacity)
        {
            capacity = std::min(limit, capacity * 2);
            auto *grown =
                static_cast<unsigned char *>(slopfin::bigalloc::reallocate(bytes, capacity));
            if (!grown)
            {
                count = -1;
                break;
            }
            bytes = grown;
        }
        std::memcpy(bytes + size, chunk, static_cast<std::size_t>(count));
        size += static_cast<std::size_t>(count);
    }
    if (count < 0 || size == 0)
    {
        slopfin::bigalloc::release(bytes);
        size = 0;
        return nullptr;
    }
    report("tmdb");
    return bytes;
}

/*
 * Resamples RGBA down to exactly `target` rows, keeping the shape, by
 * averaging the source area under each output pixel. Done once on a worker so
 * the picture is drawn at its own size every frame after, with no filtering.
 * Only shrinks: a picture smaller than asked for is left alone.
 */
unsigned char *resample_to_height(unsigned char *pixels, int &width, int &height,
                                  int target) noexcept
{
    if (target <= 0 || height <= target)
        return pixels;
    const int out_h = target;
    const int out_w =
        std::max(1, static_cast<int>(static_cast<long long>(width) * target * 2 / height + 1) / 2);
    /* One pass across, into rows of floats, then one pass down. */
    const std::size_t across = static_cast<std::size_t>(out_w) * static_cast<std::size_t>(height);
    auto *mid = static_cast<float *>(slopfin::bigalloc::allocate(across * 4u * sizeof(float)));
    auto *out = static_cast<unsigned char *>(slopfin::bigalloc::allocate(
        static_cast<std::size_t>(out_w) * static_cast<std::size_t>(out_h) * 4u));
    if (mid == nullptr || out == nullptr)
    {
        if (mid != nullptr)
            slopfin::bigalloc::release(mid);
        if (out != nullptr)
            slopfin::bigalloc::release(out);
        return pixels;
    }
    const auto pass = [](int in_size, int out_size, auto &&accumulate)
    {
        const float scale = static_cast<float>(in_size) / static_cast<float>(out_size);
        for (int o = 0; o < out_size; ++o)
        {
            const float from = static_cast<float>(o) * scale;
            const float to = from + scale;
            for (int i = static_cast<int>(from); i < in_size && static_cast<float>(i) < to; ++i)
            {
                const float weight = (std::min(to, static_cast<float>(i + 1)) -
                                      std::max(from, static_cast<float>(i))) /
                                     scale;
                accumulate(o, i, weight);
            }
        }
    };
    std::memset(mid, 0, across * 4u * sizeof(float));
    for (int y = 0; y < height; ++y)
    {
        const unsigned char *in = pixels + static_cast<std::size_t>(y) * width * 4u;
        float *row = mid + static_cast<std::size_t>(y) * out_w * 4u;
        pass(width, out_w,
             [&](int o, int i, float weight)
             {
                 for (int c = 0; c < 4; ++c)
                     row[o * 4 + c] += static_cast<float>(in[i * 4 + c]) * weight;
             });
    }
    std::vector<float> column(static_cast<std::size_t>(out_h) * 4u);
    for (int x = 0; x < out_w; ++x)
    {
        std::fill(column.begin(), column.end(), 0.0f);
        pass(height, out_h,
             [&](int o, int i, float weight)
             {
                 const float *from = mid + (static_cast<std::size_t>(i) * out_w + x) * 4u;
                 for (int c = 0; c < 4; ++c)
                     column[static_cast<std::size_t>(o) * 4u + c] += from[c] * weight;
             });
        for (int y = 0; y < out_h; ++y)
            for (int c = 0; c < 4; ++c)
                out[(static_cast<std::size_t>(y) * out_w + x) * 4u + c] =
                    static_cast<unsigned char>(std::clamp(
                        column[static_cast<std::size_t>(y) * 4u + c] + 0.5f, 0.0f, 255.0f));
    }
    slopfin::bigalloc::release(mid);
    slopfin::bigalloc::release(pixels);
    width = out_w;
    height = out_h;
    return out;
}

/* Averages whole blocks of pixels down to at most `height` rows. RGBA in, RGBA out. */
unsigned char *shrink(unsigned char *pixels, int &width, int &height, int target) noexcept
{
    if (target <= 0 || height <= target)
        return pixels;
    const int factor = (height + target - 1) / target;
    const int out_w = std::max(1, width / factor);
    const int out_h = std::max(1, height / factor);
    auto *out = static_cast<unsigned char *>(slopfin::bigalloc::allocate(
        static_cast<std::size_t>(out_w) * static_cast<std::size_t>(out_h) * 4u));
    if (out == nullptr)
        return pixels;
    const unsigned area = static_cast<unsigned>(factor * factor);
    for (int y = 0; y < out_h; ++y)
        for (int x = 0; x < out_w; ++x)
        {
            unsigned sum[4] = {};
            for (int dy = 0; dy < factor; ++dy)
            {
                const unsigned char *row = pixels + (static_cast<std::size_t>(y * factor + dy) *
                                                         static_cast<std::size_t>(width) +
                                                     static_cast<std::size_t>(x * factor)) *
                                                        4u;
                for (int dx = 0; dx < factor; ++dx)
                    for (int c = 0; c < 4; ++c)
                        sum[c] += row[dx * 4 + c];
            }
            unsigned char *to =
                out + (static_cast<std::size_t>(y) * static_cast<std::size_t>(out_w) +
                       static_cast<std::size_t>(x)) *
                          4u;
            for (int c = 0; c < 4; ++c)
                to[c] = static_cast<unsigned char>(sum[c] / area);
        }
    slopfin::bigalloc::release(pixels);
    width = out_w;
    height = out_h;
    return out;
}

/* Whether a worker may pick up warming now. Called with g_mutex held. */
bool warm_ready_locked() noexcept
{
    if (g_playing || g_warm_queue.empty() || g_warm_busy + g_prefetch_busy >= kWarmWorkers)
        return false;
    const long free_mib = g_free_mib.load();
    return free_mib < 0 || free_mib > floor_mib() + kWarmMarginMiB;
}

/*
 * Releases artwork oldest-first until the cache is under its ceiling and the
 * kernel has at least the floor free. Never what is being fetched, and never
 * what has been drawn since the last trim -- evicting a card that is on screen
 * would only queue it straight back up. Called with g_mutex held.
 */
void evict_locked(long free_mib) noexcept
{
    const long floor = floor_mib();
    /* Counted in bytes: a poster is well under a MiB, so crediting what was
       released in whole MiB would credit nothing, and the loop would release
       the entire cache to satisfy a shortfall of a few megabytes. */
    constexpr long long kMiB = 1024 * 1024;
    long long free_bytes = static_cast<long long>(free_mib) * kMiB;
    const long long floor_bytes = static_cast<long long>(floor) * kMiB;
    const auto over = [&]
    { return g_held_bytes > kMaxBytes || (free_mib >= 0 && free_bytes < floor_bytes); };
    if (!over())
        return;

    std::vector<std::pair<std::uint64_t, std::string>> ages;
    ages.reserve(g_entries.size());
    for (const auto &entry : g_entries)
    {
        if (!entry.second.pending && entry.second.last_used <= g_trim_clock)
            ages.emplace_back(entry.second.last_used, entry.first);
    }
    std::sort(ages.begin(), ages.end());

    std::size_t released = 0;
    std::size_t count = 0;
    for (const auto &aged : ages)
    {
        if (!over())
            break;
        auto found = g_entries.find(aged.second);
        if (found == g_entries.end())
            continue;
        const std::size_t bytes = weight(found->second);
        g_held_bytes -= bytes;
        released += bytes;
        ++count;
        /* The kernel's figure is not asked for again per poster; what was
           just unmapped is added back instead. */
        free_bytes += static_cast<long long>(bytes);
        /* Allocated by stb_image, which owns it through its own free. */
        slopfin::bigalloc::release(found->second.bitmap.pixels);
        g_entries.erase(found);
    }
    if (count > 0)
        slopfin::trace::mark("images: released " + std::to_string(count) + " (" +
                             std::to_string(released / (1024u * 1024u)) + " MiB), " +
                             (g_playing ? "playing" : "idle") + ", floor " + std::to_string(floor) +
                             " MiB");
}

/*
 * Records a finished fetch. Returns the warming summary when this was the last
 * of a run, for the caller to trace outside the lock; empty otherwise.
 */
std::string complete_job(const Job &job, const slopfin::gfx::Bitmap &bitmap, int status,
                         std::uint64_t fetch_us = 0, std::uint64_t decode_us = 0,
                         bool from_disk = false)
{
    std::lock_guard<std::mutex> guard(g_mutex);
    Entry &entry = g_entries[job.key];
    entry.pending = false;
    entry.prefetched = false;
    if (job.speculative)
    {
        --g_prefetch_busy;
        g_wake.notify_one();
    }
    entry.failed = !bitmap.valid();
    if (bitmap.valid())
    {
        entry.bitmap = bitmap;
        entry.failures = 0;
        entry.retry_after = {};
        g_held_bytes += weight(entry);
    }
    else
    {
        entry.failures = std::min(entry.failures + 1, 6u);
        // Transient failures recover; missing artwork is retried less often.
        const unsigned seconds = status == 404 ? 300u : std::min(60u, 1u << entry.failures);
        entry.retry_after = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    }

    if (!job.warming)
        return {};
    --g_warm_busy;
    if (bitmap.valid())
    {
        ++g_warm_loaded;
        g_warm_disk += from_disk ? 1 : 0;
        g_warm_fetch_us += fetch_us;
        g_warm_decode_us += decode_us;
    }
    else
    {
        ++g_warm_failed;
    }
    std::string summary;
    if (g_warm_queue.empty() && g_warm_busy == 0 && !g_warm_reported)
    {
        g_warm_reported = true;
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - g_warm_began)
                              .count();
        const auto loaded = static_cast<std::uint64_t>(std::max(1L, g_warm_loaded));
        const std::uint64_t decode_us = g_warm_decode_us / loaded;
        summary =
            ("images: warm done, " + std::to_string(g_warm_loaded) + " loaded (" +
             std::to_string(g_warm_disk) + " from disk), " + std::to_string(g_warm_failed) +
             " failed, in " + std::to_string(took / 1000) + "." +
             std::to_string((took % 1000) / 100) + " s; fetch avg " +
             std::to_string(g_warm_fetch_us / loaded / 1000u) + " ms, decode avg " +
             std::to_string(decode_us / 1000u) + "." + std::to_string(decode_us % 1000u / 100u) +
             " ms; cache " + std::to_string(g_held_bytes / (1024u * 1024u)) + " MiB, free " +
             std::to_string(g_free_mib.load()) + " MiB");
    }
    /* A warming slot came free. */
    g_wake.notify_one();
    return summary;
}

void *worker(void *) noexcept
{
    /* Once, from inside a worker: the kernel's own answer to where it runs. */
    static std::atomic<bool> reported_cores{false};
    if (!reported_cores.exchange(true))
    {
        std::uint64_t mask = 0;
        (void)scePthreadGetaffinity(scePthreadSelf(), &mask);
        slopfin::trace::mark("images: worker runs on cores " + hex(mask));
    }

    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> guard(g_mutex);
            for (;;)
            {
                if (!g_queue.empty())
                {
                    /* Prioritize recent visible artwork requests over stale requests from fast
                     * scrolling. */
                    job = std::move(g_queue.back());
                    g_queue.pop_back();
                    break;
                }
                if (!g_prefetch_queue.empty() && !g_playing && g_prefetch_busy == 0 &&
                    g_warm_busy < kWarmWorkers)
                {
                    job = std::move(g_prefetch_queue.front());
                    g_prefetch_queue.pop_front();
                    g_entries.at(job.key).prefetched = false;
                    job.speculative = true;
                    ++g_prefetch_busy;
                    break;
                }
                if (warm_ready_locked())
                {
                    job = std::move(g_warm_queue.front());
                    g_warm_queue.pop_front();
                    g_warm_queued.erase(job.key);
                    /* Asked for properly in the meantime, or already held. */
                    if (g_entries.find(job.key) != g_entries.end())
                        continue;
                    Entry &entry = g_entries[job.key];
                    entry.pending = true;
                    entry.last_used = 0; /* unseen: the first to go if room runs out */
                    ++g_warm_busy;
                    break;
                }
                g_wake.wait(guard);
            }
        }

        const auto fetch_began = std::chrono::steady_clock::now();
        int status = 0;
        const bool local = std::strcmp(job.kind, "File") == 0;
        std::vector<unsigned char> encoded;
        std::size_t local_size = 0;
        unsigned char *local_bytes = nullptr;
        /* Server artwork is named by its tag, so a kept copy is always current;
           a picture from the gallery is already on the console. */
        const std::string cache_key =
            local                                  ? std::string{}
            : std::strcmp(job.kind, "Remote") == 0 ? "remote|" + job.item_id
            : job.tag.empty() ? std::string{}
                              : std::string(job.kind) + "|" + job.item_id + "|" + job.tag + "|" +
                                    std::to_string(job.height);
        const bool cached = !cache_key.empty() && slopfin::image_cache::load(cache_key, encoded);
        if (cached)
            status = 200;
        else if (local)
        {
            local_bytes = read_local_file(job.item_id, local_size);
            status = local_bytes != nullptr ? 200 : 404;
        }
        else if (std::strcmp(job.kind, "User") == 0)
        {
            encoded = slopfin::jellyfin::user_image(job.item_id, job.tag);
            status = encoded.empty() ? 404 : 200;
        }
        else if (std::strcmp(job.kind, "Remote") == 0)
        {
            local_bytes = fetch_remote(job.item_id, local_size);
            status = local_bytes != nullptr ? 200 : 404;
        }
        else
        {
            encoded = slopfin::jellyfin::image(job.item_id, job.tag, job.kind, job.height, &status);
        }
        const unsigned char *source = local_bytes != nullptr ? local_bytes : encoded.data();
        const std::size_t source_size = local_bytes != nullptr ? local_size : encoded.size();
        bool keep = !cached && !cache_key.empty() && status == 200 && source_size > 0;
        const auto decode_began = std::chrono::steady_clock::now();

        slopfin::gfx::Bitmap bitmap;
        bool decode_failed = false;
        if (source != nullptr && source_size > 0)
        {
            int width = 0;
            int height = 0;
            int channels = 0;
            const bool valid = stbi_info_from_memory(source, static_cast<int>(source_size), &width,
                                                     &height, &channels) &&
                               width > 0 && height > 0 && width <= 8192 && height <= 8192 &&
                               static_cast<std::uint64_t>(width) * height <= 64u * 1024u * 1024u;
            unsigned char *pixels =
                valid ? stbi_load_from_memory(source, static_cast<int>(source_size), &width,
                                              &height, &channels, 4)
                      : nullptr;
            /* Only what decoded is worth keeping. */
            if (keep && pixels != nullptr)
                slopfin::image_cache::store(cache_key, source, source_size);
            keep = false;
            if (local_bytes != nullptr)
            {
                slopfin::bigalloc::release(local_bytes);
                local_bytes = nullptr;
            }
            if (pixels != nullptr && width > 0 && height > 0 && local)
                pixels = shrink(pixels, width, height, job.height);
            /* TMDB only serves fixed widths; bring its posters to the size
               they are drawn at. */
            if (pixels != nullptr && width > 0 && height > 0 &&
                std::strcmp(job.kind, "Remote") == 0)
                pixels = resample_to_height(pixels, width, height, job.height);
            if (pixels != nullptr && width > 0 && height > 0)
            {
                /*
                 * stb produces R,G,B,A in memory order; the framebuffer wants
                 * 0xAARRGGBB, which is B,G,R,A. Swapping the two colour bytes
                 * in place turns one into the other without a second buffer
                 * the size of the whole image.
                 */
                const std::size_t count =
                    static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
                for (std::size_t i = 0; i < count; ++i)
                {
                    unsigned char *pixel = pixels + i * 4;
                    const unsigned char red = pixel[0];
                    pixel[0] = pixel[2];
                    pixel[2] = red;
                }
                bitmap.pixels = reinterpret_cast<std::uint32_t *>(pixels);
                bitmap.width = width;
                bitmap.height = height;
                pixels = nullptr; /* ownership moved into the cache */
            }
            else
            {
                decode_failed = true;
                if (pixels != nullptr)
                    slopfin::bigalloc::release(pixels);
            }
        }

        const auto finished = std::chrono::steady_clock::now();
        const auto us = [](auto from, auto to)
        {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(to - from).count());
        };
        const auto ms = [&us](auto from, auto to) { return us(from, to) / 1000u; };
        const std::string summary = complete_job(job, bitmap, status, us(fetch_began, decode_began),
                                                 us(decode_began, finished), cached);
        if (!summary.empty())
            slopfin::trace::mark(summary);
        g_free_mib.store(available_memory_mib());

        /* Report the first several outcomes so a pattern is visible. */
        static std::atomic<int> reported{0};
        if (!bitmap.valid() || reported.fetch_add(1) < 10)
        {
            std::string line = "img status=" + std::to_string(status) +
                               " bytes=" + std::to_string(source_size) +
                               " freeMiB=" + std::to_string(g_free_mib.load()) + " fetch " +
                               std::to_string(ms(fetch_began, decode_began)) + "ms decode " +
                               std::to_string(ms(decode_began, finished)) + "ms";
            if (job.warming)
                line += " warm";
            if (bitmap.valid())
                line += " decoded " + std::to_string(bitmap.width) + "x" +
                        std::to_string(bitmap.height);
            else if (decode_failed)
                line += " DECODE FAILED";
            slopfin::trace::mark(line);
        }
    }
    return nullptr;
}
} // namespace

namespace slopfin::images
{

void set_remote_proxy(const std::string &host, int port) noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (host == g_proxy_host && port == g_proxy_port)
        return;
    g_proxy_host = host;
    g_proxy_port = port;
    g_proxy_failures = 0;
}

std::uint64_t spare_cores() noexcept
{
    return gfx::process_cores() & ~kDecoderCores;
}

bool start() noexcept
{
    if (g_running)
        return true;
    g_free_mib.store(available_memory_mib());
    /* The server's own JPEGs, kept byte for byte: already compressed, so
       nothing smaller is possible without re-encoding them worse. A poster is
       ~50 KiB and a backdrop ~80 KiB, so this holds thousands of titles; past
       it the oldest pictures go first. */
    constexpr std::uint64_t kDiskCacheMiB = 1024;
    if (!slopfin::image_cache::configure(config::data_directory() + "/cache/images",
                                         kDiskCacheMiB << 20))
        trace::mark("images: disk cache unavailable");
    else
        trace::mark("images: disk cache holds " +
                    std::to_string(slopfin::image_cache::held_bytes() / (1024u * 1024u)) +
                    " MiB of " + std::to_string(kDiskCacheMiB));

    /*
     * The default thread stack is far too small to decode a JPEG on. Ask for a
     * generous one explicitly, the same as the reference client does for its
     * network thread.
     */
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0)
        return false;
    (void)pthread_attr_setstacksize(&attributes, kWorkerStackBytes);

    /* Move artwork workers off render/decoder cores; new threads otherwise inherit the render
     * affinity. */
    std::uint64_t inherited = 0;
    (void)scePthreadGetaffinity(scePthreadSelf(), &inherited);
    const std::uint64_t spare = spare_cores();

    void *thread = nullptr;
    int started = 0;
    for (int i = 0; i < kWorkers; ++i)
    {
        if (scePthreadCreate(&thread, &attributes, worker, nullptr, "slopfin-img") != 0)
            continue;
        if (spare != 0)
            (void)scePthreadSetaffinity(thread, spare);
        ++started;
    }
    (void)pthread_attr_destroy(&attributes);
    g_running = started > 0;
    slopfin::trace::mark(g_running ? "images: " + std::to_string(started) +
                                         " workers, would have inherited " + hex(inherited) +
                                         ", given " + hex(spare)
                                   : std::string{"images: workers FAILED"});
    return g_running;
}

const gfx::Bitmap *acquire(const std::string &item_id, const std::string &tag, Kind kind,
                           int target_height) noexcept
{
    if (item_id.empty())
        return nullptr;
    const std::string key = make_key(item_id, kind, tag);

    std::lock_guard<std::mutex> guard(g_mutex);
    ++g_clock;
    auto found = g_entries.find(key);
    if (found != g_entries.end())
    {
        found->second.last_used = g_clock;
        if (found->second.prefetched)
        {
            found->second.prefetched = false;
            const auto queued = std::find_if(g_prefetch_queue.begin(), g_prefetch_queue.end(),
                                             [&key](const Job &job) { return job.key == key; });
            if (queued != g_prefetch_queue.end())
            {
                g_queue.push_back(std::move(*queued));
                g_prefetch_queue.erase(queued);
                g_wake.notify_one();
            }
        }
        if (found->second.bitmap.valid())
            return &found->second.bitmap;
        if (found->second.pending || !found->second.failed ||
            std::chrono::steady_clock::now() < found->second.retry_after)
            return nullptr;
        // Reuse this failed entry and queue exactly one retry when it is due.
    }

    /*
     * A full queue used to refuse the request outright, which is the worst
     * possible answer while scrolling: the queue fills with rows that have
     * already scrolled past, and the cards actually on screen are the ones
     * turned away. The oldest waiting job is dropped instead, so the queue
     * always has room for what is being looked at now.
     */
    while (g_queue.size() >= kMaxQueued)
    {
        g_entries.erase(g_queue.front().key);
        g_queue.pop_front();
    }

    Entry &entry = g_entries[key];
    entry.pending = true;
    entry.failed = false;
    entry.last_used = g_clock;

    Job job;
    job.key = key;
    job.item_id = item_id;
    job.tag = tag;
    job.kind = kind_name(kind);
    /* Ask the server for artwork at the size it will actually be drawn, so a
       4K surface is not fed posters cut for a 1080p one. */
    job.height = gfx::to_physical_size(target_height);
    g_queue.push_back(std::move(job));
    g_wake.notify_one();
    return nullptr;
}

bool failed(const std::string &item_id, const std::string &tag, Kind kind) noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    const auto found = g_entries.find(make_key(item_id, kind, tag));
    return found != g_entries.end() && found->second.failed && !found->second.pending;
}

void prefetch(const std::string &item_id, const std::string &tag, Kind kind,
              int target_height) noexcept
{
    if (item_id.empty())
        return;
    const auto key = make_key(item_id, kind, tag);
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_entries.find(key) != g_entries.end())
        return;
    while (g_prefetch_queue.size() >= kMaxQueued)
    {
        g_entries.erase(g_prefetch_queue.front().key);
        g_prefetch_queue.pop_front();
    }
    Entry &entry = g_entries[key];
    entry.pending = true;
    entry.prefetched = true;
    Job job;
    job.key = key;
    job.item_id = item_id;
    job.tag = tag;
    job.kind = kind_name(kind);
    job.height = gfx::to_physical_size(target_height);
    g_prefetch_queue.push_back(std::move(job));
    g_wake.notify_one();
}

void warm(const std::string &item_id, const std::string &tag, int target_height, bool soon) noexcept
{
    if (item_id.empty() || tag.empty())
        return;
    std::string key = make_key(item_id, Kind::primary);

    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_entries.find(key) != g_entries.end() || g_warm_queued.count(key) != 0)
        return;
    if (g_warm_reported)
    {
        /* A new run of warming, after an idle cache. */
        g_warm_reported = false;
        g_warm_began = std::chrono::steady_clock::now();
        g_warm_loaded = 0;
        g_warm_failed = 0;
        g_warm_disk = 0;
        g_warm_fetch_us = 0;
        g_warm_decode_us = 0;
    }

    Job job;
    job.key = key;
    job.item_id = item_id;
    job.tag = tag;
    job.kind = kind_name(Kind::primary);
    job.height = gfx::to_physical_size(target_height);
    job.warming = true;
    g_warm_queued.insert(std::move(key));
    if (soon)
        g_warm_queue.push_front(std::move(job));
    else
        g_warm_queue.push_back(std::move(job));
    g_wake.notify_one();
}

std::size_t cached_bytes() noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_held_bytes;
}

void set_playback(bool playing) noexcept
{
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (g_playing == playing)
            return;
    }
    const long free_mib = available_memory_mib();
    g_free_mib.store(free_mib);
    std::lock_guard<std::mutex> guard(g_mutex);
    g_playing = playing;
    if (playing)
    {
        /*
         * Made room for now, before the stream allocates anything, rather
         * than at the next trim: the frame pool is taken in the first second
         * of playback, and the next trim could be two seconds away.
         */
        evict_locked(free_mib);
    }
    else
    {
        g_wake.notify_all();
    }
    slopfin::trace::mark(std::string{"images: "} + (playing ? "playback began" : "playback ended") +
                         ", cache " + std::to_string(g_held_bytes / (1024u * 1024u)) +
                         " MiB, free " + std::to_string(free_mib) + " MiB");
}

void trim() noexcept
{
    const long free_mib = available_memory_mib();
    g_free_mib.store(free_mib);
    std::lock_guard<std::mutex> guard(g_mutex);
    evict_locked(free_mib);
    g_trim_clock = g_clock;
    /* Room may have come back for warming that was waiting on it. */
    if (!g_warm_queue.empty())
        g_wake.notify_one();
}

} // namespace slopfin::images
