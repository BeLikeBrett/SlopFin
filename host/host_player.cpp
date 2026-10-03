/*
 * SlopFin - Linux host backend: a synthetic player.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Simulates playback timing and controls against server-provided tracks and
 * backdrop artwork. Native decoding remains console-only.
 */

#include "../src/player.hpp"

#include "../src/audio.hpp"
#include "../src/audiocaps.hpp"
#include "../src/bitstream_probe.hpp"
#include "../src/bigalloc.hpp"
#include "../src/config.hpp"
#include "../src/jellyfin.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <string>
#include <vector>

extern "C" int sceKernelGettimeofday(void *value);

/* Declarations only; images.cpp carries the implementation. */
extern "C" unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x,
                                                int *y, int *channels_in_file,
                                                int desired_channels);
extern "C" void stbi_image_free(void *retval_from_stbi_load);

namespace
{
using slopfin::gfx::Bitmap;

std::mutex g_mutex;
slopfin::player::Status g_status;
slopfin::jellyfin::PlaybackRequest g_request;

Bitmap g_picture;
std::vector<std::uint32_t> g_source; /* the fetched backdrop, or a test pattern */
int g_source_w = 0;
int g_source_h = 0;
std::atomic<bool> g_source_ready{false};
std::atomic<bool> g_fetching{false};
std::string g_fetch_id;

double g_started_at = 0.0; /* host clock when the current run began */
double g_run_origin = 0.0; /* stream position at that moment */
long g_shown = 0;

double host_seconds() noexcept
{
    struct HostTime
    {
        std::int64_t seconds;
        std::int64_t microseconds;
    } now{};
    (void)sceKernelGettimeofday(&now);
    return static_cast<double>(now.seconds) + static_cast<double>(now.microseconds) / 1e6;
}

/* A picture with structure in it: broad colour fields, a bright band and a
   dark one, so an overlay can be judged against both at once. */
void build_test_pattern() noexcept
{
    g_source_w = 1280;
    g_source_h = 720;
    g_source.resize(static_cast<std::size_t>(g_source_w) * g_source_h);
    for (int y = 0; y < g_source_h; ++y)
        for (int x = 0; x < g_source_w; ++x)
        {
            const double u = static_cast<double>(x) / g_source_w;
            const double v = static_cast<double>(y) / g_source_h;
            auto r = static_cast<std::uint32_t>(40 + 200 * u * (1.0 - v));
            auto g = static_cast<std::uint32_t>(30 + 190 * v);
            auto b = static_cast<std::uint32_t>(60 + 180 * (1.0 - u));
            if (v > 0.72 && v < 0.82)
                r = g = b = 245; /* a bright band under where the controls sit */
            if (v > 0.86)
                r = g = b = 12;
            g_source[static_cast<std::size_t>(y) * g_source_w + x] =
                0xff000000u | (r << 16) | (g << 8) | b;
        }
}

void *fetch_backdrop(void *) noexcept
{
    std::string id;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        id = g_fetch_id;
    }
    std::vector<unsigned char> bytes = slopfin::jellyfin::image(id, {}, "Backdrop", 1080);
    if (bytes.empty())
        bytes = slopfin::jellyfin::image(id, {}, "Primary", 1080);
    if (!bytes.empty())
    {
        int w = 0;
        int h = 0;
        int channels = 0;
        unsigned char *pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                      &w, &h, &channels, 4);
        if (pixels != nullptr && w > 0 && h > 0)
        {
            std::vector<std::uint32_t> decoded(static_cast<std::size_t>(w) * h);
            for (std::size_t i = 0; i < decoded.size(); ++i)
            {
                const unsigned char *p = pixels + i * 4;
                decoded[i] = (static_cast<std::uint32_t>(p[3]) << 24) |
                             (static_cast<std::uint32_t>(p[0]) << 16) |
                             (static_cast<std::uint32_t>(p[1]) << 8) | p[2];
            }
            stbi_image_free(pixels);
            std::lock_guard<std::mutex> guard(g_mutex);
            g_source = std::move(decoded);
            g_source_w = w;
            g_source_h = h;
            g_source_ready = true;
            g_fetching = false;
            return nullptr;
        }
        if (pixels != nullptr)
            stbi_image_free(pixels);
    }
    std::lock_guard<std::mutex> guard(g_mutex);
    build_test_pattern();
    g_source_ready = true;
    g_fetching = false;
    return nullptr;
}

/* The stream position, advanced by the host clock unless paused. */
double position_now() noexcept
{
    if (g_status.paused || g_status.state != slopfin::player::State::playing)
        return g_run_origin;
    return g_run_origin + (host_seconds() - g_started_at);
}

void restart_clock(double at) noexcept
{
    g_run_origin = at;
    g_started_at = host_seconds();
}
} // namespace

namespace slopfin::player
{

bool start(const jellyfin::PlaybackRequest &request, const std::string &title,
           double duration_seconds) noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    g_request = request;
    g_status = Status{};
    g_status.state = State::playing;
    g_status.title = title;
    g_status.duration_seconds = duration_seconds > 0.0 ? duration_seconds : 45.0 * 60.0;
    g_status.width = 1920;
    g_status.height = 1080;
    g_status.frame_rate = 23.976;
    g_status.source_fps = 23.976;
    g_status.audio_index = request.audio_index;
    g_status.subtitle_index = request.subtitle_index;
    g_status.max_bitrate = request.max_bitrate;
    g_status.media_source_id = request.media_source_id;
    g_status.colour_info = "BT.709 8-bit (host preview)";
    g_status.delivery_info = "host preview, no decode";
    restart_clock(request.start_seconds);

    if (!g_fetching.exchange(true))
    {
        g_source_ready = false;
        g_fetch_id = request.item_id;
        pthread_t thread{};
        if (pthread_create(&thread, nullptr, fetch_backdrop, nullptr) == 0)
            pthread_detach(thread);
        else
            g_fetching = false;
    }
    return true;
}

void stop(bool) noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    g_status.state = State::idle;
}

void set_paused(bool paused) noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    const double at = position_now();
    g_status.paused = paused;
    restart_clock(at);
}

bool paused() noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_status.paused;
}

jellyfin::PlaybackRequest current_request() noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_request;
}

/* The preview has no console markers: it negotiates as a default build. */
jellyfin::PlaybackRequest with_console_options(jellyfin::PlaybackRequest request) noexcept
{
    return request;
}

bool restart(const jellyfin::PlaybackRequest &request, bool) noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    g_request = request;
    g_status.audio_index = request.audio_index;
    g_status.subtitle_index = request.subtitle_index;
    g_status.max_bitrate = request.max_bitrate;
    restart_clock(request.start_seconds);
    return true;
}

bool seek(double seconds) noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    g_request.start_seconds = seconds;
    restart_clock(std::max(0.0, seconds));
    return true;
}

void show_text_subtitle(int index) noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    g_status.subtitle_index = index;
}

Status status() noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    Status out = g_status;
    out.position_seconds = std::clamp(position_now(), 0.0, out.duration_seconds);
    out.shown = g_shown;
    out.frames = g_shown;
    out.decoded = g_shown;
    out.queued = 3;
    out.frame_interval_us = 41708;
    return out;
}

void probe_decoder_support() noexcept
{
}

const gfx::Bitmap *frame() noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_status.state != State::playing)
        return nullptr;
    if (!g_source_ready)
        return nullptr;
    if (g_source.empty())
        return nullptr;

    /*
     * A slow pan, so the picture is visibly running and the overlay is judged
     * against moving content rather than a still. The crop is 90 percent of
     * the source so there is something to pan across.
     */
    const int out_w = std::min(1920, g_source_w);
    const int out_h = out_w * g_source_h / g_source_w;
    if (!g_picture.valid() || g_picture.width != out_w || g_picture.height != out_h)
    {
        if (g_picture.pixels != nullptr)
            bigalloc::release(g_picture.pixels);
        g_picture.pixels = static_cast<std::uint32_t *>(
            bigalloc::allocate(static_cast<std::size_t>(out_w) * out_h * sizeof(std::uint32_t)));
        g_picture.width = out_w;
        g_picture.height = out_h;
        g_picture.hdr10 = false;
    }
    if (g_picture.pixels == nullptr)
        return nullptr;

    const double phase = position_now() * 0.05;
    const int span_x = std::max(1, g_source_w - out_w);
    const int span_y = std::max(1, g_source_h - out_h);
    const int shift_x =
        static_cast<int>((0.5 + 0.5 * std::sin(phase)) * static_cast<double>(span_x));
    const int shift_y =
        static_cast<int>((0.5 + 0.5 * std::cos(phase * 0.7)) * static_cast<double>(span_y));
    for (int y = 0; y < out_h; ++y)
    {
        const int sy = std::clamp(y + shift_y, 0, g_source_h - 1);
        const std::uint32_t *in = g_source.data() + static_cast<std::size_t>(sy) * g_source_w;
        std::uint32_t *out = g_picture.pixels + static_cast<std::size_t>(y) * out_w;
        for (int x = 0; x < out_w; ++x)
            out[x] = in[std::clamp(x + shift_x, 0, g_source_w - 1)];
    }
    if (!g_status.paused)
        ++g_shown;
    return &g_picture;
}

} // namespace slopfin::player

/* ------------------------------------------------------------------ audio */

namespace slopfin::audio
{
OutputStats output_stats() noexcept
{
    return {};
}
void interrupt() noexcept
{
}
bool start(Codec, bool) noexcept
{
    return true;
}
void stop(bool) noexcept
{
}
void submit(const std::uint8_t *, std::size_t, std::int64_t) noexcept
{
}
double played_seconds() noexcept
{
    return 0.0;
}
double clock_seconds() noexcept
{
    return -1.0;
}
void begin_capture() noexcept
{
}
void write_capture() noexcept
{
}
void set_paused(bool) noexcept
{
}
bool paused() noexcept
{
    return false;
}
bool running() noexcept
{
    return false;
}
double buffered_seconds() noexcept
{
    return 0.0;
}
} // namespace slopfin::audio

namespace audiocaps
{
void probe() noexcept
{
}
} // namespace audiocaps

namespace bitstream_probe
{
void start(const char *) noexcept
{
}
} // namespace bitstream_probe
