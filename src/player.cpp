/*
 * SlopFin - video playback.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Videodec2 sequence and its structure layouts follow ProsperoTV's
 * iptv_native_backend.c, copyright BlackBearReloaded, GPL-3.0-or-later.
 */

#include "player.hpp"

#include "audio.hpp"
#include "bigalloc.hpp"
#include "config.hpp"
#include "http.hpp"
#include "images.hpp"
#include "gfx.hpp"
#include "hdr.hpp"
#include "hevc_base_layer.hpp"
#include "media_bitrate.hpp"
#include "packet_queue.hpp"
#include "jellyfin.hpp"
#include "presentation_queue.hpp"
#include "reporter.hpp"
#include "stream_clock.hpp"
#include "subtitles.hpp"
#include "order_capture.hpp"
#include "trace.hpp"
#include "tsdemux.hpp"
#include "video_sps.hpp"

#include <algorithm>
#include <atomic>
#include <deque>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <string>
#include <vector>

extern "C"
{
    int unlink(const char *path);
}

extern "C"
{
    int sceKernelUsleep(std::uint32_t microseconds);
    int sceKernelGettimeofday(void *timeval);
    int scePthreadCreate(void **thread, const void *attr, void *(*entry)(void *), void *argument,
                         const char *name);
    int scePthreadJoin(void *thread, void **result);
    void *scePthreadSelf();
    int scePthreadSetaffinity(void *thread, std::uint64_t mask);
    int sceSysmoduleLoadModule(std::uint16_t module_id);
    int sceSysmoduleUnloadModule(std::uint16_t module_id);
    std::size_t sceKernelGetDirectMemorySize();
    int sceKernelAllocateDirectMemory(std::int64_t search_start, std::int64_t search_end,
                                      std::size_t length, std::size_t alignment, int memory_type,
                                      std::int64_t *physical_address);
    int sceKernelMapDirectMemory(void **address, std::size_t length, int protection, int flags,
                                 std::int64_t physical_address, std::size_t alignment);
    int sceKernelReleaseDirectMemory(std::int64_t start, std::size_t length);
    int sceKernelMunmap(void *address, std::size_t length);
    int sceKernelMapNamedFlexibleMemory(void **address, std::size_t length, int protection,
                                        int flags, const char *name);
    int sceKernelAvailableFlexibleMemorySize(std::size_t *size);
}

namespace
{
using namespace slopfin;

/* ------------------------------------------------- Videodec2 ABI */

struct Videodec2DecoderConfig
{
    std::uint64_t size;
    std::uint32_t resource_type, codec_type, profile, max_level;
    std::int32_t max_width, max_height, max_dpb_frames;
    std::uint32_t pipeline_depth;
    std::uint64_t compute_queue, cpu_affinity;
    std::int32_t cpu_priority;
    std::uint8_t optimize_progressive, check_memory_type, reserved0, reserved1;
    void *extra_config;
};

struct Videodec2DecoderMemory
{
    std::uint64_t size, cpu_size;
    void *cpu;
    std::uint64_t gpu_size;
    void *gpu;
    std::uint64_t cpu_gpu_size;
    void *cpu_gpu;
    std::uint64_t max_frame_size;
    std::uint32_t frame_alignment, reserved;
};

struct Videodec2ComputeConfig
{
    std::uint64_t size;
    std::uint16_t pipe_id, queue_id;
    std::uint8_t check_memory_type, reserved0;
    std::uint16_t reserved1;
};

struct Videodec2ComputeMemory
{
    std::uint64_t size, cpu_gpu_size;
    void *cpu_gpu;
};

struct Videodec2Input
{
    std::uint64_t size;
    void *au;
    std::uint64_t au_size, pts, dts, attached;
};

struct Videodec2Frame
{
    std::uint64_t size;
    void *buffer;
    std::uint64_t buffer_size;
    std::uint32_t accepted, reserved;
};

struct Videodec2Output
{
    std::uint64_t size;
    std::uint8_t valid, error, picture_count, padding;
    std::uint32_t codec, width, pitch, height, reserved;
    void *buffer;
    std::uint64_t buffer_size;
    std::uint32_t frame_format, pitch_bytes;
};

static_assert(sizeof(Videodec2DecoderConfig) == 72, "unexpected Videodec2 config layout");
static_assert(sizeof(Videodec2DecoderMemory) == 72, "unexpected Videodec2 memory layout");
static_assert(sizeof(Videodec2ComputeConfig) == 16, "unexpected Videodec2 compute config layout");
static_assert(sizeof(Videodec2ComputeMemory) == 24, "unexpected Videodec2 compute memory layout");
static_assert(sizeof(Videodec2Input) == 48, "unexpected Videodec2 input layout");
static_assert(sizeof(Videodec2Frame) == 32, "unexpected Videodec2 frame layout");
static_assert(sizeof(Videodec2Output) == 56, "unexpected Videodec2 output layout");

extern "C"
{
    std::int32_t sceVideodec2QueryComputeMemoryInfo(Videodec2ComputeMemory *memory);
    std::int32_t sceVideodec2AllocateComputeQueue(const Videodec2ComputeConfig *config,
                                                  const Videodec2ComputeMemory *memory,
                                                  void **queue);
    std::int32_t sceVideodec2ReleaseComputeQueue(void *queue);
    std::int32_t sceVideodec2QueryDecoderMemoryInfo(const Videodec2DecoderConfig *config,
                                                    Videodec2DecoderMemory *memory);
    std::int32_t sceVideodec2CreateDecoder(const Videodec2DecoderConfig *config,
                                           const Videodec2DecoderMemory *memory, void **decoder);
    std::int32_t sceVideodec2DeleteDecoder(void *decoder);
    std::int32_t sceVideodec2Decode(void *decoder, Videodec2Input *input, Videodec2Frame *frame,
                                    Videodec2Output *output);
    std::int32_t sceVideodec2Reset(void *decoder);
}

constexpr std::uint16_t kVideoModuleId = 207;

/*
 * The decoder's codec identifiers. Measured with probe_decoder_support: this
 * hardware configures H.264 High to 4K, HEVC Main to 4K, and HEVC Main 10 to 4K
 * at levels 5.1 and 6.0. Main 10 doubles the frame buffer (12537856 to 25071616
 * bytes at 4K), which is P010 -- two bytes per sample -- so a 10-bit source can
 * be decoded natively rather than transcoded. Profile 3 is refused.
 */
constexpr std::uint32_t kCodecH264 = 1;
constexpr std::uint32_t kCodecHevc = 0x000ee049;
constexpr std::uint32_t kProfileH264High = 100;
constexpr std::uint32_t kProfileHevcMain = 1;
constexpr std::uint32_t kProfileHevcMain10 = 2;
/*
 * More slots than the reference uses. An accepted frame buffer stays owned by
 * the decoder while it is a reference picture, so reusing one too soon corrupts
 * what it is still reading from.
 */
/*
 * More frame buffers than the decoder's picture buffer holds. If a supplied
 * buffer is retained as a reference picture, recycling it before the decoder
 * has finished with it corrupts every frame that predicts from it.
 */
/*
 * Comfortably more surfaces than the decoder's picture buffer holds. Pictures
 * one and twelve decode cleanly and later ones do not, which is what recycling
 * a surface the decoder still holds as a reference looks like.
 */
constexpr std::size_t kPipelineSlots = 16;
/* Keep decoder pipeline depth at 3: depth 1 serializes slice-free 4K streams and severely reduces
 * throughput. */
constexpr std::uint32_t kDecoderDepth = 3;
/* One coded frame: about 100 KiB at 1080p, well under a megabyte even at 4K. */
constexpr std::size_t kInputSlotBytes = 0x200000;
/*
 * 0x32 and 0x33 are mapping *protections*, not memory types: 0x32 is GPU
 * read/write, 0x33 adds CPU write. Direct memory for the decoder is always
 * allocated as type 12 with 16 KiB alignment. Passing a protection where the
 * type belongs yields memory the decoder will happily write outside of, which
 * takes the whole console down.
 */
constexpr int kDirectMemoryType = 12;
constexpr std::size_t kDirectAlignment = 0x4000;
constexpr int kProtectGpu = 0x32;
constexpr int kProtectCpuGpu = 0x33;

/*
 * Direct memory is mapped write-combining, so a plain memcpy can still be
 * sitting in the write-combine buffers when the decoder's own engine reads it.
 * Flushing the range and fencing makes the access unit visible before the
 * decode is asked for; without it the decoder reads a partly-written frame and
 * produces exactly the patchwork of good and broken macroblocks seen here.
 */
void flush_range(const void *address, std::size_t length) noexcept
{
    const auto *at = static_cast<const std::uint8_t *>(address);
    const auto *end = at + length;
    for (; at < end; at += 64)
        __asm__ volatile("clflush (%0)" : : "r"(at) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

/* Makes writes the hardware performed visible to this core's reads. */
void invalidate_range(const void *address, std::size_t length) noexcept
{
    const auto *at = static_cast<const std::uint8_t *>(address);
    const auto *end = at + length;
    __asm__ volatile("mfence" ::: "memory");
    for (; at < end; at += 64)
        __asm__ volatile("clflush (%0)" : : "r"(at) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

std::size_t align_16k(std::size_t value) noexcept
{
    constexpr std::size_t k = 16u * 1024u;
    return (value + k - 1u) & ~(k - 1u);
}

/* A block of direct memory, mapped for the decoder's use. */
struct DirectBlock
{
    void *address = nullptr;
    std::int64_t physical = 0;
    std::size_t size = 0;

    bool allocate(std::size_t bytes, int protection, std::int64_t limit) noexcept
    {
        if (bytes == 0 || limit <= 0)
            return false;
        size = align_16k(bytes);
        if (sceKernelAllocateDirectMemory(0, limit, size, kDirectAlignment, kDirectMemoryType,
                                          &physical) != 0)
        {
            size = 0;
            return false;
        }
        if (sceKernelMapDirectMemory(&address, size, protection, 0, physical, kDirectAlignment) !=
            0)
        {
            (void)sceKernelReleaseDirectMemory(physical, size);
            address = nullptr;
            size = 0;
            return false;
        }
        return true;
    }

    void release() noexcept
    {
        if (address != nullptr)
            (void)sceKernelMunmap(address, size);
        if (size != 0)
            (void)sceKernelReleaseDirectMemory(physical, size);
        address = nullptr;
        size = 0;
    }
};

/* ------------------------------------------------- frame delivery */

/* Bound the converted-frame pool; block the decoder until the presentation thread releases a slot.
 */
constexpr std::size_t kFramePool = 6;

enum class SlotState : std::uint8_t
{
    free = 0,
    ready,
    displaying,
};

struct PoolFrame
{
    gfx::Bitmap bitmap;
    std::size_t capacity = 0;
    double seconds = 0.0;
    SlotState state = SlotState::free;
};

std::mutex g_mutex;
PoolFrame g_pool[kFramePool];

/* Set from the server's negotiation: the picture needs tone mapping. */
bool g_stream_is_hdr = false;

/* Bit depth the server said the source carries; decides the decoder profile. */
int g_stream_bit_depth = 8;
/* Source picture size from the server, 0 when it did not say. */
int g_stream_width = 0, g_stream_height = 0;

/* Measured display interval for hold statistics. Live presentation selects the
 * newest picture due on the audio clock; cadence.hpp is an unused experiment.
 * Producer stalls and audio underruns must be measured independently of these
 * display-side counters. */
double g_display_hz = 0.0;
std::uint64_t g_cadence_last_us = 0;

/* Wall clock at which stream position `g_clock_origin` was shown. */
std::uint64_t g_clock_started_us = 0;
double g_clock_origin = -1.0;
std::uint64_t g_last_shown_us = 0;
std::uint64_t g_rate_started_us = 0;
long g_rate_started_shown = 0;

player::Status g_status;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_running{false};
/* Set only while a seek is stopping the old worker. The stream/decoders die,
   but HDMI HDR/audio modes remain in place for the replacement worker. */
std::atomic<bool> g_preserve_outputs_on_exit{false};
void *g_thread = nullptr;

jellyfin::PlaybackRequest g_request;
std::string g_title;
double g_duration_seconds = 0.0;
double g_start_seconds = 0.0;
/* Set per stream by the negotiation: timestamps already run on title time. */
bool g_absolute_timestamps = false;
/* False only for a file played directly, which the server's muxer never touched. */
bool g_server_muxed = false;
std::atomic<bool> g_paused{false};
std::uint64_t g_pause_started_us = 0;

audio::Codec delivered_audio_codec(const jellyfin::PlaybackPlan &plan) noexcept
{
    if (plan.direct)
        return plan.audio_codec == "dts"      ? audio::Codec::dts
               : plan.audio_codec == "truehd" ? audio::Codec::truehd
               : plan.audio_codec == "eac3"   ? audio::Codec::eac3
               : plan.audio_codec == "ac3"    ? audio::Codec::ac3
               : plan.audio_codec == "mp3"    ? audio::Codec::mp3
                                              : audio::Codec::aac_adts;
    std::string lowered = plan.path;
    for (char &c : lowered)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    if (lowered.find("audiocodec=dts") != std::string::npos)
        return audio::Codec::dts;
    if (lowered.find("audiocodec=truehd") != std::string::npos)
        return audio::Codec::truehd;
    if (lowered.find("audiocodec=eac3") != std::string::npos)
        return audio::Codec::eac3;
    if (lowered.find("audiocodec=ac3") != std::string::npos)
        return audio::Codec::ac3;
    return lowered.find("audiocodec=mp3") != std::string::npos ? audio::Codec::mp3
                                                               : audio::Codec::aac_adts;
}

void set_state(player::State state, const std::string &message) noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    g_status.state = state;
    g_status.message = message;
}

/* ------------------------------------------------- subtitle fetching */

/*
 * The first request for an embedded subtitle makes the server extract it from
 * the whole file, which can take several seconds. That must never run on the
 * decode thread, which would starve the picture, or on the render thread,
 * which would freeze the interface, so subtitles have a thread of their own.
 */
std::mutex g_subtitle_mutex;
std::string g_subtitle_pending;
std::string g_subtitle_loaded;
int g_subtitle_generation = 0;
std::atomic<bool> g_subtitle_thread{false};

void *subtitle_entry(void *) noexcept
{
    for (;;)
    {
        (void)sceKernelUsleep(100000);
        std::string path;
        int generation = 0;
        {
            std::lock_guard<std::mutex> guard(g_subtitle_mutex);
            path.swap(g_subtitle_pending);
            generation = g_subtitle_generation;
        }
        if (path.empty())
            continue;
        /*
         * The first request for an embedded subtitle makes the server extract
         * it from the file, and it answers with an error until that finishes.
         * One attempt therefore fails on exactly the titles that have never
         * been watched with subtitles before.
         */
        std::string body;
        for (int attempt = 0; attempt < 6 && body.empty(); ++attempt)
        {
            if (attempt > 0)
                (void)sceKernelUsleep(2000000);
            body = jellyfin::fetch_text(path);
        }
        std::vector<subtitles::Cue> cues = subtitles::parse_srt(body);
        trace::mark("player: subtitles " + std::to_string(cues.size()) + " cues from " +
                    std::to_string(body.size()) + " bytes");
        std::lock_guard<std::mutex> guard(g_subtitle_mutex);
        /* A newer choice may have arrived while this one was downloading. */
        if (generation == g_subtitle_generation)
        {
            subtitles::set(std::move(cues));
            g_subtitle_loaded = path;
        }
    }
    return nullptr;
}

void request_subtitles(const std::string &path) noexcept
{
    bool expected = false;
    if (g_subtitle_thread.compare_exchange_strong(expected, true))
    {
        pthread_attr_t attributes;
        const bool have = pthread_attr_init(&attributes) == 0;
        if (have)
            (void)pthread_attr_setstacksize(&attributes, 512u * 1024u);
        void *thread = nullptr;
        if (scePthreadCreate(&thread, have ? &attributes : nullptr, subtitle_entry, nullptr,
                             "slopfin-subs") != 0)
            g_subtitle_thread.store(false);
        if (have)
            (void)pthread_attr_destroy(&attributes);
    }
    std::lock_guard<std::mutex> guard(g_subtitle_mutex);
    /* Restarting at a new position keeps the same track; do not flicker it off. */
    if (path == g_subtitle_loaded && subtitles::loaded())
        return;
    ++g_subtitle_generation;
    g_subtitle_pending = path;
    g_subtitle_loaded.clear();
    subtitles::clear();
}

void clear_subtitles() noexcept
{
    std::lock_guard<std::mutex> guard(g_subtitle_mutex);
    ++g_subtitle_generation;
    g_subtitle_pending.clear();
    g_subtitle_loaded.clear();
    subtitles::clear();
}

/*
 * With CopyTimestamps the stream counts from the start of the title, but a
 * stream the server muxed also runs 1.4 s ahead of it (stream_clock.hpp).
 * Subtitles, the timeline and reported progress all read this value.
 */
double title_seconds(double stream_seconds) noexcept
{
    return player::film_seconds(stream_seconds, g_start_seconds, g_absolute_timestamps,
                                g_server_muxed);
}

/* ------------------------------------------------- colour conversion */

struct ConvertJob
{
    const std::uint8_t *luma;
    const std::uint8_t *chroma;
    std::uint32_t *out;
    int width;
    int height;
    int pitch; /* bytes per luma row */
    int first_row;
    int last_row;
    int step; /* 2 decimates 4K to the display's width while converting */
    int out_width;
    bool ten_bit; /* PS5 Main10: two bytes per sample, low-aligned value */
    bool hdr;     /* BT.2020 primaries with the PQ transfer curve */
};

/* Precompute tone-mapping tables; per-pixel floating-point conversion is too expensive at 4K. */
std::uint8_t g_pq_to_sdr[1024];
bool g_tables_ready = false;

double pq_eotf(double coded) noexcept
{
    /* SMPTE ST 2084, normalised so 1.0 is 10000 nits. */
    constexpr double m1 = 2610.0 / 16384.0;
    constexpr double m2 = 128.0 * 2523.0 / 4096.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = 32.0 * 2413.0 / 4096.0;
    constexpr double c3 = 32.0 * 2392.0 / 4096.0;
    const double powed = std::pow(coded, 1.0 / m2);
    const double numerator = powed - c1 > 0.0 ? powed - c1 : 0.0;
    return std::pow(numerator / (c2 - c3 * powed), 1.0 / m1);
}

void build_tone_tables() noexcept
{
    if (g_tables_ready)
        return;
    constexpr double kPeakNits = 1000.0;
    constexpr double kPaperWhite = 203.0;
    /* Extended Reinhard works on light normalised so reference white is 1.0.
       Feeding it absolute nits and dividing afterwards, as this did at first,
       maps reference white to 0.005 and renders the whole picture black. */
    constexpr double kPeak = kPeakNits / kPaperWhite;
    /*
     * Normalise so reference white lands at full scale rather than at the 0.52
     * plain Reinhard gives it, then hold back 15 percent as headroom for
     * highlights. Tuned against a real 4K HDR frame until the result matched
     * what the server produces when it tone maps the same title itself.
     */
    const double white = 1.0 * (1.0 + 1.0 / (kPeak * kPeak)) / 2.0;
    constexpr double kHeadroom = 0.85;
    for (int code = 0; code < 1024; ++code)
    {
        const double nits = pq_eotf(static_cast<double>(code) / 1023.0) * 10000.0;
        const double light = nits / kPaperWhite;
        const double mapped = light * (1.0 + light / (kPeak * kPeak)) / (1.0 + light);
        double value = mapped / white * kHeadroom;
        value = value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
        /* BT.709 transfer. */
        const double encoded = value < 0.018 ? value * 4.5 : 1.099 * std::pow(value, 0.45) - 0.099;
        const int out = static_cast<int>(encoded * 255.0 + 0.5);
        g_pq_to_sdr[code] = static_cast<std::uint8_t>(out < 0 ? 0 : (out > 255 ? 255 : out));
    }
    g_tables_ready = true;
}

/* BT.709 limited range, integer maths, which is what broadcast video uses. */
void convert_rows_nv12(const ConvertJob &job) noexcept
{
    for (int y = job.first_row; y < job.last_row; ++y)
    {
        const int source_y = y * job.step;
        const std::uint8_t *luma_row = job.luma + static_cast<std::size_t>(source_y) * job.pitch;
        const std::uint8_t *chroma_row =
            job.chroma + static_cast<std::size_t>(source_y / 2) * job.pitch;
        std::uint32_t *out_row = job.out + static_cast<std::size_t>(y) * job.out_width;

        for (int x = 0; x < job.out_width; ++x)
        {
            const int sx = x * job.step;
            const int luma = (static_cast<int>(luma_row[sx]) - 16) * 1192;
            const int chroma_index = (sx & ~1);
            const int u = static_cast<int>(chroma_row[chroma_index]) - 128;
            const int v = static_cast<int>(chroma_row[chroma_index + 1]) - 128;

            int r = (luma + 1836 * v) >> 10;
            int g = (luma - 218 * u - 546 * v) >> 10;
            int b = (luma + 2163 * u) >> 10;
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);

            out_row[x] = 0xff000000u | (static_cast<std::uint32_t>(r) << 16) |
                         (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
        }
    }
}

/* Main10 decoder samples store significant bits in the low 10 bits of little-endian 16-bit words;
 * pitch is in bytes. */
void convert_rows_p010(const ConvertJob &job) noexcept
{
    const bool native_hdr = job.hdr && slopfin::gfx::hdr_output();
    for (int y = job.first_row; y < job.last_row; ++y)
    {
        const int source_y = y * job.step;
        const auto *luma_row = reinterpret_cast<const std::uint16_t *>(
            job.luma + static_cast<std::size_t>(source_y) * job.pitch);
        const auto *chroma_row = reinterpret_cast<const std::uint16_t *>(
            job.chroma + static_cast<std::size_t>(source_y / 2) * job.pitch);
        std::uint32_t *out_row = job.out + static_cast<std::size_t>(y) * job.out_width;

        if (native_hdr)
        {
            std::uint32_t *__restrict output = out_row;
            for (int x = 0; x < job.out_width; ++x)
            {
                const int sx = x * job.step;
                output[x] =
                    slopfin::hdr::from_ycbcr10(luma_row[sx] & 1023, chroma_row[sx & ~1] & 1023,
                                               chroma_row[(sx & ~1) + 1] & 1023);
            }
            continue;
        }

        for (int x = 0; x < job.out_width; ++x)
        {
            const int sx = x * job.step;
            const int chroma_index = (sx & ~1);
            /*
             * This hardware writes plain 10-bit values into the low bits of
             * each 16-bit sample, not the high bits the P010 name implies.
             * Shifting down by six, as the format normally requires, left luma
             * near zero and both chroma channels near minus half scale, which
             * renders every frame solid green.
             */
            const int y10 = luma_row[sx] & 0x3ff;
            const int u10 = chroma_row[chroma_index] & 0x3ff;
            const int v10 = chroma_row[chroma_index + 1] & 0x3ff;

            if (!job.hdr)
            {
                const int luma = ((y10 >> 2) - 16) * 1192;
                const int u = (u10 >> 2) - 128;
                const int v = (v10 >> 2) - 128;
                int r = (luma + 1836 * v) >> 10;
                int g = (luma - 218 * u - 546 * v) >> 10;
                int b = (luma + 2163 * u) >> 10;
                r = r < 0 ? 0 : (r > 255 ? 255 : r);
                g = g < 0 ? 0 : (g > 255 ? 255 : g);
                b = b < 0 ? 0 : (b > 255 ? 255 : b);
                out_row[x] = 0xff000000u | (static_cast<std::uint32_t>(r) << 16) |
                             (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
                continue;
            }

            /* BT.2020 non-constant luminance, limited range, at ten bits. */
            const int luma = (y10 - 64) * 1192;
            const int u = u10 - 512;
            const int v = v10 - 512;
            int r = (luma + 1721 * v) >> 10;
            int g = (luma - 192 * u - 667 * v) >> 10;
            int b = (luma + 2196 * u) >> 10;
            r = r < 0 ? 0 : (r > 1023 ? 1023 : r);
            g = g < 0 ? 0 : (g > 1023 ? 1023 : g);
            b = b < 0 ? 0 : (b > 1023 ? 1023 : b);

            /* Tone map each channel, then bring BT.2020 primaries to BT.709.
               The matrix runs on the mapped values, which is approximate but
               costs one pass instead of two round trips through linear light. */
            const int tr = g_pq_to_sdr[r];
            const int tg = g_pq_to_sdr[g];
            const int tb = g_pq_to_sdr[b];
            /* BT.2020 to BT.709 primaries, in 1/1024ths. */
            int or_ = (1700 * tr - 602 * tg - 75 * tb) >> 10;
            int og = (-128 * tr + 1160 * tg - 8 * tb) >> 10;
            int ob = (-19 * tr - 103 * tg + 1146 * tb) >> 10;
            or_ = or_ < 0 ? 0 : (or_ > 255 ? 255 : or_);
            og = og < 0 ? 0 : (og > 255 ? 255 : og);
            ob = ob < 0 ? 0 : (ob > 255 ? 255 : ob);

            out_row[x] = 0xff000000u | (static_cast<std::uint32_t>(or_) << 16) |
                         (static_cast<std::uint32_t>(og) << 8) | static_cast<std::uint32_t>(ob);
        }
    }
}

void convert_rows(const ConvertJob &job) noexcept
{
    if (job.ten_bit)
        convert_rows_p010(job);
    else
        convert_rows_nv12(job);
}

/* Microseconds from a monotonic-enough clock, for timing the pipeline. */
std::uint64_t now_us() noexcept
{
    struct
    {
        std::int64_t seconds;
        std::int64_t microseconds;
    } stamp{};
    if (sceKernelGettimeofday(&stamp) != 0)
        return 0;
    return static_cast<std::uint64_t>(stamp.seconds) * 1000000u +
           static_cast<std::uint64_t>(stamp.microseconds);
}

/* Called with g_mutex held after choosing a new picture. */
/* Display intervals the current picture has been held for. */
int g_held_intervals = 0;

void note_presentation(std::uint64_t now) noexcept
{
    /*
     * How long the outgoing picture was held, in display intervals rather
     * than milliseconds: the cadence is a pattern of whole intervals and a
     * millisecond average hides it completely.
     */
    if (g_held_intervals > 0)
    {
        const std::size_t bucket =
            g_held_intervals < 5 ? static_cast<std::size_t>(g_held_intervals) : 5u;
        ++g_status.holds[bucket];
    }
    g_held_intervals = 0;
    if (g_last_shown_us != 0)
        g_status.frame_interval_us = now - g_last_shown_us;
    g_last_shown_us = now;
    ++g_status.shown;
    if (g_rate_started_us == 0)
    {
        g_rate_started_us = now;
        g_rate_started_shown = g_status.shown;
    }
    else if (now - g_rate_started_us >= 1000000)
    {
        g_status.frame_rate = (g_status.shown - g_rate_started_shown) * 1000000.0 /
                              static_cast<double>(now - g_rate_started_us);
        g_rate_started_us = now;
        g_rate_started_shown = g_status.shown;
    }
}

/*
 * Converted on the decode thread. A pool of worker threads spinning on short
 * sleeps made this about three milliseconds faster per frame but starved the
 * render thread for seven or eight vertical blanks at a time, which is far
 * worse than the time it saved. The decode thread already waits tens of
 * milliseconds for a free slot, so it has the headroom.
 */
void convert_frame(const std::uint8_t *base, int width, int height, int pitch, std::uint32_t *out,
                   bool ten_bit, bool hdr, int step) noexcept
{
    if (ten_bit && hdr && !slopfin::gfx::hdr_output())
        build_tone_tables();
    ConvertJob job;
    job.luma = base;
    job.chroma = base + static_cast<std::size_t>(pitch) * height;
    job.out = out;
    job.width = width;
    job.height = height;
    job.pitch = pitch;
    job.step = step < 1 ? 1 : step;
    job.out_width = width / job.step;
    job.first_row = 0;
    job.last_row = height / job.step;
    job.ten_bit = ten_bit;
    job.hdr = hdr;
    convert_rows(job);
    // Convert SDR once at source resolution, not on every 4K display redraw.
    if (gfx::hdr_output() && !(ten_bit && hdr))
    {
        const std::size_t pixels = static_cast<std::size_t>(job.out_width) * job.last_row;
        for (std::size_t i = 0; i < pixels; ++i)
            out[i] = gfx::to_hdr_pixel(out[i]);
    }
}

/* Converts one picture into a free slot, waiting while the pool is full. */
void publish(const std::uint8_t *nv12, int width, int height, int pitch, double seconds,
             bool ten_bit, bool hdr) noexcept
{
    if (nv12 == nullptr || width <= 0 || height <= 0 || pitch < width)
        return;

    /*
     * The framebuffer is 1920 wide, so converting every pixel of a 4K picture
     * does four times the work and then throws three quarters of it away at
     * blit time. Decimating during the conversion took a 4K HDR frame from 58
     * milliseconds to something the decode thread can sustain.
     */
    const int step = width >= 3840 ? 2 : 1;
    const int out_width = width / step;
    const int out_height = height / step;

    const std::uint64_t wait_started = now_us();
    std::size_t index = kFramePool;
    for (;;)
    {
        {
            std::lock_guard<std::mutex> guard(g_mutex);
            for (std::size_t i = 0; i < kFramePool; ++i)
            {
                if (g_pool[i].state == SlotState::free)
                {
                    index = i;
                    break;
                }
            }
        }
        if (index < kFramePool)
            break;
        if (g_stop.load(std::memory_order_acquire))
            return;
        (void)sceKernelUsleep(2000);
    }

    {
        const std::uint64_t waited = now_us() - wait_started;
        std::lock_guard<std::mutex> guard(g_mutex);
        g_status.slotwait_us = waited;
        if (waited > g_status.slotwait_max_us)
            g_status.slotwait_max_us = waited;
    }

    PoolFrame &target = g_pool[index];
    const std::size_t needed = static_cast<std::size_t>(out_width) *
                               static_cast<std::size_t>(out_height) * sizeof(std::uint32_t);
    if (target.capacity < needed)
    {
        bigalloc::release(target.bitmap.pixels);
        target.bitmap.pixels = static_cast<std::uint32_t *>(bigalloc::allocate(needed));
        target.capacity = target.bitmap.pixels != nullptr ? needed : 0;
    }
    if (target.bitmap.pixels == nullptr)
        return;
    target.bitmap.width = out_width;
    target.bitmap.height = out_height;
    target.bitmap.hdr10 = gfx::hdr_output();
    target.seconds = seconds;

    const std::uint64_t started = now_us();
    convert_frame(nv12, width, height, pitch, target.bitmap.pixels, ten_bit, hdr, step);
    const std::uint64_t convert_us = now_us() - started;

    std::lock_guard<std::mutex> guard(g_mutex);
    target.state = SlotState::ready;
    audio::video_ready();
    ++g_status.frames;
    g_status.width = width;
    g_status.height = height;
    g_status.convert_us = convert_us;
}

/* ------------------------------------------------- decoder */

struct Decoder
{
    void *compute_queue = nullptr;
    void *handle = nullptr;
    Videodec2ComputeMemory compute_memory{};
    Videodec2DecoderMemory memory{};
    DirectBlock compute_block;
    DirectBlock gpu_block;
    DirectBlock cpu_gpu_block;
    DirectBlock input_block;
    DirectBlock frame_block;
    std::size_t cpu_mapping = 0;
    std::size_t frame_slot_bytes = 0;
    /* Frame surfaces cycled through: at least the default sixteen, more when
       the stream's picture buffer is deeper, so a slot is never handed back
       while the decoder can still hold it as a reference. */
    std::size_t slots = kPipelineSlots;
    std::size_t max_frame_size = 0;
    std::uint32_t depth = 1; /* what the decoder was actually created with */
    bool module_loaded = false;

    bool open(slopfin::ts::Codec codec, int max_width, int max_height, int bit_depth,
              int dpb_frames) noexcept;
    void close() noexcept;
};

bool Decoder::open(slopfin::ts::Codec codec, int max_width, int max_height, int bit_depth,
                   int dpb_frames) noexcept
{
    const bool hevc = codec == slopfin::ts::Codec::hevc;
    /* Main 10 is a different profile, and asking for Main against a 10-bit
       stream opens a decoder that rejects every access unit it is given. */
    const bool ten_bit = hevc && bit_depth >= 10;
    if (sceSysmoduleLoadModule(kVideoModuleId) != 0)
    {
        trace::mark("player: video module would not load");
        return false;
    }
    module_loaded = true;

    trace::mark("player: video module loaded");
    const auto direct_limit = static_cast<std::int64_t>(sceKernelGetDirectMemorySize());
    if (direct_limit <= 0)
        return false;

    compute_memory.size = sizeof(compute_memory);
    if (sceVideodec2QueryComputeMemoryInfo(&compute_memory) != 0)
        return false;
    if (!compute_block.allocate(static_cast<std::size_t>(compute_memory.cpu_gpu_size),
                                kProtectCpuGpu, direct_limit))
    {
        trace::mark("player: compute memory allocation failed");
        return false;
    }
    compute_memory.cpu_gpu = compute_block.address;
    compute_memory.cpu_gpu_size = compute_block.size;

    Videodec2ComputeConfig compute_config{};
    compute_config.size = sizeof(compute_config);
    if (sceVideodec2AllocateComputeQueue(&compute_config, &compute_memory, &compute_queue) != 0)
    {
        trace::mark("player: compute queue refused");
        return false;
    }

    Videodec2DecoderConfig config{};
    config.size = sizeof(config);
    config.resource_type = 1;
    config.codec_type = hevc ? kCodecHevc : kCodecH264;
    config.profile = hevc ? (ten_bit ? kProfileHevcMain10 : kProfileHevcMain) : kProfileH264High;
    /* Level 5.1 covers 4K HEVC and 1080p H.264; 4K H.264 is 5.2, the level
       the research configured and proved (codecs-and-resolutions.md). */
    config.max_level = hevc ? 153 : max_width > 1920 ? 52 : 51;
    config.max_width = max_width;
    config.max_height = max_height;
    /* UHD HEVC streams can require six pictures in their decoded-picture
       buffer (Pacific Rim's SPS declares six). Four rejects that stream.
       The sixteen output slots still outnumber the decoder's reference set. */
    config.max_dpb_frames = dpb_frames;
    slots = std::max<std::size_t>(kPipelineSlots,
                                  static_cast<std::size_t>(dpb_frames) + kDecoderDepth + 7);
    depth = kDecoderDepth;
    if (std::FILE *marker = std::fopen("/data/slopfin-depth1", "rb"); marker != nullptr)
    {
        (void)std::fclose(marker);
        depth = 1;
    }
    config.pipeline_depth = depth;
    config.compute_queue = reinterpret_cast<std::uint64_t>(compute_queue);
    config.cpu_affinity = 0x3f;
    config.cpu_priority = 700;
    /* Everything Jellyfin transcodes is progressive; the interlaced path is
       what broadcast H.264 needs and is not wanted here. */
    config.optimize_progressive = hevc ? 1 : 0;

    trace::mark("player: querying decoder memory at depth " + std::to_string(depth));
    memory.size = sizeof(memory);
    std::int32_t queried = sceVideodec2QueryDecoderMemoryInfo(&config, &memory);
    if (queried != 0 && depth != 1)
    {
        /* The query validates every field, so a refusal here means this
           depth is not offered; the synchronous mode always has been. */
        trace::mark("player: depth " + std::to_string(depth) + " refused 0x" +
                    [queried]
                    {
                        char text[16];
                        std::snprintf(text, sizeof(text), "%x", static_cast<unsigned>(queried));
                        return std::string{text};
                    }() +
                    ", using depth 1");
        depth = 1;
        config.pipeline_depth = 1;
        memory = Videodec2DecoderMemory{};
        memory.size = sizeof(memory);
        queried = sceVideodec2QueryDecoderMemoryInfo(&config, &memory);
    }
    if (queried != 0)
    {
        trace::mark("player: decoder memory query failed");
        return false;
    }

    cpu_mapping = align_16k(static_cast<std::size_t>(memory.cpu_size));
    std::size_t available = 0;
    if (sceKernelAvailableFlexibleMemorySize(&available) == 0 && available < cpu_mapping)
        return false;
    if (sceKernelMapNamedFlexibleMemory(&memory.cpu, cpu_mapping, 0x03, 0, "SlopFinVdec") != 0)
        return false;

    trace::mark("player: decoder memory cpu=" + std::to_string(memory.cpu_size / 1024) +
                "K gpu=" + std::to_string(memory.gpu_size / 1024) +
                "K cpugpu=" + std::to_string(memory.cpu_gpu_size / 1024) +
                "K frame=" + std::to_string(memory.max_frame_size) +
                " align=" + std::to_string(memory.frame_alignment));
    /*
     * The decoder states the alignment its frame surfaces require. Slots that
     * do not honour it are written as though they were aligned, which scatters
     * macroblocks and leaves the picture a patchwork.
     */
    std::size_t frame_alignment = memory.frame_alignment;
    if (frame_alignment < kDirectAlignment)
        frame_alignment = kDirectAlignment;
    max_frame_size = static_cast<std::size_t>(memory.max_frame_size);
    frame_slot_bytes = (max_frame_size + frame_alignment - 1u) & ~(frame_alignment - 1u);
    if (frame_slot_bytes == 0 || memory.gpu_size == 0)
    {
        trace::mark("player: decoder reported unusable sizes");
        return false;
    }
    if (!gpu_block.allocate(static_cast<std::size_t>(memory.gpu_size), kProtectGpu, direct_limit))
        return false;
    if (memory.cpu_gpu_size != 0 &&
        !cpu_gpu_block.allocate(static_cast<std::size_t>(memory.cpu_gpu_size), kProtectCpuGpu,
                                direct_limit))
        return false;
    /* Input access units and output frame slots require CPU mappings; decoder-internal surfaces
     * remain device-owned. */
    if (!input_block.allocate(kInputSlotBytes * slots, kProtectGpu, direct_limit))
        return false;
    if (!frame_block.allocate(frame_slot_bytes * slots, kProtectGpu, direct_limit))
        return false;

    memory.gpu = gpu_block.address;
    memory.gpu_size = gpu_block.size;
    if (cpu_gpu_block.size != 0)
    {
        memory.cpu_gpu = cpu_gpu_block.address;
        memory.cpu_gpu_size = cpu_gpu_block.size;
    }

    trace::mark("player: creating decoder");
    if (sceVideodec2CreateDecoder(&config, &memory, &handle) != 0)
    {
        trace::mark("player: decoder creation failed");
        return false;
    }
    (void)sceVideodec2Reset(handle);
    trace::mark("player: decoder ready");
    return true;
}

void Decoder::close() noexcept
{
    if (handle != nullptr)
        (void)sceVideodec2DeleteDecoder(handle);
    if (compute_queue != nullptr)
        (void)sceVideodec2ReleaseComputeQueue(compute_queue);
    handle = nullptr;
    compute_queue = nullptr;

    if (memory.cpu != nullptr && cpu_mapping != 0)
        (void)sceKernelMunmap(memory.cpu, cpu_mapping);
    memory.cpu = nullptr;

    frame_block.release();
    input_block.release();
    cpu_gpu_block.release();
    gpu_block.release();
    compute_block.release();

    if (module_loaded)
        (void)sceSysmoduleUnloadModule(kVideoModuleId);
    module_loaded = false;
}

/* ------------------------------------------------- capability probe */

/* Probe complete decoder configurations without allocating output surfaces. */
void probe_decoder_support_impl() noexcept
{
    std::FILE *report = std::fopen("/data/slopfin-video-caps.txt", "wb");
    if (report == nullptr)
        return;
    std::fprintf(report, "SlopFin video decoder capability probe\n\n");

    if (sceSysmoduleLoadModule(kVideoModuleId) != 0)
    {
        std::fprintf(report, "video module would not load\n");
        (void)std::fclose(report);
        return;
    }
    const auto direct_limit = static_cast<std::int64_t>(sceKernelGetDirectMemorySize());

    Videodec2ComputeMemory compute_memory{};
    compute_memory.size = sizeof(compute_memory);
    DirectBlock compute_block;
    void *compute_queue = nullptr;
    if (sceVideodec2QueryComputeMemoryInfo(&compute_memory) == 0 &&
        compute_block.allocate(static_cast<std::size_t>(compute_memory.cpu_gpu_size),
                               kProtectCpuGpu, direct_limit))
    {
        compute_memory.cpu_gpu = compute_block.address;
        compute_memory.cpu_gpu_size = compute_block.size;
        Videodec2ComputeConfig compute_config{};
        compute_config.size = sizeof(compute_config);
        if (sceVideodec2AllocateComputeQueue(&compute_config, &compute_memory, &compute_queue) != 0)
            compute_queue = nullptr;
    }
    std::fprintf(report, "compute queue: %s\n\n", compute_queue != nullptr ? "ready" : "FAILED");

    struct Candidate
    {
        const char *label;
        std::uint32_t codec;
        std::uint32_t profile;
        std::uint32_t level;
        int width;
        int height;
    };
    /* HEVC profile 1 is Main (8-bit); profile 2 is Main 10, which every HDR
       source uses. Whether that configures decides if 4K HDR can be played
       directly instead of transcoded. */
    static const Candidate kCandidates[] = {
        {"H.264 High @5.1 1080p", kCodecH264, kProfileH264High, 51, 1920, 1088},
        {"H.264 High @5.2 4K", kCodecH264, kProfileH264High, 52, 3840, 2176},
        {"HEVC Main @5.1 1080p", kCodecHevc, 1, 153, 1920, 1088},
        {"HEVC Main @5.1 4K", kCodecHevc, 1, 153, 3840, 2176},
        {"HEVC Main10 @5.1 1080p", kCodecHevc, 2, 153, 1920, 1088},
        {"HEVC Main10 @5.1 4K", kCodecHevc, 2, 153, 3840, 2176},
        {"HEVC Main10 @6.0 4K", kCodecHevc, 2, 180, 3840, 2176},
        {"HEVC profile 3 4K", kCodecHevc, 3, 153, 3840, 2176},
    };

    static constexpr int kDpbSizes[] = {4, 5, 6, 7, 8, 10, 16};
    for (const Candidate &candidate : kCandidates)
        for (const int dpb : kDpbSizes)
        {
            Videodec2DecoderConfig config{};
            config.size = sizeof(config);
            config.resource_type = 1;
            config.codec_type = candidate.codec;
            config.profile = candidate.profile;
            config.max_level = candidate.level;
            config.max_width = candidate.width;
            config.max_height = candidate.height;
            /* Match the configuration the player actually opens with; the query
               validates every field, so a probe that differs anywhere reports the
               working configuration as refused. */
            config.max_dpb_frames = dpb;
            config.pipeline_depth = kDecoderDepth;
            config.compute_queue = reinterpret_cast<std::uint64_t>(compute_queue);
            config.cpu_affinity = 0x3f;
            config.cpu_priority = 700;
            config.optimize_progressive = candidate.codec == kCodecHevc ? 1 : 0;

            Videodec2DecoderMemory memory{};
            memory.size = sizeof(memory);
            const std::int32_t result = sceVideodec2QueryDecoderMemoryInfo(&config, &memory);
            if (result == 0)
                std::fprintf(
                    report,
                    "  %-24s dpb %2d SUPPORTED  cpu %lluK gpu %lluK cpugpu %lluK frame %llu\n",
                    candidate.label, dpb, static_cast<unsigned long long>(memory.cpu_size / 1024),
                    static_cast<unsigned long long>(memory.gpu_size / 1024),
                    static_cast<unsigned long long>(memory.cpu_gpu_size / 1024),
                    static_cast<unsigned long long>(memory.max_frame_size));
            else
                std::fprintf(report, "  %-24s dpb %2d refused 0x%x\n", candidate.label, dpb,
                             result);
        }

    if (compute_queue != nullptr)
        (void)sceVideodec2ReleaseComputeQueue(compute_queue);
    compute_block.release();
    (void)sceSysmoduleUnloadModule(kVideoModuleId);
    std::fprintf(report, "\nprobe complete\n");
    (void)std::fclose(report);
}

/* ------------------------------------------------- playback thread */

/*
 * The server decides what to send, from the device profile SlopFin publishes.
 * When the source video already fits the decoder's limits the server copies the
 * stream into a transport stream instead of re-encoding it, so the console's
 * hardware does the decoding rather than the server's GPU.
 */

// Reading/demuxing must continue while video waits for a free picture slot:
// the next audio PES may be behind those video packets in the transport.
class TransportProducer
{
    http::Stream &stream_;
    player::OrderCapture &capture_;
    void *thread_ = nullptr;

  public:
    /* 128 MiB rather than 64: about fifteen seconds of this library's heaviest
       stream instead of seven. The server's disk stalls for seconds at a time
       under contention (docs/SOFTWARE_AUDIO.md); the only defence on this side
       is having more of the stream already in hand when it happens. */
    player::PacketQueue video{128u * 1024u * 1024u};
    std::size_t total_bytes = 0;
    TransportProducer(http::Stream &stream, player::OrderCapture &capture)
        : stream_(stream), capture_(capture)
    {
    }
    bool start() noexcept
    {
        if (!video.valid())
            return false;
        pthread_attr_t attr;
        if (pthread_attr_init(&attr) != 0)
            return false;
        (void)pthread_attr_setstacksize(&attr, 512u * 1024u);
        const int result = scePthreadCreate(
            &thread_, &attr,
            [](void *p) -> void *
            {
                static_cast<TransportProducer *>(p)->run();
                return nullptr;
            },
            this, "slopfin-transport");
        (void)pthread_attr_destroy(&attr);
        if (result != 0)
            thread_ = nullptr;
        return result == 0;
    }
    void stop() noexcept
    {
        if (!thread_)
            return;
        g_stop.store(true, std::memory_order_release);
        video.close();
        audio::interrupt();
        stream_.interrupt();
        int joined = scePthreadJoin(thread_, nullptr);
        if (joined != 0)
            trace::mark("player: transport join failed; retaining its buffers");
        while (joined != 0)
        {
            (void)sceKernelUsleep(1000000);
            joined = scePthreadJoin(thread_, nullptr);
        }
        thread_ = nullptr;
    }
    ~TransportProducer()
    {
        stop();
    }

  private:
    void stage(int value) noexcept
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status.input_stage = value;
        g_status.input_since_us = now_us();
    }
    void run() noexcept
    {
        const auto spare = images::spare_cores();
        if (spare)
            (void)scePthreadSetaffinity(scePthreadSelf(), spare);
        ts::Demuxer demuxer;
        player::MediaBitrate bitrate;
        std::vector<std::uint8_t> chunk(64u * 1024u);
        std::size_t window_bytes = 0;
        std::uint64_t window_start = now_us();
        while (!g_stop.load(std::memory_order_acquire))
        {
            const auto began = now_us();
            stage(1);
            const long received = stream_.read(chunk.data(), chunk.size());
            const auto now = now_us();
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_status.network_us = now - began;
                g_status.network_max_us = std::max(g_status.network_max_us, now - began);
            }
            stage(2);
            if (received > 0)
            {
                total_bytes += static_cast<std::size_t>(received);
                window_bytes += static_cast<std::size_t>(received);
                capture_.input(chunk.data(), static_cast<std::size_t>(received));
                demuxer.feed(chunk.data(), static_cast<std::size_t>(received));
            }
            else
                demuxer.finish();
            if (g_paused.load())
            {
                window_bytes = 0;
                window_start = now;
            }
            else if (now - window_start >= 3000000)
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_status.download_bitrate =
                    double(window_bytes) * 8000000.0 / double(now - window_start);
                window_bytes = 0;
                window_start = now;
            }
            ts::AccessUnit unit;
            while (!g_stop.load() && demuxer.next(unit))
            {
                const double measured =
                    bitrate.add(unit.data.size(), unit.video, unit.dts >= 0 ? unit.dts : unit.pts);
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_status.measured_bitrate = measured;
                }
                if (unit.video)
                {
                    stage(4);
                    if (!video.push(unit, demuxer.video_codec(), g_stop))
                    {
                        if (!g_stop.load())
                            set_state(player::State::failed,
                                      "Video packet exceeds the playback buffer.");
                        video.close();
                        return;
                    }
                }
                else
                {
                    stage(3);
                    audio::submit(unit.data.data(), unit.data.size(), unit.pts);
                    if (audio::failed())
                    {
                        set_state(player::State::failed,
                                  "CPU audio decode failed. Disable the audio trial and retry with "
                                  "server conversion.");
                        video.close();
                        return;
                    }
                }
                stage(2);
            }
            if (received <= 0)
            {
                if (!g_stop.load())
                    audio::finish();
                if (audio::failed())
                    set_state(player::State::failed,
                              "CPU audio decoder could not finish the stream.");
                break;
            }
        }
        video.close();
        stage(0);
    }
};

void *playback_entry(void *) noexcept
{
    const config::Settings &settings = config::current();
    Decoder decoder;
    http::Stream stream;
    player::OrderCapture order_capture;

    /*
     * The message says which of these is happening, because "Opening stream"
     * for the whole sequence tells a viewer nothing about why it is taking as
     * long as it is.
     */
    set_state(player::State::opening, "Asking the server");

    g_request = player::with_console_options(g_request);
    jellyfin::PlaybackPlan plan = jellyfin::playback_plan(g_request);
    if (!plan.valid)
    {
        set_state(player::State::failed,
                  "Playback negotiation failed. Try again or change quality.");
        g_running.store(false);
        return nullptr;
    }
    g_absolute_timestamps = plan.absolute_timestamps;
    g_server_muxed = !plan.direct;
    if (g_server_muxed)
        trace::mark("player: server-muxed stream, film clock corrected by -1.400 s");
    trace::mark(std::string{"player: timestamps "} +
                (plan.absolute_timestamps ? "on title time" : "relative to start") + ", subtitle " +
                std::to_string(g_request.subtitle_index) +
                (plan.subtitle_burned        ? " burned in"
                 : plan.subtitle_url.empty() ? ""
                                             : " as text"));
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_status.subtitle_burned = plan.subtitle_burned;
        g_status.media_source_id = plan.media_source_id;
        g_status.source_fps = plan.frame_rate;
    }
    if (!plan.subtitle_url.empty())
        request_subtitles(plan.subtitle_url);
    else
        clear_subtitles();
    trace::mark(std::string{"player: server will send "} +
                (plan.video_codec.empty() ? "?" : plan.video_codec.c_str()) + " video, " +
                (plan.audio_codec.empty() ? "?" : plan.audio_codec.c_str()) + " audio x" +
                std::to_string(plan.audio_channels) + (plan.direct ? " (direct)" : " (server)"));
    trace::mark("player: connecting media stream");
    set_state(player::State::opening, "Connecting");
    if (!stream.open(settings.host, settings.port, plan.path, {}))
    {
        set_state(player::State::failed, "The server would not start the stream.");
        decoder.close();
        g_running.store(false);
        return nullptr;
    }
    trace::mark("player: stream open");
    reporter::begin({g_request.item_id, plan.media_source_id, plan.play_session,
                     plan.direct ? "DirectPlay" : "Transcode", g_request.audio_index,
                     g_request.subtitle_index},
                    g_request.start_seconds);
    std::string delivery;
    for (int attempt = 0; attempt < 30 && !g_stop.load(); ++attempt)
    {
        delivery = jellyfin::playback_delivery(plan);
        if (plan.video_delivery_known)
            break;
        (void)sceKernelUsleep(100000);
    }
    if (!plan.video_delivery_known && plan.hdr)
    {
        set_state(player::State::failed,
                  "Could not confirm HDR stream format. Try playback again.");
        g_running.store(false);
        return nullptr;
    }
    g_stream_is_hdr = plan.hdr;
    g_stream_bit_depth = plan.bit_depth;
    g_stream_width = plan.width;
    g_stream_height = plan.height;
    gfx::request_hdr_output(plan.hdr);
    while (gfx::hdr_request_pending() && !g_stop.load(std::memory_order_acquire))
        (void)sceKernelUsleep(1000);
    trace::mark(gfx::hdr_output() ? "player: HDR10 mode acknowledged"
                                  : "player: SDR mode acknowledged");
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        /*
         * Honest about what is known .
         * The console's HDR setting is read from its output status, so the
         * line says which way it points and why SDR was chosen. Whether the
         * TV itself switched is still not something the app can read.
         */
        const gfx::SystemHdr setting = gfx::system_hdr();
        const char *const why_sdr = gfx::sdr_only()                  ? "SDR forced on this console"
                                    : setting == gfx::SystemHdr::off ? "PS5 HDR setting is Off"
                                    : setting == gfx::SystemHdr::on
                                        ? "HDR10 buffers refused"
                                        : "PS5 HDR setting not readable";
        if (gfx::hdr_output())
            g_status.colour_info = plan.hdr
                                       ? "HDR source -> HDR10 output | PS5 HDR: On When Supported"
                                       : "SDR source -> HDR10 output | PS5 HDR: On When Supported";
        else if (plan.hdr)
            g_status.colour_info =
                std::string{"HDR source -> SDR output | tone mapped by SlopFin | "} + why_sdr;
        else
            g_status.colour_info = "SDR source -> SDR output";
        g_status.delivery_info = "Checking server video/audio delivery...";
    }
    trace::mark("player: " + delivery);
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_status.delivery_info = delivery;
    }
    const audio::Codec audio_codec = delivered_audio_codec(plan);
    if (!audio::start(audio_codec, audio_codec == audio::Codec::truehd))
        trace::mark("player: continuing without audio");
    if (audio::bitstream())
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_status.delivery_info += audio_codec == audio::Codec::dts
                                      ? " | Bitstream to TV (DTS core)"
                                      : " | Bitstream to TV (TV decodes Dolby)";
    }
    else if (audio_codec == audio::Codec::truehd || audio_codec == audio::Codec::eac3 ||
             audio_codec == audio::Codec::dts)
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_status.delivery_info += " | Console CPU decode -> PCM (trial)";
    }
    bool decoder_ready = false;
    set_state(player::State::opening, "Buffering");

    TransportProducer transport(stream, order_capture);
    if (!transport.start())
    {
        set_state(player::State::failed, "Could not start the transport buffer.");
        audio::stop(g_preserve_outputs_on_exit.load(std::memory_order_acquire));
        g_running.store(false);
        return nullptr;
    }
    std::size_t submitted = 0;
    std::size_t rejected = 0;
    std::size_t decode_errors = 0;
    /*
     * The decoder's write into a frame slot is a hardware write, and the
     * proven path never reads those slots with the CPU: it hands them to the
     * GPU, whose work is naturally ordered after the write. A CPU read has no
     * such ordering, so pictures are held briefly and converted several
     * decodes later, by which time the engine has certainly finished.
     */
    struct Ready
    {
        const std::uint8_t *pixels;
        int width;
        int height;
        int pitch;
        double seconds;
        bool ten_bit;
    };
    std::deque<Ready> ready;
    /* Track decoder-owned slots in submission order; never reuse or read an in-flight slot. */
    struct InFlight
    {
        const std::uint8_t *slot;
        std::size_t submission;
    };
    std::deque<InFlight> in_flight;
    player::PresentationQueue timestamps;
    constexpr std::size_t kReadDelay = 4;
    std::size_t accepted = 0;
    std::size_t concealed = 0;
    double last_published_seconds = 0.0;
    double frame_duration = 1.0 / 24.0;
    bool saw_picture = false;

    ts::AccessUnit unit;
    ts::Codec stream_codec = ts::Codec::unknown;
    while (transport.video.pop(unit, stream_codec, g_stop))
    {
        if (plan.dv_hdr10_base && plan.video_delivery_known && plan.hdr)
        {
            auto size = unit.data.size();
            if (stream_codec != ts::Codec::hevc ||
                !video::strip_dolby_vision(unit.data.data(), size))
            {
                set_state(player::State::failed,
                          "Invalid Dolby Vision base-layer stream. Use SDR fallback.");
                g_stop.store(true);
                break;
            }
            unit.data.resize(size);
        }
        if (unit.data.empty() || unit.data.size() > kInputSlotBytes)
            continue;

        /* The decoder is configured from the codec the stream declares,
           so an 8-bit HEVC source plays without any conversion at all. */
        if (!decoder_ready)
        {
            const bool hevc = stream_codec == ts::Codec::hevc;
            trace::mark(hevc ? "player: stream is HEVC" : "player: stream is H.264");
            /* Derive picture dimensions and reference depth from this stream's SPS. */
            const video::StreamShape shape =
                video::stream_shape(unit.data.data(), unit.data.size(), hevc);
            const int default_dpb = hevc ? 6 : 5;
            const int bit_depth = shape.valid ? shape.bit_depth : g_stream_bit_depth;
            const bool large =
                hevc || (shape.valid ? shape.width > 1920 || shape.height > 1088
                                     : g_stream_width == 0 || g_stream_width > 1920 ||
                                           g_stream_height > 1088);
            const int dpb = shape.valid ? std::max(shape.dpb, default_dpb) : default_dpb;
            trace::mark("player: SPS " + std::string{shape.valid ? "" : "unreadable "} +
                        std::to_string(shape.width) + "x" + std::to_string(shape.height) + " dpb " +
                        std::to_string(shape.dpb) + " depth " + std::to_string(shape.bit_depth) +
                        " level " + std::to_string(shape.level) + "; opening " +
                        (large ? "4K" : "1080p") + " dpb " + std::to_string(dpb) + " at " +
                        std::to_string(bit_depth) + " bits");
            set_state(player::State::opening, "Starting the decoder");
            bool opened = decoder.open(stream_codec, large ? 3840 : 1920, large ? 2176 : 1088,
                                       bit_depth, dpb);
            if (!opened && dpb != default_dpb)
            {
                /* A deeper buffer may not fit in memory; the default always has. */
                trace::mark("player: decoder refused dpb " + std::to_string(dpb) +
                            ", retrying at " + std::to_string(default_dpb));
                decoder.close();
                opened = decoder.open(stream_codec, large ? 3840 : 1920, large ? 2176 : 1088,
                                      bit_depth, default_dpb);
            }
            if (!opened)
            {
                set_state(player::State::failed, "The hardware decoder could not be opened.");
                g_stop.store(true, std::memory_order_release);
                break;
            }
            decoder_ready = true;
        }

        const std::size_t slot = submitted % decoder.slots;
        auto *input_slot =
            static_cast<std::uint8_t *>(decoder.input_block.address) + slot * kInputSlotBytes;
        auto *frame_slot = static_cast<std::uint8_t *>(decoder.frame_block.address) +
                           slot * decoder.frame_slot_bytes;
        std::memcpy(input_slot, unit.data.data(), unit.data.size());
        flush_range(input_slot, unit.data.size());

        Videodec2Input input{};
        input.size = sizeof(input);
        input.au = input_slot;
        input.au_size = unit.data.size();
        input.pts = unit.pts >= 0 ? static_cast<std::uint64_t>(unit.pts * 1000 / 90) : UINT64_MAX;
        input.dts = unit.dts >= 0 ? static_cast<std::uint64_t>(unit.dts * 1000 / 90) : UINT64_MAX;

        Videodec2Frame frame{};
        frame.size = sizeof(frame);
        frame.buffer = frame_slot;
        /*
         * Declare exactly the size the decoder asked for, not the slot
         * spacing. The documented contract is that the returned byte count
         * equals luma plus chroma for the coded geometry; handing over a
         * rounded-up size makes the surface it lays out disagree with the
         * geometry it reports.
         */
        frame.buffer_size = decoder.max_frame_size;

        Videodec2Output output{};
        output.size = sizeof(output);

        const std::uint64_t decode_started = now_us();
        const std::int32_t result = sceVideodec2Decode(decoder.handle, &input, &frame, &output);
        const std::uint64_t decode_us = now_us() - decode_started;
        ++submitted;
        {
            std::lock_guard<std::mutex> guard(g_mutex);
            g_status.decode_us = decode_us;
        }
        /*
         * Only a non-zero call result is fatal. output.error is a
         * per-picture concealment flag: the decoder still hands back a
         * complete picture and sets it when that picture referenced data
         * it never saw, which is normal for the first frames after a seek.
         * Treating it as failure discarded five frames in six.
         */
        if (result != 0)
        {
            ++decode_errors;
            {
                std::lock_guard<std::mutex> guard(g_mutex);
                g_status.decode_failures = static_cast<long>(decode_errors);
            }
            if (decode_errors <= 6)
                trace::mark("player: decode call failed result=" + std::to_string(result) +
                            " au=" + std::to_string(unit.data.size()));
            if (decode_errors < 60)
                continue;
            set_state(player::State::failed, "The stream could not be decoded.");
            g_stop.store(true, std::memory_order_release);
            break;
        }
        if (output.error != 0)
            ++concealed;

        if (frame.accepted != 0)
        {
            in_flight.push_back(InFlight{frame_slot, submitted - 1});
            /* The slots cycle, so anything older than that is a slot the
               decoder dropped and has since been handed again. */
            while (in_flight.size() > decoder.slots)
                in_flight.pop_front();
        }

        if (!timestamps.push(unit.pts))
        {
            set_state(player::State::failed, "The decoder stopped returning pictures.");
            g_stop.store(true, std::memory_order_release);
            break;
        }

        if (output.valid == 0)
            continue; /* the decoder is still filling its picture buffer */

        std::int64_t picture_pts = -1;
        (void)timestamps.take(picture_pts);

        /* Which slot this picture came back in. Older entries ahead of it
           will never be returned now, so they go with it. */
        bool held = false;
        std::size_t lag = 0;
        for (std::size_t i = 0; i < in_flight.size(); ++i)
        {
            if (in_flight[i].slot != output.buffer)
                continue;
            held = true;
            lag = (submitted - 1) - in_flight[i].submission;
            in_flight.erase(in_flight.begin(),
                            in_flight.begin() + static_cast<std::ptrdiff_t>(i + 1));
            break;
        }

        if (accepted < 12)
            trace::mark("player: pts input=" + std::to_string(unit.pts) + " picture=" +
                        std::to_string(picture_pts) + " depth=" + std::to_string(decoder.depth) +
                        " lag=" + (held ? std::to_string(lag) : std::string{"none"}));

        if (!saw_picture)
        {
            trace::mark("player: first picture " + std::to_string(output.width) + "x" +
                        std::to_string(output.height) + " pitch=" + std::to_string(output.pitch) +
                        " pitch_bytes=" + std::to_string(output.pitch_bytes) +
                        " format=" + std::to_string(output.frame_format) +
                        " buffer=" + std::to_string(output.buffer_size) +
                        " pictures=" + std::to_string(output.picture_count) +
                        " slotbuf=" + std::to_string(decoder.frame_slot_bytes) +
                        " same=" + std::to_string(output.buffer == frame_slot ? 1 : 0));
        }
        /*
         * Everything about the picture is checked before a single byte is
         * read. The decoder hands back a pointer into its own memory, and
         * reading past it is what takes the console down rather than
         * merely failing playback.
         */
        /*
         * P010 stores two bytes per sample, so a row is twice as wide in
         * bytes as it is in samples. Both the size check and the row stride
         * must use the byte pitch, not the sample pitch.
         */
        const std::uint32_t row_bytes = output.pitch_bytes != 0 ? output.pitch_bytes : output.pitch;
        const bool sample_pitch_sane = row_bytes == output.pitch || row_bytes == output.pitch * 2u;
        const std::uint64_t needed_bytes =
            static_cast<std::uint64_t>(row_bytes) * output.height * 3u / 2u;
        /*
         * frame.accepted says whether the decoder actually wrote into the
         * buffer handed to it. Without that check a picture it declined is
         * read anyway, which shows up as a frame that is partly current
         * and partly stale: the blocky smearing over moving scenes.
         */
        const bool geometry_sane =
            held && output.picture_count == 1 && output.width >= 16 && output.height >= 16 &&
            output.width <= 4096 && output.height <= 2304 && output.pitch >= output.width &&
            (output.pitch & 1u) == 0 && sample_pitch_sane && needed_bytes <= output.buffer_size &&
            output.buffer_size <= decoder.frame_slot_bytes;
        if (!saw_picture)
            trace::mark("player: buffer_size=" + std::to_string(output.buffer_size) +
                        " y+uv=" + std::to_string(needed_bytes) +
                        " max_frame=" + std::to_string(decoder.max_frame_size));
        if (!geometry_sane)
        {
            ++rejected;
            if (rejected <= 3)
                trace::mark("player: refused accepted=" + std::to_string(frame.accepted) +
                            " pictures=" + std::to_string(output.picture_count) +
                            " same=" + std::to_string(output.buffer == frame_slot ? 1 : 0) +
                            " held=" + std::to_string(held ? 1 : 0) +
                            " in_flight=" + std::to_string(in_flight.size()));
            /* A declined picture is normal while the decoder reorders, so
               keep going and only give up if none is ever accepted. */
            if (rejected < 120)
                continue;
            trace::mark("player: refusing picture w=" + std::to_string(output.width) + " h=" +
                        std::to_string(output.height) + " pitch=" + std::to_string(output.pitch) +
                        " pitch_bytes=" + std::to_string(output.pitch_bytes) +
                        " buffer=" + std::to_string(output.buffer_size) +
                        " needed=" + std::to_string(needed_bytes) +
                        " format=" + std::to_string(output.frame_format));
            set_state(player::State::failed, "The decoder returned an unusable picture.");
            g_stop.store(true, std::memory_order_release);
            break;
        }

        /*
         * Development aid: capture one picture with real content so the
         * decoder's output can be judged separately from the conversion.
         */
        ++accepted;
        {
            std::lock_guard<std::mutex> guard(g_mutex);
            g_status.decoded = static_cast<long>(accepted);
        }
        if (!saw_picture)
        {
            saw_picture = true;
            set_state(player::State::playing, {});
        }
        /*
         * Main 10 output is P010, which stores each sample in two bytes, so
         * a row occupies twice the width. That is the reliable signal here:
         * frame_format was 0 for every 8-bit picture observed, and its
         * 10-bit value is not documented.
         */
        const bool ten_bit = row_bytes >= output.width * 2u;
        ready.push_back(
            Ready{static_cast<const std::uint8_t *>(output.buffer), static_cast<int>(output.width),
                  static_cast<int>(output.height), static_cast<int>(row_bytes),
                  picture_pts >= 0 ? static_cast<double>(picture_pts) / 90000.0 : -1.0, ten_bit});

        if (ready.size() > kReadDelay)
        {
            const Ready shown = ready.front();
            ready.pop_front();
            const auto bytes = static_cast<std::size_t>(shown.pitch) * shown.height * 3u / 2u;
            invalidate_range(shown.pixels, bytes);

            /*
             * This timestamp belongs to the returned picture, not to the
             * access unit submitted on that Decode call. Relabelling an
             * already reordered picture with decode-order PTS makes the
             * display reorder it a second time and motion jump backwards.
             */
            double seconds = shown.seconds;
            if (seconds < 0.0)
                seconds = last_published_seconds + frame_duration;
            else if (last_published_seconds > 0.0 && seconds > last_published_seconds)
                frame_duration = std::min(0.2, seconds - last_published_seconds);
            last_published_seconds = std::max(last_published_seconds, seconds);

            /*
             * Development aid: dump the decoder's own surface so the plane
             * layout can be measured off-console rather than assumed.
             */
            if (std::FILE *want = std::fopen("/data/slopfin-nv12", "rb"); want != nullptr)
            {
                (void)std::fclose(want);
                (void)unlink("/data/slopfin-nv12");
                if (std::FILE *dump = std::fopen("/data/slopfin-nv12.bin", "wb"); dump != nullptr)
                {
                    (void)std::fwrite(shown.pixels, 1, decoder.max_frame_size, dump);
                    (void)std::fclose(dump);
                }
                if (std::FILE *side = std::fopen("/data/slopfin-nv12.txt", "wb"); side != nullptr)
                {
                    std::fprintf(side, "width %d\nheight %d\npitch %d\nbytes %zu\n", shown.width,
                                 shown.height, shown.pitch,
                                 static_cast<std::size_t>(decoder.max_frame_size));
                    (void)std::fclose(side);
                }
            }

            order_capture.picture(shown.pixels, shown.width, shown.height, shown.pitch,
                                  shown.ten_bit, seconds);
            publish(shown.pixels, shown.width, shown.height, shown.pitch, seconds, shown.ten_bit,
                    g_stream_is_hdr);
        }
    }

    transport.stop();
    stream.close();
    decoder.close();
    audio::stop(g_preserve_outputs_on_exit.load(std::memory_order_acquire));
    double ended_at = 0.0;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (g_status.state == player::State::playing || g_status.state == player::State::opening)
            g_status.state = player::State::ended;
        ended_at = g_status.position_seconds;
    }
    reporter::end(ended_at);
    g_running.store(false);
    trace::mark("player: finished after " + std::to_string(transport.total_bytes / 1024) +
                " KiB  " + "submitted=" + std::to_string(submitted) +
                " accepted=" + std::to_string(accepted) + " refused=" + std::to_string(rejected) +
                " concealed=" + std::to_string(concealed) +
                " failed=" + std::to_string(decode_errors));
    return nullptr;
}
} // namespace

namespace slopfin::player
{
void probe_decoder_support() noexcept
{
    probe_decoder_support_impl();
}

bool start(const jellyfin::PlaybackRequest &request, const std::string &title,
           double duration_seconds) noexcept
{
    const double start_seconds = request.start_seconds;
    if (g_running.load(std::memory_order_acquire))
    {
        trace::mark("player: start deferred; worker is still running");
        return false;
    }
    /* Reap the previous worker, including its local destructors and stack,
       before resetting any state it could still touch. */
    if (g_thread != nullptr)
    {
        if (scePthreadJoin(g_thread, nullptr) != 0)
        {
            trace::mark("player: could not join previous worker before start");
            return false;
        }
        g_thread = nullptr;
    }

    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_status = Status{};
        g_status.state = State::opening;
        g_status.title = title;
        g_status.duration_seconds = duration_seconds;
        g_status.position_seconds = start_seconds;
        g_status.audio_index = request.audio_index;
        g_status.subtitle_index = request.subtitle_index;
        g_status.max_bitrate = request.max_bitrate;
        g_clock_origin = -1.0;
        g_cadence_last_us = 0;
        g_held_intervals = 0;
        g_last_shown_us = 0;
        g_rate_started_us = 0;
        g_rate_started_shown = 0;
        for (PoolFrame &slot : g_pool)
            slot.state = SlotState::free;
    }
    /* Clear old subtitle cues when starting a different item. */
    /*
     * The display's own rate, for the cadence. It is read once here rather
     * than per frame: the console will not let a title change it, so it cannot
     * move underneath us.
     */
    {
        int display_w = 0;
        int display_h = 0;
        double hz = 0.0;
        g_display_hz = gfx::output_mode(display_w, display_h, hz) && hz > 0.0 ? hz : 59.94;
    }
    if (g_request.item_id != request.item_id)
        clear_subtitles();
    g_request = request;
    g_title = title;
    g_duration_seconds = duration_seconds;
    g_start_seconds = start_seconds;
    g_paused.store(false, std::memory_order_release);
    audio::set_paused(false);
    g_stop.store(false, std::memory_order_release);
    g_running.store(true, std::memory_order_release);

    pthread_attr_t attributes;
    const bool have = pthread_attr_init(&attributes) == 0;
    if (have)
        (void)pthread_attr_setstacksize(&attributes, 4u * 1024u * 1024u);
    const int created = scePthreadCreate(&g_thread, have ? &attributes : nullptr, playback_entry,
                                         nullptr, "slopfin-play");
    if (have)
        (void)pthread_attr_destroy(&attributes);
    /*
     * Off the render core . Created from the render thread,
     * this thread inherited its core, and it converts every picture -- about
     * 22 ms a 4K frame when an HDR source is tone mapped to SDR. Sharing the
     * core that must finish a frame every 16 ms held Backrooms to 21 fps with
     * 60% of render loops over 20 ms. CLAUDE.md's rule for background threads.
     */
    if (const std::uint64_t spare = images::spare_cores(); created == 0 && spare != 0)
        (void)scePthreadSetaffinity(g_thread, spare);
    if (created != 0)
    {
        trace::mark("player: thread creation failed result=" + std::to_string(created));
        g_thread = nullptr;
        g_running.store(false, std::memory_order_release);
        set_state(State::failed, "Could not start the playback thread.");
        return false;
    }
    return true;
}

void stop(bool preserve_output) noexcept
{
    double position = 0.0;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        position = g_status.position_seconds;
    }
    reporter::end(position);
    /* A paused worker may be waiting for audio room; let it run to its exit. */
    g_paused.store(false, std::memory_order_release);
    audio::set_paused(false);
    g_preserve_outputs_on_exit.store(preserve_output, std::memory_order_release);
    g_stop.store(true, std::memory_order_release);
    if (g_thread != nullptr)
    {
        if (scePthreadJoin(g_thread, nullptr) != 0)
        {
            trace::mark("player: worker join failed");
            return;
        }
        g_thread = nullptr;
    }
    g_preserve_outputs_on_exit.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> guard(g_mutex);
    g_status.state = State::idle;
    g_status.paused = false;
    if (!preserve_output)
        gfx::request_hdr_output(false);
}

void set_paused(bool paused) noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_paused.load(std::memory_order_acquire) == paused)
        return;
    const std::uint64_t now = now_us();
    if (paused)
        g_pause_started_us = now;
    else if (g_clock_origin >= 0.0)
        g_clock_started_us += now - g_pause_started_us;
    g_paused.store(paused, std::memory_order_release);
    g_status.paused = paused;
    audio::set_paused(paused);
}

bool paused() noexcept
{
    return g_paused.load(std::memory_order_acquire);
}

jellyfin::PlaybackRequest current_request() noexcept
{
    return g_request;
}

jellyfin::PlaybackRequest with_console_options(jellyfin::PlaybackRequest request) noexcept
{
    request.allow_software_audio = false;
    request.allow_bitstream = true;
    if (auto *marker = std::fopen("/data/slopfin-no-bitstream", "rb"))
    {
        request.allow_bitstream = false;
        std::fclose(marker);
    }
#ifdef SLOPFIN_SOFTWARE_AUDIO
    if (auto *marker = std::fopen("/data/slopfin-software-audio", "rb"))
    {
        request.allow_software_audio = true;
        std::fclose(marker);
    }
#endif
    if (auto *marker = std::fopen("/data/slopfin-dv-hdr10", "rb"))
    {
        std::fclose(marker);
        request.allow_dv_hdr10_base = true;
    }
    else
        request.allow_dv_hdr10_base = false;
    return request;
}

bool restart(const jellyfin::PlaybackRequest &request, bool preserve_output) noexcept
{
    const std::string title = g_title;
    const double duration = g_duration_seconds;
    stop(preserve_output);
    return start(request, title, duration);
}

bool seek(double seconds) noexcept
{
    jellyfin::PlaybackRequest request = g_request;
    const double limit = g_duration_seconds > 10.0 ? g_duration_seconds - 5.0 : seconds;
    request.start_seconds = std::clamp(seconds, 0.0, std::max(0.0, limit));
    /* Only the timeline changes. Keep the current HDR scanout and HDMI audio
       port alive so the TV does not see every skip as a new format session. */
    return restart(request, true);
}

void show_text_subtitle(int index) noexcept
{
    g_request.subtitle_index = index;
    std::string source;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_status.subtitle_index = index;
        source = g_status.media_source_id;
    }
    if (index < 0 || source.empty())
    {
        clear_subtitles();
        return;
    }
    request_subtitles("/Videos/" + g_request.item_id + "/" + source + "/Subtitles/" +
                      std::to_string(index) + "/0/Stream.srt");
}

Status status() noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    Status snapshot = g_status;
    // Rate updates arrive with new pictures. A stalled or paused picture must
    // not keep advertising the last healthy FPS indefinitely.
    if (snapshot.paused || (g_last_shown_us && now_us() - g_last_shown_us >= 1000000))
        snapshot.frame_rate = 0.0;
    return snapshot;
}

const gfx::Bitmap *frame() noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);

    std::size_t showing = kFramePool;
    for (std::size_t i = 0; i < kFramePool; ++i)
    {
        if (g_pool[i].state == SlotState::displaying)
            showing = i;
    }

    /*
     * Video follows the audio clock when there is one. The audio output runs
     * on its own crystal, so pacing the picture against anything else drifts
     * away from the sound over the length of a title. The wall clock is only
     * the fallback for a stream with no audio.
     */
    if (audio::failed())
    {
        g_status.state = State::failed;
        g_status.message =
            "Audio could not decode or refill its buffer. Try server audio conversion.";
        g_stop.store(true, std::memory_order_release);
        return showing == kFramePool ? nullptr : &g_pool[showing].bitmap;
    }
    if (audio::buffering())
    {
        g_status.frame_rate = 0.0;
        if (showing != kFramePool)
            return &g_pool[showing].bitmap;
        // Show the first completed frame while both clocks wait. Do not consume
        // ready slots or claim advancing pictures during startup buffering.
        const PoolFrame *first = nullptr;
        for (const auto &slot : g_pool)
            if (slot.state == SlotState::ready && (!first || slot.seconds < first->seconds))
                first = &slot;
        return first ? &first->bitmap : nullptr;
    }
    const double audio_now = audio::clock_seconds();
    if (audio_now >= 0.0)
    {
        const std::uint64_t now = now_us();
        const double shown_at = showing != kFramePool ? g_pool[showing].seconds : -1.0;

        /*
         * How many display intervals have actually passed. Counting calls
         * instead would lose a missed vblank, and a missed vblank is exactly
         * when the cadence most needs to know.
         */
        const double interval_us = g_display_hz > 0.0 ? 1000000.0 / g_display_hz : 16683.0;
        double intervals = 1.0;
        if (g_cadence_last_us != 0)
        {
            intervals = static_cast<double>(now - g_cadence_last_us) / interval_us;
            intervals = std::floor(intervals + 0.5);
            if (intervals < 1.0)
                intervals = 1.0;
            if (intervals > 8.0)
                intervals = 8.0;
        }
        g_cadence_last_us = now;
        g_held_intervals += static_cast<int>(intervals);

        /* Present the newest due picture and free older ones; select by media timestamp rather than
         * a free-running display phase. */
        std::size_t best = kFramePool;
        for (std::size_t i = 0; i < kFramePool; ++i)
        {
            if (g_pool[i].state != SlotState::ready)
                continue;
            if (g_pool[i].seconds <= shown_at)
            {
                /* Decoded, then overtaken before it was ever displayed. */
                ++g_status.dropped;
                g_pool[i].state = SlotState::free;
                continue;
            }
            if (g_pool[i].seconds > audio_now)
                continue;
            if (best == kFramePool || g_pool[i].seconds > g_pool[best].seconds)
                best = i;
        }
        if (best != kFramePool)
        {
            for (std::size_t i = 0; i < kFramePool; ++i)
            {
                if (g_pool[i].state == SlotState::ready && g_pool[i].seconds < g_pool[best].seconds)
                {
                    ++g_status.dropped;
                    g_pool[i].state = SlotState::free;
                }
            }
            if (showing != kFramePool)
                g_pool[showing].state = SlotState::free;
            g_pool[best].state = SlotState::displaying;
            showing = best;
            note_presentation(now);
            g_status.position_seconds = title_seconds(g_pool[best].seconds);
        }

        int ready_count = 0;
        for (const PoolFrame &slot : g_pool)
            ready_count += slot.state == SlotState::ready ? 1 : 0;
        g_status.queued = ready_count;
        g_status.audio_lead_ms = static_cast<int>(
            (showing != kFramePool ? (g_pool[showing].seconds - audio_now) * 1000.0 : 0.0));
        reporter::update(g_status.position_seconds, g_paused.load(std::memory_order_acquire));
        return showing != kFramePool && g_pool[showing].bitmap.valid() ? &g_pool[showing].bitmap
                                                                       : nullptr;
    }

    /* Start the clock on the first frame that arrives, then follow the wall. */
    if (g_clock_origin < 0.0)
    {
        std::size_t earliest = kFramePool;
        for (std::size_t i = 0; i < kFramePool; ++i)
        {
            if (g_pool[i].state != SlotState::ready)
                continue;
            if (earliest == kFramePool || g_pool[i].seconds < g_pool[earliest].seconds)
                earliest = i;
        }
        if (earliest == kFramePool)
            return nullptr;
        g_clock_origin = g_pool[earliest].seconds;
        g_clock_started_us = now_us();
    }

    /* Paused: the clock stands still at the moment pause was pressed. */
    const std::uint64_t now =
        g_paused.load(std::memory_order_acquire) ? g_pause_started_us : now_us();
    const double due = g_clock_origin + static_cast<double>(now - g_clock_started_us) / 1000000.0;

    /*
     * The newest frame whose time has come. Anything at or before what is
     * already showing arrived too late to be useful and is dropped, so the
     * picture never steps backwards when timestamps arrive out of order.
     */
    ++g_held_intervals;
    const double shown_seconds = showing != kFramePool ? g_pool[showing].seconds : -1.0;
    std::size_t chosen = kFramePool;
    for (std::size_t i = 0; i < kFramePool; ++i)
    {
        if (g_pool[i].state != SlotState::ready)
            continue;
        if (g_pool[i].seconds <= shown_seconds)
        {
            ++g_status.dropped;
            g_pool[i].state = SlotState::free;
            continue;
        }
        if (g_pool[i].seconds > due)
            continue;
        if (chosen == kFramePool || g_pool[i].seconds > g_pool[chosen].seconds)
            chosen = i;
    }

    if (chosen != kFramePool)
    {
        /* Discard anything that fell due before the frame being shown. */
        for (std::size_t i = 0; i < kFramePool; ++i)
        {
            if (g_pool[i].state == SlotState::ready && g_pool[i].seconds < g_pool[chosen].seconds)
            {
                ++g_status.dropped;
                g_pool[i].state = SlotState::free;
            }
        }
        if (showing != kFramePool)
            g_pool[showing].state = SlotState::free;
        g_pool[chosen].state = SlotState::displaying;
        showing = chosen;

        note_presentation(now);
        g_status.position_seconds = title_seconds(g_pool[chosen].seconds);
    }

    int ready = 0;
    for (const PoolFrame &slot : g_pool)
        ready += slot.state == SlotState::ready ? 1 : 0;
    g_status.queued = ready;
    reporter::update(g_status.position_seconds, g_paused.load(std::memory_order_acquire));

    return showing != kFramePool && g_pool[showing].bitmap.valid() ? &g_pool[showing].bitmap
                                                                   : nullptr;
}

} // namespace slopfin::player
