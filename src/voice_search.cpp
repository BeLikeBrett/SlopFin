/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "voice_search.hpp"
#include "voice_search_policy.hpp"

#include "config.hpp"
#include "bigalloc.hpp"
#include "images.hpp"
#include "trace.hpp"
#include "update/sha256.h"
#include "web_transport.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#ifdef SLOPFIN_VOICE_SEARCH
#include <whisper.h>
#endif

#if defined(SLOPFIN_VOICE_SEARCH) && !defined(SLOPFIN_HOST)
/* The native ggml model and compute arenas can exceed the ordinary process
 * heap. The dependency's paired hooks always use flexible-memory mappings. */
extern "C" void *slopfin_voice_allocate(std::size_t bytes) noexcept
{
    return slopfin::bigalloc::allocate(bytes);
}
extern "C" void slopfin_voice_free(void *pointer) noexcept
{
    slopfin::bigalloc::release(pointer);
}
#endif
#ifdef SLOPFIN_HOST
#include <SDL.h>
#else
extern "C"
{
    int sceKernelLoadStartModule(const char *, std::size_t, const void *, unsigned, const void *,
                                 int *);
    int sceKernelDlsym(int, const char *, void *);
    int sceUserServiceGetInitialUser(int *);
    int sceUserServiceGetForegroundUser(int *);
    int scePthreadCreate(void **, const void *, void *(*)(void *), void *, const char *);
    int scePthreadJoin(void *, void **);
    int scePthreadSetaffinity(void *, std::uint64_t);
}
#endif

namespace
{
using namespace slopfin;
using voice_search::Phase;
using voice_search::detail::kGrain;
using voice_search::detail::kRate;
constexpr std::uint64_t kModelBytes = 32152673;
constexpr char kModelDigest[] = "818710568da3ca15689e31a743197b520007872ff9576237bda97bd1b469c3d7";
constexpr char kModelUrl[] = "https://huggingface.co/ggerganov/whisper.cpp/resolve/"
                             "5359861c739e955e79d9a303bcbc70fb988958b1/ggml-tiny-q5_1.bin";
std::mutex g_mutex;
voice_search::Snapshot g_state;
std::string g_result;
std::atomic<bool> g_running{false}, g_cancel{false}, g_finish{false};
http::WebRequest *g_request = nullptr; // Borrowed only while g_mutex is held.
#ifdef SLOPFIN_HOST
pthread_t g_thread{};
#else
void *g_thread = nullptr;
#endif
bool g_joinable = false;

void state(Phase phase, const char *message, float progress = 0)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_cancel || phase == Phase::idle)
        g_state = {phase, message, progress, 0, false};
}
bool fail(const char *message)
{
    state(Phase::error, message);
    return false;
}
bool valid_model(const std::string &path)
{
    const int file = open(path.c_str(), O_RDONLY);
    if (file < 0)
        return false;
    struct stat st
    {
    };
    if (fstat(file, &st) != 0 || !S_ISREG(st.st_mode) ||
        static_cast<std::uint64_t>(st.st_size) != kModelBytes)
    {
        close(file);
        return false;
    }
    sf_sha256 hash;
    sf_sha_init(&hash);
    std::array<unsigned char, 65536> block{};
    ssize_t count = 0;
    while (!g_cancel)
    {
        count = read(file, block.data(), block.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        sf_sha_add(&hash, block.data(), static_cast<std::size_t>(count));
    }
    const bool ok = count == 0 && !g_cancel;
    close(file);
    char digest[65]{};
    sf_sha_hex(&hash, digest);
    return ok && std::strcmp(digest, kModelDigest) == 0;
}
bool trusted_url(std::string_view url)
{
    // Hugging Face redirects its immutable LFS objects to its Xet CDN. No
    // credentials are sent; every byte is authenticated by the pinned digest.
    constexpr std::string_view prefix = "https://";
    if (!url.starts_with(prefix))
        return false;
    auto host = url.substr(prefix.size());
    host = host.substr(0, host.find('/'));
    return host == "huggingface.co" || host == "cdn-lfs.huggingface.co" ||
           host.ends_with(".xethub.hf.co") || host.ends_with(".hf.co");
}
bool model_ready(const std::string &path)
{
    if (valid_model(path))
        return true;
    if (g_cancel)
        return false;
    state(Phase::downloading, "Downloading the voice model (32 MB)…");
    const std::string partial = path + ".partial";
    const int file = open(partial.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (file < 0)
        return fail("Could not save the voice model. Check available storage.");
    bool ok = false;
    std::uint64_t received = 0;
    sf_sha256 hash;
    sf_sha_init(&hash);
    std::string url = kModelUrl;
    for (int redirect = 0; redirect < 6 && !g_cancel && trusted_url(url); ++redirect)
    {
        auto request = std::make_unique<http::WebRequest>();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_request = request.get();
        }
        const bool opened = request->open(url, "GET", {}, {}, {}, true);
        if (opened && request->status() == 200)
        {
            std::array<unsigned char, 65536> block{};
            long count = 0;
            while (!g_cancel && (count = request->read(block.data(), block.size())) > 0)
            {
                if (received + static_cast<unsigned long>(count) > kModelBytes)
                    break;
                std::size_t written = 0;
                while (!g_cancel && written < static_cast<std::size_t>(count))
                {
                    const auto amount = write(file, block.data() + written,
                                              static_cast<std::size_t>(count) - written);
                    if (amount < 0 && errno == EINTR)
                        continue;
                    if (amount <= 0)
                        break;
                    written += static_cast<std::size_t>(amount);
                }
                if (written != static_cast<std::size_t>(count))
                    break;
                sf_sha_add(&hash, block.data(), static_cast<std::size_t>(count));
                received += static_cast<std::uint64_t>(count);
                std::lock_guard<std::mutex> lock(g_mutex);
                if (!g_cancel)
                    g_state.progress =
                        static_cast<float>(received) / static_cast<float>(kModelBytes);
            }
            ok = !g_cancel && count == 0 && received == kModelBytes;
        }
        const int status = request->status();
        url = request->location();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_request = nullptr;
        }
        request.reset();
        if (ok || status == 200 ||
            !(status == 301 || status == 302 || status == 303 || status == 307 || status == 308))
            break;
    }
    char digest[65]{};
    sf_sha_hex(&hash, digest);
    ok = ok && std::strcmp(digest, kModelDigest) == 0;
    if (fsync(file) != 0)
        ok = false;
    if (close(file) != 0)
        ok = false;
    if (ok && !g_cancel && rename(partial.c_str(), path.c_str()) == 0)
        return true;
    unlink(partial.c_str());
    return g_cancel ? false
                    : fail("Voice model download failed. Check the internet and try again.");
}

class Microphone
{
  public:
    bool open()
    {
#ifdef SLOPFIN_HOST
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
            return false;
        initialized_ = true;
        SDL_AudioSpec wanted{}, actual{};
        wanted.freq = kRate;
        wanted.format = AUDIO_S16SYS;
        wanted.channels = 1;
        wanted.samples = kGrain;
        device_ = SDL_OpenAudioDevice(nullptr, SDL_TRUE, &wanted, &actual, 0);
        if (!device_)
            return false;
        SDL_PauseAudioDevice(device_, 0);
        return true;
#else
        // Resolve optional AudioIn lazily, so missing firmware support never
        // prevents ordinary browsing or playback from launching.
        static int module = -1;
        if (module < 0)
        {
            int result = 0;
            module = sceKernelLoadStartModule("/system/common/lib/libSceAudioIn.sprx", 0, nullptr,
                                              0, nullptr, &result);
        }
        if (module < 0 || !symbol(module, "sceAudioInOpen", open_) ||
            !symbol(module, "sceAudioInInput", input_) ||
            !symbol(module, "sceAudioInClose", close_) ||
            !symbol(module, "sceAudioInGetSilentState", silent_))
            return false;
        int (*initialize)() = nullptr;
        if (symbol(module, "sceAudioInInit", initialize))
            (void)initialize();
        int user = -1;
        if (sceUserServiceGetForegroundUser(&user) < 0 || user <= 0)
            if (sceUserServiceGetInitialUser(&user) < 0 || user <= 0)
                return false;
        handle_ = open_(user, 5, 0, kGrain, kRate, 0); // Voice recognition.
        if (handle_ < 0)
            handle_ = open_(user, 1, 0, kGrain, kRate, 0); // General user input.
        return handle_ >= 0;
#endif
    }
    bool read(std::array<std::int16_t, kGrain> &block, bool &muted)
    {
#ifdef SLOPFIN_HOST
        while (!g_cancel && !g_finish && SDL_GetQueuedAudioSize(device_) < sizeof(block))
            SDL_Delay(8);
        if (g_cancel || g_finish)
            return false;
        muted = false;
        std::size_t filled = 0;
        auto *destination = reinterpret_cast<unsigned char *>(block.data());
        while (filled < sizeof(block) && !g_cancel && !g_finish)
        {
            filled += SDL_DequeueAudio(device_, destination + filled,
                                       static_cast<Uint32>(sizeof(block) - filled));
            if (filled < sizeof(block))
                SDL_Delay(4);
        }
        return filled == sizeof(block);
#else
        if (input_(handle_, block.data()) < 0)
            return false;
        const int status = silent_(handle_);
        // Treat an unknown microphone state as muted instead of risking
        // processing audio while the platform says capture is unavailable.
        muted = status < 0 || status != 0;
        return true;
#endif
    }
    ~Microphone()
    {
#ifdef SLOPFIN_HOST
        if (device_)
            SDL_CloseAudioDevice(device_);
        if (initialized_)
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
#else
        if (handle_ >= 0)
            (void)close_(handle_);
#endif
    }

  private:
#ifdef SLOPFIN_HOST
    SDL_AudioDeviceID device_ = 0;
    bool initialized_ = false;
#else
    template <typename T> static bool symbol(int module, const char *name, T &out)
    {
        void *address = nullptr;
        if (sceKernelDlsym(module, name, &address) < 0 || !address)
            return false;
        out = reinterpret_cast<T>(address);
        return true;
    }
    int handle_ = -1;
    int (*open_)(int, int, int, unsigned, unsigned, unsigned) = nullptr;
    int (*input_)(int, void *) = nullptr;
    int (*close_)(int) = nullptr;
    int (*silent_)(int) = nullptr;
#endif
};

bool capture(std::vector<float> &samples)
{
    Microphone mic;
    if (!mic.open())
        return fail("Microphone unavailable. Check PS5 microphone settings.");
    voice_search::detail::CaptureGate gate;
    state(Phase::listening, "Speak a title…");
    samples.reserve(voice_search::detail::kMaxSamples);
    std::array<std::int16_t, kGrain> block{};
    while (!g_cancel && !g_finish)
    {
        bool muted = false;
        if (!mic.read(block, muted))
        {
            if (g_cancel || g_finish)
                break;
            return fail("Microphone disconnected. Check PS5 microphone settings.");
        }
        const bool keep = gate.accept(block, muted);
        for (auto sample : block)
            samples.push_back(sample / 32768.0f);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_cancel)
            {
                g_state.level = gate.level;
                g_state.muted = muted;
                g_state.message =
                    muted ? "Microphone muted. Unmute it to speak." : "Speak a title…";
            }
        }
        if (!keep)
            break;
    }
    if (g_cancel)
        return false;
    return gate.heard_speech() || fail("No speech heard. Unmute your microphone and try again.");
}

void *run(void *) noexcept
{
#ifdef SLOPFIN_VOICE_SEARCH
    const std::string directory = config::data_directory() + "/voice";
    (void)mkdir(directory.c_str(), 0700);
    const std::string path = directory + "/ggml-tiny-q5_1.bin";
    if (model_ready(path) && !g_cancel)
    {
        state(Phase::preparing, "Preparing voice search…");
        // Whisper's diagnostic output contains timings and could include
        // recognised words; the search query is not sent to the app log.
        whisper_log_set([](enum ggml_log_level, const char *, void *) {}, nullptr);
        auto params = whisper_context_default_params();
        params.use_gpu = false;
        whisper_context *context = whisper_init_from_file_with_params(path.c_str(), params);
        if (!context)
            fail("Could not load the voice model. Try again after restarting SlopFin.");
        else
        {
            std::vector<float> samples;
            if (!g_cancel && capture(samples) && !g_cancel)
            {
                state(Phase::processing, "Recognising on your PS5…");
                auto options = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
                options.n_threads = 4;
                options.translate = false;
                options.language = "auto";
                options.no_context = true;
                options.single_segment = true;
                options.print_realtime = false;
                options.print_progress = false;
                options.print_timestamps = false;
                options.print_special = false;
                options.suppress_nst = true;
                options.max_tokens = 64;
                options.temperature_inc = 0;
                options.abort_callback = [](void *) { return g_cancel.load(); };
                options.progress_callback =
                    [](whisper_context *, whisper_state *, int value, void *)
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    if (!g_cancel)
                        g_state.progress = std::clamp(value / 100.0f, 0.0f, 1.0f);
                };
                const int code = whisper_full(context, options, samples.data(),
                                              static_cast<int>(samples.size()));
                if (!g_cancel)
                {
                    std::string result;
                    if (code == 0)
                        for (int i = 0; i < whisper_full_n_segments(context); ++i)
                            result += whisper_full_get_segment_text(context, i);
                    result = voice_search::detail::clean_query(result);
                    if (result.empty())
                        fail("Didn't catch that. Try saying the title again.");
                    else
                    {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        if (!g_cancel)
                        {
                            g_result = std::move(result);
                            g_state = {Phase::complete, "Ready", 1, 0, false};
                        }
                    }
                }
            }
            // Neither captured PCM nor its float representation is written to disk.
            std::fill(samples.begin(), samples.end(), 0.0f);
            whisper_free(context);
        }
    }
#else
    fail("Voice search is unavailable in this build.");
#endif
    if (g_cancel)
        state(Phase::idle, "");
#ifdef SLOPFIN_HOST
    // This worker was created by pthreads rather than SDL_CreateThread.
    SDL_TLSCleanup();
#endif
    g_running = false;
    return nullptr;
}
void join()
{
    if (!g_joinable)
        return;
#ifdef SLOPFIN_HOST
    (void)pthread_join(g_thread, nullptr);
#else
    (void)scePthreadJoin(g_thread, nullptr);
#endif
    g_joinable = false;
}
} // namespace

namespace slopfin::voice_search
{
bool start() noexcept
{
    if (g_running.exchange(true))
        return false;
    join(); // Previous worker already completed; never wait on recognition here.
    g_cancel = false;
    g_finish = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_result.clear();
        g_state = {Phase::preparing, "Preparing voice search…", 0, 0, false};
    }
    pthread_attr_t attributes;
    const bool have_attributes = pthread_attr_init(&attributes) == 0;
    if (have_attributes)
        (void)pthread_attr_setstacksize(&attributes, 4u * 1024u * 1024u);
#ifdef SLOPFIN_HOST
    const int rc = pthread_create(&g_thread, have_attributes ? &attributes : nullptr, run, nullptr);
#else
    const int rc = scePthreadCreate(&g_thread, have_attributes ? &attributes : nullptr, run,
                                    nullptr, "slopfin-voice");
#endif
    if (have_attributes)
        (void)pthread_attr_destroy(&attributes);
    if (rc != 0)
    {
        g_running = false;
        return fail("Could not start voice search. Try again.");
    }
    g_joinable = true;
#ifndef SLOPFIN_HOST
    if (const auto spare = images::spare_cores(); spare)
        (void)scePthreadSetaffinity(g_thread, spare);
#endif
    return true;
}
void finish() noexcept
{
    g_finish = true;
}
void cancel() noexcept
{
    g_cancel = true;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_request)
        g_request->interrupt();
    g_result.clear();
    g_state = {};
}
Snapshot snapshot() noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state;
}
bool busy() noexcept
{
    return g_running;
}
bool take_result(std::string &out) noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_result.empty())
        return false;
    out = std::move(g_result);
    g_result.clear();
    g_state = {};
    return true;
}
void shutdown() noexcept
{
    cancel();
    join();
}
} // namespace slopfin::voice_search
