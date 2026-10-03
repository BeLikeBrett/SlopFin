/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
// Exercise the production PCM queue with a decoder stub and a stalled sink.
#include "../src/audio.cpp"
#include <cassert>
#include <future>
#include <thread>
extern "C" int sceAudiodecDecode(int, AudiodecControl *)
{
    g_info.channel_count = 2;
    g_info.sampling_frequency = 48000;
    g_pcm_item.length = 8; // two stereo frames
    const std::int16_t values[] = {101, 202, 303, 404};
    std::memcpy(g_pcm.data(), values, sizeof(values));
    return 0;
}
namespace slopfin::trace
{
void mark(std::string_view) noexcept
{
}
} // namespace slopfin::trace
int main()
{
    g_running.store(true);
    g_quit.store(false);
    g_codec = slopfin::audio::Codec::aac_adts;
    std::array<std::int16_t, 8> storage;
    storage.fill(9);
    g_ring = storage;
    g_filled = 8;
    g_read = g_write = 0;
    g_out_channels = 2;
    g_desired_channels.store(2);
    const std::uint8_t frame[] = {0xff, 0xf1, 0x4c, 0x80, 0, 0xe0, 0}; // ADTS length 7
    auto writer = std::async(std::launch::async,
                             [&] { slopfin::audio::submit_frames(frame, sizeof(frame), 90000); });
    assert(writer.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        assert(g_filled == 8); // full buffer was neither overwritten nor silently discarded
        g_filled = 0;
        g_space_changed.notify_all();
    }
    assert(writer.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    writer.get();
    assert(g_filled == 4 && g_ring[0] == 101 && g_ring[3] == 404);
    g_filled = g_ring.size();
    auto blocked = std::async(std::launch::async,
                              [&] { slopfin::audio::submit_frames(frame, sizeof(frame), 90000); });
    assert(blocked.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    slopfin::audio::interrupt();
    assert(blocked.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    blocked.get();
    // EOF drains a partial hardware grain without losing the last PCM frame.
    g_quit.store(false);
    g_grain = 2;
    g_filled = 2;
    g_write = 2;
    g_read = 0;
    g_ring[0] = 111;
    g_ring[1] = 222;
    g_ring[2] = g_ring[3] = 999;
    slopfin::audio::finish();
    assert(g_filled == 4 && g_ring[0] == 111 && g_ring[1] == 222);
    assert(g_ring[2] == 0 && g_ring[3] == 0);
    slopfin::audio::finish();
    assert(g_filled == 4);
}
