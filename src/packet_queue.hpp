/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/* Bounded compressed-video queue. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_PACKET_QUEUE_HPP
#define SLOPFIN_PACKET_QUEUE_HPP
#include "bigalloc.hpp"
#include "tsdemux.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

namespace slopfin::player
{
// One transport producer and one video consumer. The byte ring lives outside
// the small process heap; descriptors never grow. Closing drains queued data.
class PacketQueue
{
    struct Entry
    {
        std::size_t bytes;
        std::int64_t pts, dts;
        ts::Codec codec;
    };
    std::array<Entry, 1024> entries_{};
    std::uint8_t *data_;
    const std::size_t capacity_;
    std::size_t read_ = 0, write_ = 0, used_ = 0, first_ = 0, count_ = 0;
    bool closed_ = false;
    std::mutex mutex_;
    std::condition_variable changed_;

  public:
    explicit PacketQueue(std::size_t capacity) noexcept
        : data_(static_cast<std::uint8_t *>(bigalloc::allocate(capacity))), capacity_(capacity)
    {
    }
    ~PacketQueue()
    {
        bigalloc::release(data_);
    }
    bool valid() const noexcept
    {
        return data_ != nullptr && capacity_ != 0;
    }
    void close() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        changed_.notify_all();
    }
    bool push(const ts::AccessUnit &unit, ts::Codec codec, const std::atomic<bool> &stop)
    {
        if (!valid() || unit.data.size() > capacity_)
            return false;
        std::unique_lock<std::mutex> lock(mutex_);
        while (!closed_ && !stop.load() &&
               (count_ == entries_.size() || unit.data.size() > capacity_ - used_))
            changed_.wait_for(lock, std::chrono::milliseconds(100));
        if (closed_ || stop.load())
            return false;
        const auto size = unit.data.size();
        const auto head = std::min(size, capacity_ - write_);
        if (size)
        {
            std::memcpy(data_ + write_, unit.data.data(), head);
            std::memcpy(data_, unit.data.data() + head, size - head);
        }
        write_ = (write_ + size) % capacity_;
        used_ += size;
        entries_[(first_ + count_) % entries_.size()] = {size, unit.pts, unit.dts, codec};
        ++count_;
        changed_.notify_all();
        return true;
    }
    bool pop(ts::AccessUnit &unit, ts::Codec &codec, const std::atomic<bool> &stop)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (count_ == 0 && !closed_ && !stop.load())
            changed_.wait_for(lock, std::chrono::milliseconds(100));
        if (stop.load() || count_ == 0)
            return false;
        const auto entry = entries_[first_];
        unit.data.resize(entry.bytes);
        const auto head = std::min(entry.bytes, capacity_ - read_);
        if (entry.bytes)
        {
            std::memcpy(unit.data.data(), data_ + read_, head);
            std::memcpy(unit.data.data() + head, data_, entry.bytes - head);
        }
        unit.pts = entry.pts;
        unit.dts = entry.dts;
        unit.video = true;
        codec = entry.codec;
        read_ = (read_ + entry.bytes) % capacity_;
        used_ -= entry.bytes;
        first_ = (first_ + 1) % entries_.size();
        --count_;
        changed_.notify_all();
        return true;
    }
};
} // namespace slopfin::player
#endif
