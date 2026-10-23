/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_VOICE_SEARCH_HPP
#define SLOPFIN_VOICE_SEARCH_HPP
#include <cstdint>
#include <string>

namespace slopfin::voice_search
{
enum class Phase : std::uint8_t
{
    idle,
    preparing,
    downloading,
    listening,
    processing,
    complete,
    error
};
struct Snapshot
{
    Phase phase = Phase::idle;
    std::string message;
    float progress = 0.0f;
    float level = 0.0f;
    bool muted = false;
};
/* Asynchronous; the first use downloads and verifies a 32 MB multilingual model.
 * Captures the signed-in user's PS5 input device. The controller microphone is
 * used when it is selected in PS5 microphone settings; mute is never changed. */
bool start() noexcept;
void finish() noexcept; /* Stop recording and recognise the captured words. */
void cancel() noexcept; /* Discard audio/result; does not wait on the render thread. */
[[nodiscard]] Snapshot snapshot() noexcept;
[[nodiscard]] bool busy() noexcept;
bool take_result(std::string &out) noexcept;
void shutdown() noexcept; /* Cancel and join before tearing down HTTP/platform. */
} // namespace slopfin::voice_search
#endif
