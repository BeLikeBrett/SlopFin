/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_STREAM_FAILURE_HPP
#define SLOPFIN_STREAM_FAILURE_HPP
#include <string>

namespace slopfin::player
{
/*
 * What to tell the viewer when the media request fails. Every SlopFin stream
 * is repackaged or converted by the server's FFmpeg, unlike clients that
 * direct-play, so a server whose FFmpeg cannot run fails here and nowhere
 * else; say so rather than leaving one message for every cause.
 */
inline std::string stream_failure_message(int status)
{
    const std::string code = " (HTTP " + std::to_string(status) + ")";
    if (status <= 0)
        return "Could not connect to the server's video stream. Check that the PS5 can "
               "reach your server, then try again.";
    if (status == 401 || status == 403)
        return "The server refused the video stream" + code +
               ". Sign out and back in, and check this user is allowed to play media.";
    if (status == 404)
        return "The server could not find this video" + code +
               ". Rescan the library in Jellyfin, then try again.";
    if (status >= 500)
        return "The server could not prepare this video" + code +
               ". SlopFin needs Jellyfin's FFmpeg to repackage video; check the FFmpeg log in "
               "Dashboard > Logs.";
    return "The server would not start the stream" + code + ".";
}
} // namespace slopfin::player
#endif
