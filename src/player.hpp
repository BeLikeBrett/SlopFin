/*
 * SlopFin - video playback.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Jellyfin serves a single continuous MPEG-TS stream, which is demultiplexed
 * and fed to the console's hardware decoder. The decoder emits NV12, which is
 * converted to the framebuffer's format on worker threads.
 *
 * The Videodec2 setup follows ProsperoTV's iptv_native_backend.c, copyright
 * BlackBearReloaded, GPL-3.0-or-later.
 */

#ifndef SLOPFIN_PLAYER_HPP
#define SLOPFIN_PLAYER_HPP

#include "gfx.hpp"
#include "jellyfin.hpp"

#include <cstdint>
#include <string>

namespace slopfin::player
{

enum class State : std::uint8_t
{
    idle = 0,
    opening,
    playing,
    ended,
    failed,
};

struct Status
{
    State state = State::idle;
    std::string message;
    std::string title;
    int width = 0;
    int height = 0;
    long frames = 0;
    long decoded = 0;
    long decode_failures = 0;
    double position_seconds = 0.0;
    double duration_seconds = 0.0;
    /* Timing, so the pipeline can be judged rather than guessed at. */
    std::uint64_t decode_us = 0;
    int input_stage = 0; // 1 socket read, 2 demux, 3 audio submit, 4 video queue
    std::uint64_t input_since_us = 0;
    std::uint64_t convert_us = 0;
    std::uint64_t frame_interval_us = 0;
    double frame_rate = 0.0; // measured over at least one second
    double source_fps = 0.0; // the rate the video was authored at, from the server
    /*
     * The cadence, which is what judder actually is. holds[n] counts pictures
     * held for n display intervals, so 23.976 on a 59.94 Hz output should be
     * nothing but threes and twos in equal number. Anything in holds[1] or
     * holds[4] is a limp, and `dropped` counts pictures decoded but never
     * shown, which is worse than either.
     */
    unsigned holds[6] = {};
    long dropped = 0;
    std::string colour_info;
    std::string delivery_info;
    int queued = 0;
    /* Rises each time the display takes a newly decoded picture. */
    long shown = 0;
    /* Where the decode thread spent its time on the last access unit. */
    std::uint64_t network_us = 0;
    std::uint64_t demux_us = 0;
    std::uint64_t slotwait_us = 0;
    std::uint64_t network_max_us = 0;
    std::uint64_t slotwait_max_us = 0;
    /* How far the picture sits ahead of the sound, in milliseconds. */
    int audio_lead_ms = 0;
    bool paused = false;
    int audio_index = -1;
    int subtitle_index = -1;
    bool subtitle_burned = false; /* the server draws it into the picture */
    long long max_bitrate = 0;
    /* Encoded audio/video payload bits per media second (no TS overhead). */
    double measured_bitrate = 0.0;
    double download_bitrate = 0.0; /* TS bytes per wall second, includes stalls */
    std::string media_source_id;
};

/* Begins playing an item as described by the request. */
bool start(const jellyfin::PlaybackRequest &request, const std::string &title,
           double duration_seconds) noexcept;

/* A seek may keep the physical HDR/audio output modes alive while the stream
   and decoders restart, avoiding an unnecessary HDMI format renegotiation. */
void stop(bool preserve_output = false) noexcept;

/* Pausing freezes the audio clock, and the picture follows it. */
void set_paused(bool paused) noexcept;
bool paused() noexcept;

/* The request now playing, to change one thing about it and restart. */
jellyfin::PlaybackRequest current_request() noexcept;

/* Restarts the stream; this is how seeking and track changes work, since the
   server delivers one continuous stream per request. */
bool restart(const jellyfin::PlaybackRequest &request, bool preserve_output = false) noexcept;

/* The request as this console would negotiate it: the CPU audio decoder and
   the Dolby Vision base-layer path, where their markers switch them on. Play
   and the Details sheet both go through it, so their answers cannot differ. */
jellyfin::PlaybackRequest with_console_options(jellyfin::PlaybackRequest request) noexcept;
bool seek(double seconds) noexcept;

/* Switches to a text subtitle (or off, with -1) without restarting the stream.
   Only valid when neither the old nor the new track is burned in. */
void show_text_subtitle(int index) noexcept;

Status status() noexcept;

/* Development aid: report which decoder configurations the hardware accepts. */
void probe_decoder_support() noexcept;

/*
 * The newest decoded frame, or nullptr when none is ready. Valid until the
 * next call; the render thread is the only caller.
 */
const gfx::Bitmap *frame() noexcept;

} // namespace slopfin::player

#endif
