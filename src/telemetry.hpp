/*
 * SlopFin - per-frame timing log.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Judder is a property of the distribution of frame intervals, not of any one
 * number, and it cannot be read off a screenshot. While explicitly enabled,
 * render iterations are recorded into a ring. A separate dump request writes
 * it to disk; ordinary playback never periodically writes a trace.
 */

#ifndef SLOPFIN_TELEMETRY_HPP
#define SLOPFIN_TELEMETRY_HPP

#include <cstdint>

namespace slopfin::telemetry
{

struct Sample
{
    std::uint64_t at_us;      /* when this render iteration began */
    std::uint32_t loop_us;    /* since the previous iteration */
    std::uint32_t draw_us;    /* spent drawing */
    std::uint32_t present_us; /* spent in the flip, which includes the vblank wait */
    std::uint32_t decode_us;
    std::uint32_t convert_us;
    std::int32_t pts_ms;       /* the picture on screen, -1 when none */
    std::uint8_t advanced;     /* a new picture was taken this iteration */
    std::uint8_t queued;       /* frames decoded and waiting */
    std::uint32_t network_us;  /* last read from the socket */
    std::uint32_t slotwait_us; /* decode thread waiting for a free frame slot */
    std::uint32_t pad_us;      /* reading the controller */
    std::uint32_t decoded, converted, shown, dropped, decode_failures;
    std::uint32_t input_stage, input_wait_us, audio_buffered, audio_played, audio_underrun,
        audio_errors;
    std::uint64_t audio_buffering;
    std::uint32_t audio_rebuffers;
    std::uint64_t software_frames, software_work_us, software_queue_us, software_bytes;
    std::uint8_t composed; /* the page background was rebuilt this iteration */
};

bool enabled() noexcept;
void record(const Sample &sample) noexcept;

/* Writes the ring to /data/slopfin-frames.csv. Diagnostic request only: blocks. */
void flush() noexcept;

void enable(bool on) noexcept;

} // namespace slopfin::telemetry

#endif
