/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_TEST_FAKE_VOICE_SEARCH_HPP
#define SLOPFIN_TEST_FAKE_VOICE_SEARCH_HPP
#include "../src/voice_search.hpp"
// Linked fake backend: include once in each test executable.

namespace slopfin::voice_search
{
inline Snapshot fake_snapshot;
inline std::string fake_result;
inline int starts = 0, finishes = 0, cancellations = 0, deliveries = 0;
bool fail_start = false;

bool busy() noexcept
{
    return fake_snapshot.phase == Phase::preparing || fake_snapshot.phase == Phase::downloading ||
           fake_snapshot.phase == Phase::listening || fake_snapshot.phase == Phase::processing;
}
bool start() noexcept
{
    if (busy())
        return false;
    if (fail_start)
    {
        fake_snapshot = {Phase::error, "Could not start voice search. Try again."};
        return false;
    }
    ++starts;
    fake_result.clear();
    fake_snapshot = {Phase::listening, "Controller microphone", 0.0f, 0.65f, false};
    return true;
}
void finish() noexcept
{
    ++finishes;
    fake_snapshot.phase = Phase::processing;
}
void cancel() noexcept
{
    ++cancellations;
    fake_result.clear();
    fake_snapshot = {};
}
Snapshot snapshot() noexcept
{
    return fake_snapshot;
}
bool take_result(std::string &out) noexcept
{
    if (fake_snapshot.phase != Phase::complete || fake_result.empty())
        return false;
    out = std::move(fake_result);
    fake_result.clear();
    fake_snapshot = {};
    ++deliveries;
    return true;
}
void shutdown() noexcept
{
    cancel();
}
} // namespace slopfin::voice_search

#endif
