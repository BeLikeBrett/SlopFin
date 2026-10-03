/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/* Bounded media-ahead gate, independent of wall-clock sleeps and devices.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_AUDIO_BUFFER_GATE_HPP
#define SLOPFIN_AUDIO_BUFFER_GATE_HPP
#include <algorithm>
#include <cstdint>
namespace slopfin::audio
{
class BufferGate
{
    bool enabled_ = false, waiting_ = false, failed_ = false;
    std::uint32_t waited_ = 0, target_ = 72000, events_ = 0;

  public:
    void reset(bool enabled) noexcept
    {
        *this = BufferGate{};
        enabled_ = waiting_ = enabled;
    }
    // Called only for an unpaused AudioOut quantum, all counts at 48 kHz.
    bool ready(std::uint32_t available, std::uint32_t grain, bool video_ready, bool eof) noexcept
    {
        if (!enabled_)
            return available >= grain;
        if (failed_)
            return false;
        if (!waiting_ && available < grain && !eof)
        {
            waiting_ = true;
            waited_ = 0;
            ++events_;
            target_ = std::min(192000u, 96000u + std::min(events_ - 1, 2u) * 48000u);
        }
        if (waiting_)
        {
            if (video_ready && available >= grain &&
                (available >= target_ || eof || waited_ >= 480000))
            {
                waiting_ = false;
                waited_ = 0;
            }
            else if (waited_ >= 480000)
            {
                // No playable audio/video after ten seconds is a failure, not
                // permission to hang forever or silently switch clocks.
                waiting_ = false;
                failed_ = true;
                return false;
            }
            else
            {
                waited_ += grain;
                return false;
            }
        }
        return available >= grain;
    }
    bool waiting() const noexcept
    {
        return waiting_;
    }
    bool failed() const noexcept
    {
        return failed_;
    }
    std::uint32_t events() const noexcept
    {
        return events_;
    }
};
} // namespace slopfin::audio
#endif
