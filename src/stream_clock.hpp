/*
 * SlopFin - turning a stream timestamp into a position in the film.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every stream Jellyfin remuxes or transcodes for this client is written by
 * ffmpeg's MPEG-TS muxer, and that muxer shifts every timestamp forward by
 * twice the mux delay (libavformat/mpegtsenc.c: `delay = max_delay * 2`,
 * applied unless -mpegts_copyts 1). The command-line default mux delay is
 * 0.7 s (fftools/ffmpeg_opt.c), and Jellyfin passes neither option, so its
 * streams run 1.4 s ahead of the film.
 *
 * Validated against source presentation timestamps: a picture at
 * 166.833 s in FROM S04E03 arrived stamped 168.233 s, and the same +1.400 s on
 * X-Men '97, Silo, and a re-encoded stream. The console logged the same
 * 168.233 s. Taken as film time, that clock showed every subtitle 1.4 s early,
 * put the timeline 1.4 s ahead, and reported resume points 1.4 s late.
 *
 * Header-only so the arithmetic is tested on the host.
 */

#ifndef SLOPFIN_STREAM_CLOCK_HPP
#define SLOPFIN_STREAM_CLOCK_HPP

#include <algorithm>

namespace slopfin::player
{

/* ffmpeg's MPEG-TS muxer offset at its default mux delay: 2 x 0.7 s. */
inline constexpr double kServerMuxShiftSeconds = 1.4;

/*
 * `stream_seconds` is a timestamp from the stream. `absolute` says the server
 * copied the source timestamps (CopyTimestamps), so the stream counts from the
 * start of the film; otherwise it counts from `start_seconds`, where playback
 * was asked to begin. `server_muxed` is false only for a file played directly,
 * which no muxer touched.
 */
inline double film_seconds(double stream_seconds, double start_seconds, bool absolute,
                           bool server_muxed) noexcept
{
    const double base = absolute ? stream_seconds : start_seconds + stream_seconds;
    return std::max(0.0, base - (server_muxed ? kServerMuxShiftSeconds : 0.0));
}

} // namespace slopfin::player

#endif
