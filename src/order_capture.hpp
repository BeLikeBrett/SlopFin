/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/* SlopFin - opt-in decoder sequence capture. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_ORDER_CAPTURE_HPP
#define SLOPFIN_ORDER_CAPTURE_HPP
#include "bigalloc.hpp"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <mutex>

namespace slopfin::player
{
// Captures the exact compressed input and sparse luma samples from 120 outputs.
// No disk writes during the measured sequence; buffers use flexible memory.
class OrderCapture
{
    std::mutex mutex_;
    static constexpr std::size_t kInputBytes = 32 * 1024 * 1024;
    static constexpr int kFrames = 120, kWidth = 96, kHeight = 40;
    std::uint8_t *input_ = nullptr;
    std::uint16_t *pictures_ = nullptr;
    std::size_t used_ = 0;
    int count_ = 0, width_ = 0, height_ = 0;
    double pts_[kFrames]{};
    bool done_ = false;

  public:
    OrderCapture() noexcept
    {
        auto *marker = std::fopen("/data/slopfin-order", "rb");
        if (!marker)
            return;
        std::fclose(marker);
        input_ = static_cast<std::uint8_t *>(bigalloc::allocate(kInputBytes));
        pictures_ =
            static_cast<std::uint16_t *>(bigalloc::allocate(kFrames * kWidth * kHeight * 2));
    }
    ~OrderCapture()
    {
        bigalloc::release(input_);
        bigalloc::release(pictures_);
    }
    void input(const void *bytes, std::size_t size) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (done_ || !input_ || !pictures_ || size > kInputBytes - used_)
            return;
        std::memcpy(input_ + used_, bytes, size);
        used_ += size;
    }
    void picture(const std::uint8_t *base, int width, int height, int pitch, bool ten_bit,
                 double seconds) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (done_ || !input_ || !pictures_)
            return;
        width_ = width;
        height_ = height;
        pts_[count_] = seconds;
        auto *out = pictures_ + count_ * kWidth * kHeight;
        for (int y = 0; y < kHeight; ++y)
        {
            const auto *row = base + std::size_t(y * height / kHeight) * pitch;
            for (int x = 0; x < kWidth; ++x)
            {
                const int sx = x * width / kWidth;
                out[y * kWidth + x] =
                    ten_bit ? reinterpret_cast<const std::uint16_t *>(row)[sx] & 1023 : row[sx] * 4;
            }
        }
        if (++count_ != kFrames)
            return;
        done_ = true;
        if (auto *f = std::fopen("/data/slopfin-order.ts", "wb"))
        {
            std::fwrite(input_, 1, used_, f);
            std::fclose(f);
        }
        if (auto *f = std::fopen("/data/slopfin-order.y16", "wb"))
        {
            std::fwrite(pictures_, 2, kFrames * kWidth * kHeight, f);
            std::fclose(f);
        }
        if (auto *f = std::fopen("/data/slopfin-order.txt", "wb"))
        {
            std::fprintf(f, "%d %d %d %d %d\n", width_, height_, kWidth, kHeight, count_);
            for (int i = 0; i < count_; ++i)
                std::fprintf(f, "%.6f\n", pts_[i]);
            std::fclose(f);
        }
    }
};
} // namespace slopfin::player
#endif
