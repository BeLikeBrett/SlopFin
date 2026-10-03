/*
 * SlopFin - audio decode and output.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AAC frames come out of the demultiplexer as ADTS, are decoded by the
 * console's audio decoder, and are handed to the audio output in fixed blocks.
 * Derived from ProsperoTV's iptv_native_backend.c, copyright
 * BlackBearReloaded, GPL-3.0-or-later.
 */

#ifndef SLOPFIN_AUDIO_HPP
#define SLOPFIN_AUDIO_HPP

#include <cstddef>
#include <cstdint>

namespace slopfin::audio
{

enum class Codec : std::uint8_t
{
    aac_adts = 0,
    mp3,
    ac3,
    eac3,
    dts,
    truehd,
};

bool start(Codec codec = Codec::aac_adts, bool buffered_start = false) noexcept;
void video_ready() noexcept;
bool buffering() noexcept;
/* preserve_output keeps the current HDMI/PCM port and its format alive across
   a seek; decoders and queued stream state are still torn down and rebuilt. */
void stop(bool preserve_output = false) noexcept;
/* Release a blocked submit before joining its producer; does not free decoders. */
void interrupt() noexcept;
struct OutputStats
{
    std::uint64_t played_frames = 0, underrun_frames = 0;
    std::uint32_t buffered_frames = 0, errors = 0;
    // CPU codec work is elapsed producer time excluding its PCM sink callback,
    // not process CPU utilization. Cumulative per playback session.
    std::uint64_t buffering_frames = 0;
    std::uint32_t rebuffer_events = 0;
    std::uint64_t software_frames = 0, software_work_us = 0, software_queue_us = 0,
                  software_bytes = 0;
};
OutputStats output_stats() noexcept;

/* Queues compressed audio from one PES packet. Timestamps are 90 kHz. */
void submit(const std::uint8_t *data, std::size_t bytes, std::int64_t pts) noexcept;
/* Drain compressed decoder tail on clean transport EOF only. */
void finish() noexcept;
bool failed() noexcept;

/* Seconds of audio already handed to the output. */
double played_seconds() noexcept;

/*
 * Where in the stream the sound currently being heard sits, or a negative
 * value when nothing has played yet. Video follows this rather than a wall
 * clock: the audio output runs on its own crystal, and anything paced against
 * a different clock drifts away from it over the length of a film.
 */
double clock_seconds() noexcept;

/* Development aid: capture decoded audio so it can be inspected off-console. */
void begin_capture() noexcept;
void write_capture() noexcept;

/*
 * Pausing stops consuming decoded audio and emits silence, which freezes the
 * clock video follows; the picture therefore pauses too, and the decode thread
 * stops reading the network once its buffers fill.
 */
void set_paused(bool paused) noexcept;
bool paused() noexcept;

/* True while compressed audio is going to the TV undecoded (IEC 61937). */
bool bitstream() noexcept;

/* True once the output is running and the clock is meaningful. */
bool running() noexcept;

/* Buffered but not yet played, in seconds. */
double buffered_seconds() noexcept;

} // namespace slopfin::audio

#endif
