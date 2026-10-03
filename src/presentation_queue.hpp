/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/* SlopFin - timestamps for the decoder's reordered pictures.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SLOPFIN_PRESENTATION_QUEUE_HPP
#define SLOPFIN_PRESENTATION_QUEUE_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace slopfin::player
{
/* H.264/HEVC access units arrive in decode order; VideoDec2 returns pictures
 * in presentation order. Keep timestamps for ALL successful submissions,
 * including calls which buffer a picture without returning one yet.
 */
class PresentationQueue
{
  public:
    bool push(std::int64_t pts) noexcept
    {
        if (count_ == values_.size())
            return false;
        values_[count_++] = pts;
        return true;
    }

    bool take(std::int64_t &pts) noexcept
    {
        if (count_ == 0)
            return false;
        std::size_t best = 0;
        for (std::size_t i = 1; i < count_; ++i)
            if (values_[i] >= 0 && (values_[best] < 0 || values_[i] < values_[best]))
                best = i;
        pts = values_[best];
        for (std::size_t i = best + 1; i < count_; ++i)
            values_[i - 1] = values_[i];
        --count_;
        return true;
    }

  private:
    std::array<std::int64_t, 64> values_{};
    std::size_t count_ = 0;
};
} // namespace slopfin::player
#endif
