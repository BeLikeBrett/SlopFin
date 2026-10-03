/*
 * SlopFin - audio decode and output.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Derived from ProsperoTV's iptv_native_backend.c, copyright
 * BlackBearReloaded, GPL-3.0-or-later.
 */

#include "audio.hpp"
#include "audio_buffer_gate.hpp"
#include "software_audio.hpp"
#include "adts_frames.hpp"
#include "ac3.hpp"
#include "iec61937.hpp"

#include "bigalloc.hpp"
#include "trace.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <string>
#include <span>
#include <vector>

extern "C"
{
    int sceKernelUsleep(std::uint32_t microseconds);
    int scePthreadCreate(void **thread, const void *attr, void *(*entry)(void *), void *argument,
                         const char *name);
    int scePthreadJoin(void *thread, void **result);
    int sceSysmoduleLoadModule(std::uint16_t module_id);
    int sceSysmoduleUnloadModule(std::uint16_t module_id);
}

namespace
{
using slopfin::audio::Codec;

constexpr std::uint16_t kAudioModuleId = 0x0088;
constexpr std::uint32_t kCodecMp3 = 2;
constexpr std::uint32_t kCodecAac = 3;
constexpr std::int32_t kWordSize16 = 1;

/* The output takes a fixed number of stereo frames per call. */
constexpr std::uint32_t kMaxGrain = 2048;
// 512 restores real-time stereo output on FW 8.20; 256 ran about 4% slow.
std::uint32_t g_grain = 512;
constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kFormatStereoS16 = 1;
/* Format 6 is signed 16-bit eight-channel in the standard speaker order, which
   is what an HDMI receiver expects; the system encodes it to Dolby Digital or
   DTS on the way out if the receiver asks for a bitstream. */
constexpr std::uint32_t kFormat8chStdS16 = 6;
constexpr std::uint32_t kMaxChannels = 8;
constexpr int kVolume0dB = 0x8000;

/* Cover the 1.5–2 second socket gaps observed on high-bitrate local playback. */
/* PCM reservoir capacity is 16 seconds. Occupancy backpressure bounds decode-ahead; retain the
 * producer gate. */
constexpr std::size_t kRingFrames = kRate * 16;

struct AudiodecAuInfo
{
    std::uint32_t size;
    void *address;
    std::uint32_t length;
};

struct AudiodecPcmItem
{
    std::uint32_t size;
    void *address;
    std::uint32_t length;
};

struct AudiodecControl
{
    void *param;
    void *stream_info;
    AudiodecAuInfo *au_info;
    AudiodecPcmItem *pcm_item;
};

struct AudiodecParamAac
{
    std::uint32_t size;
    std::int32_t word_size;
    std::uint32_t config_number;
    std::uint32_t sampling_frequency_index;
    std::uint32_t max_channels;
    std::uint32_t enable_he_aac;
};

struct AudiodecParamMp3
{
    std::uint32_t size;
    std::int32_t word_size;
};

struct AudiodecAacInfo
{
    std::uint32_t size;
    std::uint32_t sampling_frequency;
    std::uint32_t channel_count;
    std::uint32_t he_aac;
    std::int32_t result;
};

struct AudiodecMp3Info
{
    std::uint32_t size;
    std::uint32_t header;
    std::uint8_t crc;
    std::uint8_t mode;
    std::uint8_t mode_extension;
    std::uint8_t copyright;
    std::uint8_t original;
    std::uint8_t emphasis;
    std::array<std::uint8_t, 2> reserved;
    std::int32_t result;
};

struct Mp3Frame
{
    std::size_t offset = 0;
    std::size_t bytes = 0;
    std::uint32_t rate = kRate;
    std::uint32_t channels = 2;
};

static_assert(sizeof(AudiodecParamMp3) == 8);
static_assert(sizeof(AudiodecMp3Info) == 20);

extern "C"
{
    int sceAudiodecInitLibrary(std::uint32_t codec_type);
    int sceAudiodecTermLibrary(std::uint32_t codec_type);
    int sceAudiodecCreateDecoder(AudiodecControl *control, std::uint32_t codec_type);
    int sceAudiodecDeleteDecoder(int handle);
    int sceAudiodecDecode(int handle, AudiodecControl *control);
    int sceAudioOutInit();
    int sceAudioOutOpen(int user_id, int type, int index, std::uint32_t length,
                        std::uint32_t frequency, std::uint32_t format);
    int sceAudioOutClose(int handle);
    int sceAudioOutOutput(int handle, const void *samples);
    int sceAudioOutSetVolume(int handle, int flags, const int *volumes);
    /* The disc player's HDMI bitstream calls; see tools/bitstream/README.md. */
    int sceAudioOutExConfigureOutput(int zero, long unused, int mode, int device, long unused2);
    int sceAudioOutExOpen(int user, int mode);
    int sceAudioOutExClose(int handle);
}

/* Decoder state, touched only by the thread that calls submit. */
Codec g_codec = Codec::aac_adts;
std::atomic<bool> g_decode_failed{false};
#ifdef SLOPFIN_SOFTWARE_AUDIO
slopfin::software_audio::Decoder g_software_decoder;
#endif
bool software_codec(Codec codec) noexcept
{
    return codec == Codec::truehd || codec == Codec::eac3 || codec == Codec::dts;
}
alignas(64) AudiodecParamAac g_param{};
AudiodecParamMp3 g_mp3_param{};
slopfin::audio::Ac3Param g_ac3_param{};
slopfin::audio::Ac3Info g_ac3_info{};
slopfin::audio::Ac3Frames g_ac3_frames;
slopfin::audio::AdtsFrames g_adts_frames;

/*
 * Bitstream passthrough. AC-3, E-AC-3 and DTS go to the TV undecoded, packed
 * as IEC 61937 into a stereo port the console opens for exactly that -- the
 * path the disc player uses, proven from SlopFin on 2026-09-15. The ring then
 * holds packed 16-bit words, and a "frame" is one stereo frame of the carrier,
 * which runs at 192 kHz for E-AC-3.
 */
bool g_bitstream = false;
std::uint32_t g_output_rate = kRate;
std::uint64_t g_bitstream_queued = 0; /* carrier frames queued, for the clock origin */
slopfin::audio::iec61937::Packer g_packer;

bool bitstream_candidate(Codec codec) noexcept
{
    if (codec != Codec::ac3 && codec != Codec::eac3 && codec != Codec::dts)
        return false;
    if (std::FILE *marker = std::fopen("/data/slopfin-no-bitstream", "rb"))
    {
        std::fclose(marker);
        return false;
    }
    return true;
}

/* Mode numbers from the library's ExConfigureOutput table. */
int bitstream_mode(Codec codec) noexcept
{
    return codec == Codec::eac3 ? 3 : codec == Codec::dts ? 2 : 0;
}

/* Converts carrier frames to the 48 kHz counts the buffer gate works in. */
std::uint32_t at_48k(std::size_t frames) noexcept
{
    return static_cast<std::uint32_t>(frames * kRate / g_output_rate);
}
void *g_thread = nullptr;
AudiodecAacInfo g_info{};
AudiodecMp3Info g_mp3_info{};
alignas(64) AudiodecAuInfo g_au{};
alignas(64) AudiodecPcmItem g_pcm_item{};
alignas(64) AudiodecControl g_control{};
int g_decoder = -1;
/* sceAudiodec access-unit and PCM buffers require explicit alignment. */
alignas(256) std::array<std::uint8_t, 64 * 1024> g_pcm{};

int g_sink = -1;
/* A seek can tear down stream/decoder state without dropping the HDMI port. */
bool g_output_preserved = false;
/* Channels actually written to the sink: 2 until a stream proves it needs more. */
std::uint32_t g_out_channels = 2;
bool g_module_loaded = false;
bool g_library_ready = false;
bool g_output_initialized = false;
std::atomic<bool> g_running{false};
std::atomic<bool> g_quit{false};
/* Suppresses the final drain call when a seek is keeping the HDMI port live. */
std::atomic<bool> g_preserve_request{false};
std::atomic<bool> g_paused{false};

/* Decoded stereo samples waiting for the output thread. */
std::mutex g_mutex;
std::condition_variable g_space_changed;
// Reused across tracks; the multi-megabyte reservoir belongs in flexible memory.
std::span<std::int16_t> g_ring;
std::size_t g_read = 0;
std::size_t g_write = 0;
std::size_t g_filled = 0;
std::uint64_t g_frames_played = 0;
std::uint64_t g_underrun_frames = 0;
std::uint32_t g_output_errors = 0;
slopfin::audio::BufferGate g_buffer_gate;
bool g_video_ready = false, g_audio_eof = false;
std::uint64_t g_buffering_frames = 0;
std::uint64_t g_software_frames = 0, g_software_work_us = 0, g_software_queue_us = 0,
              g_software_bytes = 0;

/* The sample rate the stream actually decodes at, for the clock. */

/* Stream position of the very first sample handed to the output. */
double g_origin_seconds = -1.0;

/*
 * Development aid: a copy of the decoded samples, for off-console inspection.
 * Four seconds of eight-channel audio is three megabytes, which does not fit in
 * the roughly two-megabyte process heap, so it lives in flexible memory.
 */
constexpr std::size_t kCaptureSamples = kRate * kMaxChannels * 4;
std::int16_t *g_capture = nullptr;
std::size_t g_capture_used = 0;
bool g_capturing = false;
std::uint32_t g_capture_channels = 2;

/* Width the sink should be running at; the output thread owns the reopen. */
std::atomic<std::uint32_t> g_desired_channels{2};

/*
 * A decoded AAC frame is laid out centre-first (C, L, R, Ls, Rs, LFE for 5.1),
 * but an HDMI receiver expects the standard order (FL, FR, FC, LFE, BL, BR,
 * SL, SR). These tables give, for each output slot, the source channel to take
 * it from, or -1 for silence.
 */
constexpr int kSilent = -1;
constexpr int kChannelMap[9][kMaxChannels] = {
    /* 0 */ {kSilent, kSilent, kSilent, kSilent, kSilent, kSilent, kSilent, kSilent},
    /* 1 C        */ {0, 0, kSilent, kSilent, kSilent, kSilent, kSilent, kSilent},
    /* 2 L R      */ {0, 1, kSilent, kSilent, kSilent, kSilent, kSilent, kSilent},
    /* 3 C L R    */ {1, 2, 0, kSilent, kSilent, kSilent, kSilent, kSilent},
    /* 4 +Cs      */ {1, 2, 0, kSilent, 3, 3, kSilent, kSilent},
    /* 5 +Ls Rs   */ {1, 2, 0, kSilent, 3, 4, kSilent, kSilent},
    /* 6 5.1      */ {1, 2, 0, 5, 3, 4, kSilent, kSilent},
    /* 7          */ {1, 2, 0, 6, 3, 4, 5, kSilent},
    /* 8 7.1      */ {1, 2, 0, 7, 5, 6, 3, 4},
};

std::uint32_t native_codec(Codec codec) noexcept
{
    return codec == Codec::ac3 ? 4 : codec == Codec::mp3 ? kCodecMp3 : kCodecAac;
}

const char *codec_name(Codec codec) noexcept
{
    if (g_bitstream)
        return codec == Codec::eac3  ? "eac3 (bitstream)"
               : codec == Codec::dts ? "dts (bitstream)"
                                     : "ac3 (bitstream)";
    return codec == Codec::truehd ? "truehd (CPU)"
           : codec == Codec::eac3 ? "eac3 (CPU)"
           : codec == Codec::dts  ? "dts (CPU)"
           : codec == Codec::ac3  ? "ac3"
           : codec == Codec::mp3  ? "mp3"
                                  : "aac";
}

std::uint32_t adts_frame_length(const std::uint8_t *data, std::size_t available) noexcept
{
    if (available < 7 || data[0] != 0xff || (data[1] & 0xf0) != 0xf0)
        return 0;
    const auto length = (static_cast<std::uint32_t>(data[3] & 0x03) << 11) |
                        (static_cast<std::uint32_t>(data[4]) << 3) |
                        (static_cast<std::uint32_t>(data[5]) >> 5);
    return length >= 7 && length <= available ? length : 0;
}

bool parse_mp3_frame(const std::uint8_t *input, std::size_t available, std::size_t offset,
                     Mp3Frame &frame) noexcept
{
    static constexpr std::array<std::uint16_t, 16> kMpeg1Bitrates{
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static constexpr std::array<std::uint16_t, 16> kMpeg2Bitrates{
        0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static constexpr std::array<std::uint32_t, 3> kBaseRates{44100, 48000, 32000};

    if (offset + 4 > available || input[offset] != 0xff || (input[offset + 1] & 0xe0) != 0xe0)
        return false;

    const std::uint32_t version = (input[offset + 1] >> 3) & 3;
    const std::uint32_t layer = (input[offset + 1] >> 1) & 3;
    const std::uint32_t bitrate_index = input[offset + 2] >> 4;
    const std::uint32_t rate_index = (input[offset + 2] >> 2) & 3;
    if (version == 1 || layer != 1 || bitrate_index == 0 || bitrate_index == 15 || rate_index == 3)
        return false;

    std::uint32_t rate = kBaseRates[rate_index];
    if (version == 2)
        rate /= 2;
    else if (version == 0)
        rate /= 4;

    const bool mpeg1 = version == 3;
    const std::uint32_t bitrate =
        (mpeg1 ? kMpeg1Bitrates[bitrate_index] : kMpeg2Bitrates[bitrate_index]) * 1000;
    const std::size_t bytes =
        (mpeg1 ? 144u : 72u) * bitrate / rate + ((input[offset + 2] >> 1) & 1u);
    if (bytes < 4 || bytes > 1441 || offset + bytes > available)
        return false;

    frame.offset = offset;
    frame.bytes = bytes;
    frame.rate = rate;
    frame.channels = (input[offset + 3] >> 6) == 3 ? 1u : 2u;
    return true;
}

bool next_mp3_frame(const std::uint8_t *input, std::size_t available, std::size_t start,
                    Mp3Frame &frame) noexcept
{
    for (std::size_t offset = start; offset + 4 <= available; ++offset)
        if (parse_mp3_frame(input, available, offset, frame))
            return true;
    return false;
}

void *output_entry(void *) noexcept
{
    std::array<std::int16_t, kMaxGrain * kMaxChannels> block{};
    while (!g_quit.load(std::memory_order_acquire))
    {
        /*
         * The stream's channel count is only known after the first frame
         * decodes, so the sink is re-opened here rather than at start-up. The
         * output thread does it because it is the thread blocked inside
         * sceAudioOutOutput; closing the handle underneath it would race.
         */
        if (const std::uint32_t desired = g_desired_channels.load(std::memory_order_acquire);
            !g_bitstream && desired != g_out_channels)
        {
            const std::uint32_t format = desired > 2 ? kFormat8chStdS16 : kFormatStereoS16;
            if (g_sink >= 0)
                (void)sceAudioOutClose(g_sink);
            g_sink = sceAudioOutOpen(0xff, 0, 0, g_grain, kRate, format);
            if (g_sink < 0)
            {
                /* Fall back to the layout that is known to open. */
                g_sink = sceAudioOutOpen(0xff, 0, 0, g_grain, kRate, kFormatStereoS16);
                g_desired_channels.store(2, std::memory_order_release);
                slopfin::trace::mark("audio: multichannel output refused, staying stereo");
            }
            const int volumes[kMaxChannels] = {kVolume0dB, kVolume0dB, kVolume0dB, kVolume0dB,
                                               kVolume0dB, kVolume0dB, kVolume0dB, kVolume0dB};
            (void)sceAudioOutSetVolume(g_sink, 0xfff, volumes);
            slopfin::trace::mark("audio: sink now " + std::to_string(desired) +
                                 " channels, handle " + std::to_string(g_sink));
            {
                std::lock_guard<std::mutex> guard(g_mutex);
                g_out_channels = g_desired_channels.load(std::memory_order_acquire);
                g_read = 0;
                g_write = 0;
                g_filled = 0;
                g_space_changed.notify_all();
            }
        }

        if (g_paused.load(std::memory_order_acquire))
        {
            /* Silence at the normal cadence keeps the port alive; the ring and
               the clock are left untouched so playback resumes exactly. */
            block.fill(0);
            (void)sceAudioOutOutput(g_sink, block.data());
            continue;
        }

        bool have = false, buffering_wait = false;
        std::size_t wanted = g_grain * 2;
        {
            std::lock_guard<std::mutex> guard(g_mutex);
            wanted = g_grain * g_out_channels;
            const std::size_t frames = g_filled / g_out_channels;
            const bool ready =
                g_buffer_gate.ready(at_48k(frames), at_48k(g_grain), g_video_ready, g_audio_eof) &&
                frames >= g_grain;
            buffering_wait = g_buffer_gate.waiting();
            if (g_buffer_gate.failed())
                g_decode_failed.store(true);
            if (ready && g_filled >= wanted)
            {
                const auto head = std::min(wanted, g_ring.size() - g_read);
                std::memcpy(block.data(), g_ring.data() + g_read, head * sizeof(std::int16_t));
                std::memcpy(block.data() + head, g_ring.data(),
                            (wanted - head) * sizeof(std::int16_t));
                g_read = (g_read + wanted) % g_ring.size();
                g_filled -= wanted;
                g_space_changed.notify_all();
                have = true;
            }
        }
        if (!have)
        {
            /* Starved: emit silence so the output keeps its cadence rather
               than stalling, which would also stall the clock video follows. */
            block.fill(0);
        }

        // Capture submitted blocks, including underrun silence. Capturing only
        // producer PCM hid gaps introduced between decode and the output sink.
        {
            std::lock_guard<std::mutex> guard(g_mutex);
            if (!g_bitstream && g_capturing && g_capture &&
                g_capture_used + wanted <= kCaptureSamples)
            {
                if (g_capture_channels != g_out_channels)
                    g_capture_used = 0;
                std::memcpy(g_capture + g_capture_used, block.data(),
                            wanted * sizeof(std::int16_t));
                g_capture_used += wanted;
                g_capture_channels = g_out_channels;
            }
        }

        /* This call blocks until the block is consumed, which is what paces
           the whole loop; there is no sleep here by design. */
        const int result = sceAudioOutOutput(g_sink, block.data());
        {
            std::lock_guard<std::mutex> guard(g_mutex);
            if (result < 0)
                ++g_output_errors;
            else if (have)
                g_frames_played += g_grain;
            else if (buffering_wait)
                g_buffering_frames += g_grain;
            else if (!g_audio_eof)
                g_underrun_frames += g_grain;
        }
    }
    if (!g_preserve_request.load(std::memory_order_acquire))
        (void)sceAudioOutOutput(g_sink, nullptr);
    return nullptr;
}
} // namespace

namespace slopfin::audio
{

bool start(Codec codec, bool buffered_start) noexcept
{
    if (g_running.load(std::memory_order_acquire))
        return true;

    const bool wants_bitstream = bitstream_candidate(codec);
    const bool reuse_output =
        g_output_preserved && g_sink >= 0 && codec == g_codec && wants_bitstream == g_bitstream;
    if (g_output_preserved && !reuse_output)
    {
        /* The format changed while an output was parked for a seek. Fall back
           to a real format restart rather than reusing the wrong HDMI mode. */
        if (g_sink >= 0 && g_bitstream)
        {
            (void)sceAudioOutExClose(g_sink);
            (void)sceAudioOutExConfigureOutput(0, 0, 255, 255, 0);
        }
        else if (g_sink >= 0)
            (void)sceAudioOutClose(g_sink);
        g_sink = -1;
        g_bitstream = false;
        g_output_rate = kRate;
    }
    g_output_preserved = false;

    if (!g_module_loaded)
    {
        if (sceSysmoduleLoadModule(kAudioModuleId) < 0)
        {
            trace::mark("audio: module would not load");
            return false;
        }
        g_module_loaded = true;
    }

    if (!reuse_output)
    {
        // Console-only pacing experiment. Read once before the output owner starts.
        g_grain = 512;
        if (auto *marker = std::fopen("/data/slopfin-audio-grain", "rb"))
        {
            unsigned requested = 0;
            if (std::fscanf(marker, "%u", &requested) == 1 &&
                (requested == 256 || requested == 512 || requested == 1024 || requested == 2048))
                g_grain = requested;
            std::fclose(marker);
        }
        trace::mark("audio: output grain " + std::to_string(g_grain));
    }
    g_codec = codec;
    g_ac3_frames.reset();
    g_adts_frames.reset();
    g_decode_failed.store(false);
    if (!reuse_output)
    {
        g_bitstream = false;
        g_output_rate = kRate;
        if (wants_bitstream)
        {
            if (!g_output_initialized && sceAudioOutInit() >= 0)
                g_output_initialized = true;
            const int mode = bitstream_mode(codec);
            const int configured =
                g_output_initialized ? sceAudioOutExConfigureOutput(0, 0, mode, 255, 0) : -1;
            const int handle = configured >= 0 ? sceAudioOutExOpen(0xff, mode) : -1;
            if (handle >= 0)
            {
                g_bitstream = true;
                g_sink = handle;
                g_output_rate = codec == Codec::eac3 ? 192000 : kRate;
                g_grain = codec == Codec::eac3 ? 1024 : 256; /* the port's own grain */
            }
            else if (configured >= 0)
                (void)sceAudioOutExConfigureOutput(0, 0, 255, 255, 0);
            char line[128];
            std::snprintf(line, sizeof(line),
                          "audio: bitstream mode %d configure 0x%08x open 0x%08x", mode,
                          static_cast<unsigned>(configured), static_cast<unsigned>(handle));
            trace::mark(line);
        }
    }
    else
        trace::mark(std::string{"audio: reusing HDMI output for seek ("} + codec_name(codec) + ")");

    if (g_bitstream)
        g_packer.reset(codec == Codec::eac3  ? iec61937::Format::eac3
                       : codec == Codec::dts ? iec61937::Format::dts
                                             : iec61937::Format::ac3);
    if (g_bitstream)
    {
        /* Nothing to decode: the TV does it. */
    }
    else if (software_codec(codec))
    {
#ifdef SLOPFIN_SOFTWARE_AUDIO
        const auto id = codec == Codec::truehd ? software_audio::Codec::truehd
                        : codec == Codec::eac3 ? software_audio::Codec::eac3
                                               : software_audio::Codec::dts;
        if (!g_software_decoder.open(id))
        {
            stop();
            return false;
        }
#else
        stop();
        return false;
#endif
    }
    else
    {
        const std::uint32_t decoder_codec = native_codec(g_codec);
        if (sceAudiodecInitLibrary(decoder_codec) < 0)
        {
            trace::mark("audio: decoder library would not start");
            stop();
            return false;
        }
        g_library_ready = true;

        g_info = AudiodecAacInfo{};
        g_info.size = sizeof(g_info);
        g_mp3_info = AudiodecMp3Info{};
        g_mp3_info.size = sizeof(g_mp3_info);
        g_au = AudiodecAuInfo{};
        g_au.size = sizeof(g_au);
        g_pcm_item = AudiodecPcmItem{};
        g_pcm_item.size = sizeof(g_pcm_item);
        if (g_codec == Codec::ac3)
        {
            g_ac3_param = Ac3Param{};
            g_ac3_info = Ac3Info{};
            g_control.param = &g_ac3_param;
            g_control.stream_info = &g_ac3_info;
        }
        else if (g_codec == Codec::mp3)
        {
            g_mp3_param = AudiodecParamMp3{sizeof(AudiodecParamMp3), kWordSize16};
            g_control.param = &g_mp3_param;
            g_control.stream_info = &g_mp3_info;
        }
        else
        {
            /* Frequency index 4 is 44.1 kHz; the decoder reports the real rate back
               in its stream info, and the output runs at a fixed 48 kHz. */
            /*
             * max_channels is an upper bound, and the decoder refuses a stream that
             * exceeds it: asking for 2 is exactly why surround content used to come
             * back as an error. Eight admits everything up to 7.1; the stream info
             * reports what a given frame actually carried.
             */
            g_param =
                AudiodecParamAac{sizeof(AudiodecParamAac), kWordSize16, 1, 4, kMaxChannels, 1};
            g_control.param = &g_param;
            g_control.stream_info = &g_info;
        }
        g_control.au_info = &g_au;
        g_control.pcm_item = &g_pcm_item;

        g_decoder = sceAudiodecCreateDecoder(&g_control, decoder_codec);
        if (g_decoder < 0)
        {
            trace::mark("audio: decoder would not open");
            stop();
            return false;
        }

    } // native decoder setup

    if (!g_output_initialized && sceAudioOutInit() < 0)
    {
        trace::mark("audio: output would not start");
        stop();
        return false;
    }
    g_output_initialized = true;
    if (!g_bitstream && !reuse_output)
        g_sink = sceAudioOutOpen(0xff, 0, 0, g_grain, kRate, kFormatStereoS16);
    if (g_sink < 0)
    {
        trace::mark("audio: output would not open");
        stop();
        return false;
    }
    const int volumes[8] = {kVolume0dB, kVolume0dB, kVolume0dB, kVolume0dB,
                            kVolume0dB, kVolume0dB, kVolume0dB, kVolume0dB};
    if (!g_bitstream && !reuse_output) /* volume scaling would corrupt the packed words */
        (void)sceAudioOutSetVolume(g_sink, 3, volumes);

    if (g_ring.empty())
    {
        constexpr auto samples = kRingFrames * kMaxChannels;
        auto *storage =
            static_cast<std::int16_t *>(bigalloc::allocate(samples * sizeof(std::int16_t)));
        if (!storage)
        {
            trace::mark("audio: PCM reservoir allocation failed");
            stop();
            return false;
        }
        g_ring = {storage, samples};
    }

    {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (!reuse_output)
            g_out_channels = 2;
        g_desired_channels.store(g_out_channels, std::memory_order_release);
        g_read = 0;
        g_write = 0;
        g_filled = 0;
        g_frames_played = 0;
        g_underrun_frames = 0;
        g_output_errors = 0;
        g_buffer_gate.reset(buffered_start);
        g_video_ready = g_audio_eof = false;
        g_buffering_frames = 0;
        g_software_frames = g_software_work_us = g_software_queue_us = g_software_bytes = 0;
        g_origin_seconds = -1.0;
        g_bitstream_queued = 0;
    }

    g_quit.store(false, std::memory_order_release);
    pthread_attr_t attributes;
    const bool have = pthread_attr_init(&attributes) == 0;
    if (have)
        (void)pthread_attr_setstacksize(&attributes, 512u * 1024u);
    const int created = scePthreadCreate(&g_thread, have ? &attributes : nullptr, output_entry,
                                         nullptr, "slopfin-audio");
    if (have)
        (void)pthread_attr_destroy(&attributes);
    if (created != 0)
    {
        g_thread = nullptr;
        trace::mark("audio: output thread would not start");
        stop();
        return false;
    }

    g_running.store(true, std::memory_order_release);
    trace::mark(std::string{"audio: ready "} + codec_name(g_codec));
    return true;
}

void interrupt() noexcept
{
    g_paused.store(false, std::memory_order_release);
    g_quit.store(true, std::memory_order_release);
    g_space_changed.notify_all();
}

void stop(bool preserve_output) noexcept
{
    g_preserve_request.store(preserve_output && g_sink >= 0, std::memory_order_release);
    interrupt();
    /* The output worker must finish its final hardware write before closing
       the sink or unloading its module. Joining also reclaims its stack. */
    if (g_thread != nullptr)
    {
        if (scePthreadJoin(g_thread, nullptr) != 0)
        {
            trace::mark("audio: worker join failed");
            return;
        }
        g_thread = nullptr;
    }

    const bool keep_output = preserve_output && g_sink >= 0;
    if (!keep_output)
    {
        /* A port parked by an earlier seek never got the worker's final drain.
           If it is being discarded instead of reused, drain it now. */
        if (g_output_preserved && g_sink >= 0)
            (void)sceAudioOutOutput(g_sink, nullptr);
        if (g_sink >= 0 && g_bitstream)
        {
            (void)sceAudioOutExClose(g_sink);
            (void)sceAudioOutExConfigureOutput(0, 0, 255, 255, 0);
        }
        else if (g_sink >= 0)
            (void)sceAudioOutClose(g_sink);
        g_sink = -1;
        g_bitstream = false;
        g_output_preserved = false;
    }
    else
    {
        g_output_preserved = true;
        trace::mark("audio: HDMI output preserved across seek");
    }
    if (g_decoder >= 0)
        (void)sceAudiodecDeleteDecoder(g_decoder);
    g_decoder = -1;
#ifdef SLOPFIN_SOFTWARE_AUDIO
    g_software_decoder.close();
#endif
    if (g_library_ready)
        (void)sceAudiodecTermLibrary(native_codec(g_codec));
    g_library_ready = false;
    if (g_module_loaded && !keep_output)
        (void)sceSysmoduleUnloadModule(kAudioModuleId);
    if (!keep_output)
        g_module_loaded = false;
    g_running.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> guard(g_mutex);
    g_origin_seconds = -1.0;
    g_frames_played = 0;
    if (!keep_output)
        g_output_rate = kRate;
    g_preserve_request.store(false, std::memory_order_release);
}

/* Queues one packed burst; blocks while the reservoir is full. */
static void queue_bitstream(const std::uint16_t *words, std::size_t count,
                            std::int64_t pts) noexcept
{
    if (count == 0 || count % 2 != 0 || count > g_ring.size())
        return;
    std::unique_lock<std::mutex> guard(g_mutex);
    while (!g_quit.load(std::memory_order_acquire) && g_filled + count > g_ring.size())
        g_space_changed.wait_for(guard, std::chrono::milliseconds(100));
    if (g_quit.load(std::memory_order_acquire))
        return;
    if (g_origin_seconds < 0.0 && pts >= 0)
        g_origin_seconds =
            static_cast<double>(pts) / 90000.0 -
            static_cast<double>(g_bitstream_queued) / static_cast<double>(g_output_rate);
    const std::size_t head = std::min(count, g_ring.size() - g_write);
    std::memcpy(g_ring.data() + g_write, words, head * sizeof(std::int16_t));
    std::memcpy(g_ring.data(), words + head, (count - head) * sizeof(std::int16_t));
    g_write = (g_write + count) % g_ring.size();
    g_filled += count;
    g_bitstream_queued += count / 2;
}

static bool queue_pcm(const std::int16_t *pcm, std::size_t samples, std::uint32_t channels,
                      std::int64_t pts, const int *map) noexcept
{
    if (!pcm || channels == 0 || channels > kMaxChannels || samples % channels != 0)
        return false;
    const std::uint32_t width = channels > 2 ? kMaxChannels : 2;
    if (g_desired_channels.load(std::memory_order_acquire) != width)
        g_desired_channels.store(width, std::memory_order_release);
    const std::size_t needed = (samples / channels) * width;
    const std::size_t capacity = std::min(g_ring.size(), kRingFrames * width);
    if (needed > capacity)
        return false;
    std::unique_lock<std::mutex> guard(g_mutex);
    while (!g_quit.load(std::memory_order_acquire) &&
           (g_out_channels != width || g_filled + needed > capacity))
    {
        if (g_desired_channels.load(std::memory_order_acquire) != width)
            return false;
        g_space_changed.wait_for(guard, std::chrono::milliseconds(100));
    }
    if (g_quit.load(std::memory_order_acquire))
        return false;
    if (g_origin_seconds < 0.0 && pts >= 0)
        g_origin_seconds = static_cast<double>(pts) / 90000.0;
    for (std::size_t i = 0; i < samples; i += channels)
    {
        for (std::uint32_t slot = 0; slot < width; ++slot)
        {
            const int source = map[slot];
            g_ring[g_write] = source < 0 ? 0 : pcm[i + static_cast<std::size_t>(source)];
            g_write = (g_write + 1) % g_ring.size();
        }
        g_filled += width;
    }
    return true;
}
#ifdef SLOPFIN_SOFTWARE_AUDIO
static bool software_pcm(void *, const software_audio::Pcm &pcm) noexcept
{
    const auto began = std::chrono::steady_clock::now();
    const bool good =
        queue_pcm(pcm.samples, pcm.frames * pcm.channels, pcm.channels, pcm.pts, pcm.map);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - began)
                             .count();
    std::lock_guard<std::mutex> guard(g_mutex);
    g_software_queue_us += static_cast<std::uint64_t>(elapsed);
    if (good)
        g_software_frames += pcm.frames;
    return good;
}
#endif

static void submit_frames(const std::uint8_t *data, std::size_t bytes, std::int64_t pts) noexcept
{
    if (!g_running.load(std::memory_order_acquire) || data == nullptr || bytes < 4)
        return;

    /*
     * A transport-stream audio packet usually carries several compressed
     * frames back to back. Each frame's header states its own length, so they
     * are walked rather than assumed to be one.
     */
    std::size_t offset = 0;
    while (offset < bytes)
    {
        std::size_t length = 0;
        std::uint32_t channels = 2;
        std::uint32_t rate = kRate;

        if (g_codec == Codec::ac3)
        {
            length = ac3_frame_bytes(data + offset, bytes - offset);
            channels = 6;
        }
        else if (g_codec == Codec::mp3)
        {
            Mp3Frame frame;
            if (!next_mp3_frame(data, bytes, offset, frame))
                break;
            offset = frame.offset;
            length = frame.bytes;
            channels = frame.channels;
            rate = frame.rate;
        }
        else
        {
            if (offset + 7 > bytes)
                break;
            const std::uint32_t adts_length = adts_frame_length(data + offset, bytes - offset);
            if (adts_length == 0)
            {
                ++offset;
                continue;
            }
            length = adts_length;
        }
        if (length == 0 || offset + length > bytes)
            break;

        g_au.address = const_cast<std::uint8_t *>(data + offset);
        g_au.length = static_cast<std::uint32_t>(length);
        g_pcm_item.address = g_pcm.data();
        g_pcm_item.length = static_cast<std::uint32_t>(g_pcm.size());

        const int result = sceAudiodecDecode(g_decoder, &g_control);
        offset += length;
        if (result < 0 || g_pcm_item.length == 0 || g_pcm_item.length > g_pcm.size())
        {
            static int s_last_error = 0;
            if (result != s_last_error)
            {
                s_last_error = result;
                slopfin::trace::mark("audio: decode failed result=" + std::to_string(result) +
                                     " codec=" + codec_name(g_codec));
            }
            continue;
        }

        if (g_codec == Codec::aac_adts)
        {
            channels = g_info.channel_count;
            rate = g_info.sampling_frequency;
        }
        else if (g_codec == Codec::ac3)
        {
            rate = g_ac3_info.sample_rate;
            // AC-3 is explicitly configured for six interleaved S16 channels.
            if (g_pcm_item.length != 1536 * 6 * sizeof(std::int16_t))
                continue;
        }
        /* One line per distinct decode outcome, so a silent path is diagnosable
           from the trace instead of by guesswork. */
        static int s_last_report = -2;
        const int report = result < 0 ? -1 : static_cast<int>(channels);
        if (report != s_last_report)
        {
            s_last_report = report;
            slopfin::trace::mark("audio: decode result=" + std::to_string(result) +
                                 " channels=" + std::to_string(channels) +
                                 " pcm=" + std::to_string(g_pcm_item.length) +
                                 " rate=" + std::to_string(rate) + " codec=" + codec_name(g_codec));
        }
        if (channels == 0 || channels > kMaxChannels || rate != kRate)
            continue;

        if (!queue_pcm(reinterpret_cast<const std::int16_t *>(g_pcm.data()),
                       g_pcm_item.length / sizeof(std::int16_t), channels, pts,
                       g_codec == Codec::ac3 ? kAc3ToStandard : kChannelMap[channels]))
            return;
    }
}

void submit(const std::uint8_t *data, std::size_t bytes, std::int64_t pts) noexcept
{
    if (!g_running.load(std::memory_order_acquire) || !data || !bytes)
        return;
    if (g_bitstream)
    {
        g_packer.feed(data, bytes, pts, queue_bitstream);
        return;
    }
    if (software_codec(g_codec))
    {
#ifdef SLOPFIN_SOFTWARE_AUDIO
        const auto began = std::chrono::steady_clock::now();
        const auto queued_before = g_software_queue_us; // single producer owns updates
        if (!g_software_decoder.feed(data, bytes, pts, software_pcm, nullptr) && !g_quit.load())
            g_decode_failed.store(true);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - began)
                                 .count();
        std::lock_guard<std::mutex> guard(g_mutex);
        const auto queue_time = g_software_queue_us - queued_before;
        g_software_work_us +=
            static_cast<std::uint64_t>(elapsed) > queue_time ? elapsed - queue_time : 0;
        g_software_bytes += bytes;
#endif
        return;
    }
    if (g_codec == Codec::ac3)
        g_ac3_frames.feed(data, bytes, pts, submit_frames);
    else if (g_codec == Codec::aac_adts)
        g_adts_frames.feed(data, bytes, pts, submit_frames);
    else
        submit_frames(data, bytes, pts);
}

void finish() noexcept
{
    if (g_bitstream && !g_quit.load())
        g_packer.flush(queue_bitstream);
#ifdef SLOPFIN_SOFTWARE_AUDIO
    if (!g_bitstream && software_codec(g_codec) && !g_quit.load() &&
        !g_software_decoder.finish(software_pcm, nullptr))
        g_decode_failed.store(true);
#endif
    // AudioOut consumes whole grains. Pad the final partial grain once, or a
    // short TrueHD tail remains queued forever after clean EOF.
    std::lock_guard<std::mutex> guard(g_mutex);
    g_audio_eof = true;
    if (g_quit.load() || g_ring.empty() || !g_filled)
        return;
    const std::size_t grain_samples = g_grain * g_out_channels;
    const std::size_t padding = (grain_samples - g_filled % grain_samples) % grain_samples;
    for (std::size_t i = 0; i < padding; ++i)
    {
        g_ring[g_write] = 0;
        g_write = (g_write + 1) % g_ring.size();
    }
    g_filled += padding;
}
bool failed() noexcept
{
    return g_decode_failed.load();
}
void video_ready() noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    g_video_ready = true;
}
bool buffering() noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_buffer_gate.waiting();
}

double played_seconds() noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return static_cast<double>(g_frames_played) / static_cast<double>(g_output_rate);
}

OutputStats output_stats() noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return {g_frames_played,
            g_underrun_frames,
            static_cast<std::uint32_t>(g_filled / std::max(1u, g_out_channels)),
            g_output_errors,
            g_buffering_frames,
            g_buffer_gate.events(),
            g_software_frames,
            g_software_work_us,
            g_software_queue_us,
            g_software_bytes};
}

void set_paused(bool paused) noexcept
{
    g_paused.store(paused, std::memory_order_release);
}

bool paused() noexcept
{
    return g_paused.load(std::memory_order_acquire);
}

double clock_seconds() noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_origin_seconds < 0.0 || g_frames_played == 0)
        return -1.0;
    return g_origin_seconds +
           static_cast<double>(g_frames_played) / static_cast<double>(g_output_rate);
}

void begin_capture() noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_capture == nullptr)
        g_capture = static_cast<std::int16_t *>(
            slopfin::bigalloc::allocate(kCaptureSamples * sizeof(std::int16_t)));
    if (g_capture == nullptr)
    {
        slopfin::trace::mark("audio: capture buffer would not allocate");
        return;
    }
    g_capture_used = 0;
    g_capturing = true;
}

void write_capture() noexcept
{
    std::uint32_t channels = 2;
    std::size_t used = 0;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_capturing = false;
        channels = g_capture_channels == 0 ? 2 : g_capture_channels;
        used = g_capture_used;
    }
    if (g_capture == nullptr || used == 0)
    {
        slopfin::trace::mark("audio: nothing captured");
        return;
    }
    if (std::FILE *file = std::fopen("/data/slopfin-audio.raw", "wb"); file != nullptr)
    {
        (void)std::fwrite(g_capture, sizeof(std::int16_t), used, file);
        (void)std::fclose(file);
        slopfin::trace::mark("audio: captured " + std::to_string(used / channels) + " frames of " +
                             std::to_string(channels));
    }
    /* The analysis runs off-console, so state the layout alongside the samples. */
    if (std::FILE *sidecar = std::fopen("/data/slopfin-audio.txt", "wb"); sidecar != nullptr)
    {
        std::fprintf(sidecar, "channels %u\nrate %u\nframes %zu\n", channels, kRate,
                     used / channels);
        (void)std::fclose(sidecar);
    }
}

double buffered_seconds() noexcept
{
    std::lock_guard<std::mutex> guard(g_mutex);
    const std::uint32_t width = g_out_channels == 0 ? 2 : g_out_channels;
    return static_cast<double>(g_filled / width) / static_cast<double>(g_output_rate);
}

bool bitstream() noexcept
{
    return g_bitstream && g_running.load(std::memory_order_acquire);
}

bool running() noexcept
{
    return g_running.load(std::memory_order_acquire);
}

} // namespace slopfin::audio
