/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "packet_queue.hpp"
#include <cassert>
#include <cstdlib>
#include <future>
#include <thread>
namespace slopfin::bigalloc
{
void *allocate(std::size_t n) noexcept
{
    return std::malloc(n);
}
void release(void *p) noexcept
{
    std::free(p);
}
} // namespace slopfin::bigalloc
int main()
{
    using namespace slopfin;
    std::atomic<bool> stop{false};
    player::PacketQueue queue(19);
    assert(queue.valid());
    auto producer = std::async(std::launch::async,
                               [&]
                               {
                                   for (int i = 0; i < 1000; ++i)
                                   {
                                       ts::AccessUnit unit;
                                       unit.data.assign(1 + i % 11, static_cast<std::uint8_t>(i));
                                       unit.pts = i * 3003;
                                       unit.dts = unit.pts - 6006;
                                       assert(queue.push(unit, ts::Codec::hevc, stop));
                                   }
                                   queue.close();
                               });
    for (int i = 0; i < 1000; ++i)
    {
        ts::AccessUnit unit;
        ts::Codec codec{};
        assert(queue.pop(unit, codec, stop));
        assert(unit.data == std::vector<std::uint8_t>(1 + i % 11, static_cast<std::uint8_t>(i)));
        assert(unit.pts == i * 3003 && unit.dts == unit.pts - 6006);
        assert(unit.video && codec == ts::Codec::hevc);
    }
    ts::AccessUnit out;
    ts::Codec codec{};
    assert(!queue.pop(out, codec, stop));
    producer.get();
    player::PacketQueue blocked(1);
    ts::AccessUnit unit;
    unit.data = {42};
    assert(blocked.push(unit, ts::Codec::h264, stop));
    auto waiting =
        std::async(std::launch::async, [&] { return blocked.push(unit, ts::Codec::h264, stop); });
    assert(waiting.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    stop.store(true);
    assert(waiting.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(!waiting.get());
    assert(!blocked.pop(out, codec, stop));
}
