/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/* Optional host smoke: provide a verified model path. Uses SDL's dummy capture
 * so cancellation/mute handling can be tested without opening a real mic. */
#include "../src/voice_search.cpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

using namespace slopfin;
template <typename F> bool wait_for(F predicate, int seconds = 20)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return predicate();
}
int main(int argc, char **argv)
{
    assert(trusted_url("https://huggingface.co/model"));
    assert(trusted_url("https://cas-bridge.xethub.hf.co/file"));
    assert(!trusted_url("http://huggingface.co/model"));
    assert(!trusted_url("https://huggingface.co.evil.example/model"));
    assert(!trusted_url("https://evil.example@huggingface.co/file"));
    assert(!valid_model("/nonexistent/slopfin-voice-model"));
    if (argc < 2)
    {
        std::cout << "Voice download URL restrictions passed; model smoke not requested\n";
        return 0;
    }
    const auto data = config::data_directory();
    std::filesystem::create_directories(data + "/voice");
    const bool download = std::string_view(argv[1]) == "--download";
    if (!download)
    {
        assert(valid_model(argv[1]));
        std::filesystem::copy_file(argv[1], data + "/voice/ggml-tiny-q5_1.bin",
                                   std::filesystem::copy_options::overwrite_existing);
    }
    assert(voice_search::start());
    assert(!voice_search::start()); // No overlapping capture/download workers.
    assert(wait_for([] { return voice_search::snapshot().phase == voice_search::Phase::listening; },
                    download ? 120 : 20));
    assert(valid_model(data + "/voice/ggml-tiny-q5_1.bin"));
    voice_search::cancel();
    assert(wait_for([] { return !voice_search::busy(); }));
    assert(voice_search::snapshot().phase == voice_search::Phase::idle);
    std::string result;
    assert(!voice_search::take_result(result));
    assert(voice_search::start());
    assert(
        wait_for([] { return voice_search::snapshot().phase == voice_search::Phase::listening; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    voice_search::finish();
    assert(wait_for([] { return !voice_search::busy(); }));
    assert(voice_search::snapshot().phase == voice_search::Phase::error);
    std::cerr << "Silent capture status: " << voice_search::snapshot().message << '\n';
    assert(voice_search::snapshot().message.find("No speech") != std::string::npos);
    assert(!voice_search::take_result(result));
    voice_search::shutdown();
    SDL_Quit();
    std::cout << "Real model verification/loading, capture cancellation/restart and silence smoke "
                 "passed\n";
}
