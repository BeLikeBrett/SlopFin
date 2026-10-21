/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "stream_failure.hpp"
#include <cassert>
#include <cstdio>

using slopfin::player::stream_failure_message;

static bool has(const std::string &text, const char *part)
{
    return text.find(part) != std::string::npos;
}

int main()
{
    /* No response at all is a reachability problem, not a server refusal. */
    assert(has(stream_failure_message(0), "Could not connect"));
    assert(!has(stream_failure_message(0), "HTTP"));
    assert(has(stream_failure_message(401), "refused the video stream (HTTP 401)"));
    assert(has(stream_failure_message(403), "HTTP 403"));
    assert(has(stream_failure_message(404), "could not find this video (HTTP 404)"));
    /* An FFmpeg failure, which direct-playing clients never hit. */
    assert(has(stream_failure_message(500), "could not prepare this video (HTTP 500)"));
    assert(has(stream_failure_message(500), "FFmpeg log"));
    assert(has(stream_failure_message(503), "HTTP 503"));
    assert(stream_failure_message(302) == "The server would not start the stream (HTTP 302).");
    /* The player shows the cause, then the advice: one ". " separates them. */
    for (int status : {0, 401, 404, 500})
    {
        const std::string text = stream_failure_message(status);
        const auto split = text.find(". ");
        assert(split != std::string::npos && text.find(". ", split + 2) == std::string::npos);
        assert(text.size() - split < 110);
    }
    std::puts("stream failure messages passed");
    return 0;
}
