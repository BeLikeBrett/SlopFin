/*
 * SlopFin - per-frame timing log.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "telemetry.hpp"

#include <array>
#include <cstdio>
#include <cstring>

namespace
{
/* About twenty seconds at sixty frames a second. */
constexpr std::size_t kCapacity = 1200;

std::array<slopfin::telemetry::Sample, kCapacity> g_ring{};
std::size_t g_count = 0;
std::size_t g_next = 0;
/*
 * Off by default. Writing the ring costs several vertical blanks, and doing
 * that on a timer put a visible stutter into playback; it is enabled only when
 * a marker file asks for it.
 */
bool g_enabled = false;
} // namespace

namespace slopfin::telemetry
{

void enable(bool on) noexcept
{
    if (on)
    {
        g_count = 0;
        g_next = 0;
    }
    g_enabled = on;
}

bool enabled() noexcept
{
    return g_enabled;
}

void record(const Sample &sample) noexcept
{
    if (!g_enabled)
        return;
    g_ring[g_next] = sample;
    g_next = (g_next + 1) % kCapacity;
    if (g_count < kCapacity)
        ++g_count;
}

void flush() noexcept
{
    if (!g_enabled || g_count == 0)
        return;
    std::FILE *file = std::fopen("/data/slopfin-frames.csv", "wb");
    if (file == nullptr)
        return;
    (void)std::fputs("at_us,loop_us,draw_us,present_us,decode_us,convert_us,pts_ms,advanced,"
                     "queued,network_us,slotwait_us,pad_us,composed,decoded,converted,shown,"
                     "dropped,decode_failures,"
                     "input_stage,input_wait_us,audio_buffered,audio_played,audio_underrun,audio_"
                     "errors,software_frames,software_work_us,software_queue_us,software_bytes,"
                     "audio_buffering,audio_rebuffers\n",
                     file);

    const std::size_t first = g_count < kCapacity ? 0 : g_next;
    for (std::size_t i = 0; i < g_count; ++i)
    {
        const Sample &s = g_ring[(first + i) % kCapacity];
        (void)std::fprintf(
            file,
            "%llu,%u,%u,%u,%u,%u,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%llu,%llu,%"
            "llu,%llu,%llu,%u\n",
            static_cast<unsigned long long>(s.at_us), s.loop_us, s.draw_us, s.present_us,
            s.decode_us, s.convert_us, s.pts_ms, static_cast<unsigned>(s.advanced),
            static_cast<unsigned>(s.queued), s.network_us, s.slotwait_us, s.pad_us,
            static_cast<unsigned>(s.composed), s.decoded, s.converted, s.shown, s.dropped,
            s.decode_failures, s.input_stage, s.input_wait_us, s.audio_buffered, s.audio_played,
            s.audio_underrun, s.audio_errors, static_cast<unsigned long long>(s.software_frames),
            static_cast<unsigned long long>(s.software_work_us),
            static_cast<unsigned long long>(s.software_queue_us),
            static_cast<unsigned long long>(s.software_bytes),
            static_cast<unsigned long long>(s.audio_buffering), s.audio_rebuffers);
    }
    (void)std::fclose(file);
}

} // namespace slopfin::telemetry
