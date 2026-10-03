/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
// Exercise real audio::stop against a blocked hardware write.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/audio.cpp"
#include <cassert>
#include <condition_variable>
#include <thread>

namespace
{
std::mutex test_mutex;
std::condition_variable test_condition;
bool write_entered = false, release_write = false, join_entered = false;
bool drained = false, closed = false, expect_preserved = false, unloaded = false;
} // namespace
extern "C" int scePthreadJoin(void *thread, void **)
{
    {
        std::lock_guard<std::mutex> lock(test_mutex);
        join_entered = true;
        test_condition.notify_all();
    }
    static_cast<std::thread *>(thread)->join();
    return 0;
}
extern "C" int sceAudioOutOutput(int, const void *samples)
{
    std::unique_lock<std::mutex> lock(test_mutex);
    assert(!closed);
    if (samples == nullptr)
    {
        drained = true;
        return 0;
    }
    write_entered = true;
    test_condition.notify_all();
    test_condition.wait(lock, [] { return release_write; });
    return 0;
}
extern "C" int sceAudioOutClose(int)
{
    std::lock_guard<std::mutex> lock(test_mutex);
    assert(drained);
    closed = true;
    return 0;
}
extern "C" int sceAudioOutOpen(int, int, int, std::uint32_t, std::uint32_t, std::uint32_t)
{
    return 1;
}
extern "C" int sceAudioOutSetVolume(int, int, const int *)
{
    return 0;
}
extern "C" int sceAudioOutExClose(int)
{
    return 0;
}
extern "C" int sceAudioOutExConfigureOutput(int, long, int, int, long)
{
    return 0;
}
extern "C" int sceAudiodecDeleteDecoder(int)
{
    assert(expect_preserved ? !closed : closed);
    return 0;
}
extern "C" int sceAudiodecTermLibrary(std::uint32_t)
{
    assert(expect_preserved ? !closed : closed);
    return 0;
}
extern "C" int sceSysmoduleUnloadModule(std::uint16_t)
{
    assert(closed);
    unloaded = true;
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
    const auto prepare = []
    {
        std::lock_guard<std::mutex> lock(test_mutex);
        write_entered = release_write = join_entered = false;
        drained = closed = unloaded = false;
        g_sink = 1;
        g_decoder = 1;
        g_bitstream = false;
        g_output_preserved = false;
        g_preserve_request.store(false);
        g_library_ready = g_module_loaded = true;
        g_running.store(true);
        g_quit.store(false);
    };
    const auto wait_for_write = []
    {
        std::unique_lock<std::mutex> lock(test_mutex);
        test_condition.wait(lock, [] { return write_entered; });
    };
    const auto release_blocked_write = []
    {
        std::unique_lock<std::mutex> lock(test_mutex);
        test_condition.wait(lock, [] { return join_entered; });
        assert(!closed);
        release_write = true;
        test_condition.notify_all();
    };

    /* A seek joins the worker and resets decoder state, but the physical audio
       port remains open so the HDMI format does not renegotiate. */
    prepare();
    expect_preserved = true;
    std::thread seek_output([] { output_entry(nullptr); });
    g_thread = &seek_output;
    wait_for_write();
    std::thread seek_stopper([] { slopfin::audio::stop(true); });
    release_blocked_write();
    seek_stopper.join();
    assert(!closed && !drained && g_output_preserved && g_sink == 1 && g_module_loaded &&
           !unloaded);
    assert(g_thread == nullptr && !g_running.load());
    expect_preserved = false;
    slopfin::audio::stop();
    assert(closed && unloaded && g_sink == -1 && !g_output_preserved);

    /* A real stop still waits for the final hardware write before closing the
       port and unloading the module. */
    prepare();
    std::thread output([] { output_entry(nullptr); });
    g_thread = &output;
    wait_for_write();
    std::thread stopper([] { slopfin::audio::stop(); });
    release_blocked_write();
    stopper.join();
    assert(closed && drained && g_thread == nullptr && !g_running.load());
    // Repeated stop is safe after all resources are gone.
    slopfin::audio::stop();
    std::puts("Audio seek restart preserves HDMI output; full stop closes it safely");
}
