/*
 * SlopFin - audio capability probe.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The PS5 settings screen offers Dolby Digital and DTS, but that is the system
 * audio daemon re-encoding the final mix for an HDMI receiver; it says nothing
 * about what this process may call. So rather than infer: open every output
 * format, initialise every decoder codec id, and push real Dolby and DTS frames
 * through whatever will take them. The report is measurement, not inference.
 */

#include "audiocaps.hpp"
#include "ac3.hpp"
#include "audio.hpp"
#include "software_audio.hpp"
#include <algorithm>

#include "bigalloc.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <unistd.h>
#include <fcntl.h>

extern "C"
{
    int sceSysmoduleLoadModule(std::uint16_t module_id);
    int sceKernelUsleep(std::uint32_t);
    int sceKernelLoadStartModule(const char *, std::size_t, const void *, unsigned, const void *,
                                 int *);
    int sceKernelDlsym(int, const char *, void **);
    int sceKernelStopUnloadModule(int, std::size_t, const void *, unsigned, const void *, int *);
    int sceAudioOutInit();
    int sceAudioOutOpen(int user_id, int type, int index, std::uint32_t length,
                        std::uint32_t frequency, std::uint32_t format);
    int sceAudioOutClose(int handle);
    int sceAudioOutOutput(int handle, const void *samples);
    int sceAudiodecInitLibrary(std::uint32_t codec_type);
    int sceAudiodecTermLibrary(std::uint32_t codec_type);
    int sceAudiodecCreateDecoder(void *control, std::uint32_t codec_type);
    int sceAudiodecDeleteDecoder(int handle);
    int sceAudiodecDecode(int handle, void *control);
}

namespace
{
constexpr std::uint16_t kAudioModuleId = 0x0088;

struct AuInfo
{
    std::uint32_t size;
    void *address;
    std::uint32_t length;
};

struct PcmItem
{
    std::uint32_t size;
    void *address;
    std::uint32_t length;
};

struct Control
{
    void *param;
    void *stream_info;
    AuInfo *au_info;
    PcmItem *pcm_item;
};

struct Format
{
    std::uint32_t id;
    const char *name;
    unsigned channels;
};

constexpr Format kFormats[] = {
    {0, "S16 mono", 1},     {1, "S16 stereo", 2}, {2, "S16 8ch", 8},     {3, "float mono", 1},
    {4, "float stereo", 2}, {5, "float 8ch", 8},  {6, "S16 8ch std", 8}, {7, "float 8ch std", 8},
};

struct Sample
{
    const char *path;
    const char *label;
};

constexpr Sample kSamples[] = {
    {"/data/slopfin-codec/aac_51.bin", "AAC 5.1 (ADTS)"},
    {"/data/slopfin-codec/aac_20.bin", "AAC stereo (ADTS)"},
    {"/data/slopfin-codec/ac3_51.bin", "AC-3 5.1 (Dolby Digital)"},
    {"/data/slopfin-codec/eac3_51.bin", "E-AC-3 5.1 (Dolby Digital Plus)"},
    {"/data/slopfin-codec/dts_51.bin", "DTS 5.1"},
    {"/data/slopfin-codec/mp3_20.bin", "MP3 stereo"},
    {"/data/slopfin-codec/flac_20.bin", "FLAC stereo"},
};

/* Parameter and stream-info layouts differ per codec, so over-provision and
 * zero them: only the leading size field is set, which is what gets validated. */
constexpr std::size_t kBlobBytes = 512;
constexpr std::size_t kPcmBytes = 64 * 1024;
constexpr std::size_t kFeedBytes = 32 * 1024;

std::FILE *g_report = nullptr;

/* Firmware 8.20 validates an 88-byte AC-3 parameter and 48-byte info block.
 * Keep this probe separate from playback until its channel routing is measured. */
void probe_ac3() noexcept
{
    slopfin::audio::Ac3Param param;
    alignas(16) std::uint32_t info[12] = {};
    info[0] = sizeof(info);
    auto *feed = static_cast<std::uint8_t *>(slopfin::bigalloc::allocate(kFeedBytes));
    void *output = slopfin::bigalloc::allocate(kPcmBytes);
    std::FILE *input = std::fopen("/data/slopfin-codec/ac3_51.bin", "rb");
    if (!feed || !output || !input)
    {
        std::fprintf(g_report, "AC-3 sample or allocation unavailable\n");
        if (input)
            std::fclose(input);
        if (feed)
            slopfin::bigalloc::release(feed);
        if (output)
            slopfin::bigalloc::release(output);
        return;
    }
    const std::size_t bytes = std::fread(feed, 1, kFeedBytes, input);
    std::fclose(input);
    AuInfo au{sizeof(AuInfo), feed, 0};
    PcmItem pcm{sizeof(PcmItem), output, kPcmBytes};
    Control control{&param, info, &au, &pcm};
    const int init = sceAudiodecInitLibrary(4);
    std::fprintf(g_report, "AC-3 init=0x%x\n", init);
    std::fflush(g_report);
    const int decoder = init >= 0 ? sceAudiodecCreateDecoder(&control, 4) : -1;
    std::fprintf(g_report, "AC-3 create=0x%x info.result=0x%x\n", decoder, info[5]);
    std::fflush(g_report);
    if (decoder >= 0)
    {
        std::FILE *capture = std::fopen("/data/slopfin-ac3-probe.raw", "wb");
        std::size_t offset = 0;
        for (unsigned frame = 0; frame < 12 && offset + 7 <= bytes; ++frame)
        {
            /* This controlled fixture is 48 kHz AC-3; other rates need the
             * full frame-size table in the production parser. */
            static constexpr unsigned rates[] = {32,  40,  48,  56,  64,  80,  96,  112, 128, 160,
                                                 192, 224, 256, 320, 384, 448, 512, 576, 640};
            const auto *header = feed + offset;
            const unsigned code = header[4] & 63;
            if (header[0] != 0x0b || header[1] != 0x77 || (header[4] >> 6) != 0 || code >= 38)
                break;
            const unsigned length = rates[code / 2] * 4;
            if (offset + length > bytes)
                break;
            au.address = feed + offset;
            au.length = length;
            pcm.length = kPcmBytes;
            std::memset(output, 0, kPcmBytes);
            const int result = sceAudiodecDecode(decoder, &control);
            std::fprintf(g_report, "frame=%u result=0x%x consumed=%u pcm=%u info:", frame, result,
                         au.length, pcm.length);
            for (auto field : info)
                std::fprintf(g_report, " %x", field);
            std::fprintf(g_report, "\n");
            std::fflush(g_report);
            if (result >= 0 && pcm.length <= kPcmBytes && capture)
                std::fwrite(output, 1, pcm.length, capture);
            offset += length;
        }
        if (capture)
            std::fclose(capture);
        std::fprintf(g_report, "delete=0x%x\n", sceAudiodecDeleteDecoder(decoder));
    }
    if (init >= 0)
        std::fprintf(g_report, "term=0x%x\n", sceAudiodecTermLibrary(4));
    slopfin::bigalloc::release(feed);
    slopfin::bigalloc::release(output);
}

/* The AAC info block the decoder fills in; its size field must be set first. */
constexpr std::uint32_t kAacInfoBytes = 20;

/* An ADTS frame carries its own length, and the decoder wants exactly one. */
std::uint32_t adts_frame_length(const std::uint8_t *data, std::size_t available) noexcept
{
    if (available < 7 || data[0] != 0xff || (data[1] & 0xf0) != 0xf0)
        return 0;
    const std::uint32_t length = (static_cast<std::uint32_t>(data[3] & 0x03) << 11) |
                                 (static_cast<std::uint32_t>(data[4]) << 3) |
                                 (static_cast<std::uint32_t>(data[5]) >> 5);
    return length <= available ? length : 0;
}

/*
 * CreateDecoder validates the parameter contents, not just the leading size, so
 * a zeroed struct is always rejected. Every known layout starts {size,
 * word_size, ...}, so sweep the size and the word size and fill the tail with
 * values that are plausible for a media stream. The AAC entry, whose working
 * values are known from the player, is the control: if it fails here the probe
 * is wrong rather than the console.
 */
int create_by_search(std::uint32_t codec, void *param, void *stream_info, AuInfo *au, PcmItem *pcm,
                     std::uint32_t &size_out, std::uint32_t &word_out, int &tail_out) noexcept
{
    Control control{param, stream_info, au, pcm};
    /* Tail fills: none, stereo 48 kHz, 5.1 48 kHz, 8ch 48 kHz. */
    static const std::uint32_t kTails[][4] = {
        {0, 0, 0, 0},
        {1, 4, 2, 1},
        {1, 4, 6, 0},
        {1, 4, 8, 0},
    };
    for (std::uint32_t size = 8; size <= 64; size += 4)
    {
        for (std::uint32_t word = 1; word <= 3; ++word)
        {
            for (int tail = 0; tail < 4; ++tail)
            {
                std::memset(param, 0, kBlobBytes);
                auto *fields = static_cast<std::uint32_t *>(param);
                fields[0] = size;
                fields[1] = word;
                for (int i = 0; i < 4; ++i)
                    fields[2 + i] = kTails[tail][i];
                const int handle = sceAudiodecCreateDecoder(&control, codec);
                if (handle >= 0)
                {
                    size_out = size;
                    word_out = word;
                    tail_out = tail;
                    return handle;
                }
            }
        }
    }
    return -1;
}
} // namespace

namespace audiocaps
{
void probe() noexcept
{
    g_report = std::fopen("/data/slopfin-audio-caps.txt", "wb");
    if (g_report == nullptr)
        return;

    // Known fixtures through the production decoder, map, ring and output.
    // This branch never opens speculative codec IDs or firmware modules.
    if (std::FILE *marker = std::fopen("/data/slopfin-audio-fixture", "rb"))
    {
        char name[64]{};
        (void)std::fscanf(marker, "%63s", name);
        std::fclose(marker);
        unlink("/data/slopfin-audio-fixture");
        if (slopfin::audio::running())
        {
            std::fprintf(g_report, "Stop playback before an audio fixture; the active sink is not "
                                   "owned by this probe.\n");
            std::fclose(g_report);
            g_report = nullptr;
            return;
        }
        const bool output_trial = std::strncmp(name, "out:", 4) == 0;
        if (output_trial)
            std::memmove(name, name + 4, std::strlen(name + 4) + 1);
        const bool software =
            std::strcmp(name, "truehd_51") == 0 || std::strcmp(name, "dts_51") == 0 ||
            std::strcmp(name, "eac3_51") == 0 || std::strcmp(name, "eac3_20") == 0;
        if (software && !output_trial)
        {
            (void)slopfin::software_audio::probe(name, g_report);
            std::fclose(g_report);
            g_report = nullptr;
            return;
        }
        const bool ac3 = std::strcmp(name, "ac3_51") == 0 || std::strcmp(name, "ac3_20") == 0;
        const bool aac = std::strcmp(name, "aac_51") == 0 || std::strcmp(name, "aac_20") == 0;
        constexpr std::size_t limit = 2u * 1024u * 1024u;
        auto *data = static_cast<std::uint8_t *>(slopfin::bigalloc::allocate(limit));
        char path[128]{};
        std::snprintf(path, sizeof(path), "/data/slopfin-codec/%s.bin", name);
        const int fd = ac3 || aac || software ? ::open(path, O_RDONLY) : -1;
        std::size_t used = 0;
        if (fd >= 0 && data)
            while (used < limit)
            {
                const auto got = ::read(fd, data + used, limit - used);
                if (got <= 0)
                    break;
                used += static_cast<std::size_t>(got);
            }
        if (fd >= 0)
            ::close(fd);
        const auto codec = std::strcmp(name, "truehd_51") == 0 ? slopfin::audio::Codec::truehd
                           : std::strcmp(name, "dts_51") == 0  ? slopfin::audio::Codec::dts
                           : software                          ? slopfin::audio::Codec::eac3
                           : ac3                               ? slopfin::audio::Codec::ac3
                                                               : slopfin::audio::Codec::aac_adts;
        if (used && used < limit && slopfin::audio::start(codec))
        {
            slopfin::audio::begin_capture();
            for (std::size_t at = 0; at < used; at += 188)
                slopfin::audio::submit(data + at, std::min<std::size_t>(188, used - at),
                                       at == 0 ? 0 : -1);
            slopfin::audio::finish();
            std::fprintf(g_report, "Decoder failed: %d\n", slopfin::audio::failed());
            for (int i = 0; i < 250 && slopfin::audio::buffered_seconds() > 0.0; ++i)
                (void)sceKernelUsleep(20000);
            slopfin::audio::write_capture();
            const auto stats = slopfin::audio::output_stats();
            std::fprintf(g_report, "%s: played=%llu underrun=%llu errors=%u buffered=%u frames\n",
                         name, static_cast<unsigned long long>(stats.played_frames),
                         static_cast<unsigned long long>(stats.underrun_frames), stats.errors,
                         stats.buffered_frames);
            slopfin::audio::stop();
        }
        else
            std::fprintf(g_report, "Fixture unavailable or native audio could not start: %s\n",
                         name);
        slopfin::bigalloc::release(data);
        std::fclose(g_report);
        g_report = nullptr;
        return;
    }

    if (std::FILE *target = std::fopen("/data/slopfin-truehd-probe", "rb"))
    {
        std::fclose(target);
        unlink("/data/slopfin-truehd-probe");
        std::fprintf(g_report,
                     "TrueHD module reachability only; no decode or passthrough claim.\n");
        for (const char *path : {"/system/priv/lib/libSceAudiodecCpuTrhd.sprx",
                                 "/system/common/lib/libSceAudiodecCpu.sprx"})
        {
            int result = 0;
            const int handle = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, &result);
            std::fprintf(g_report, "%s load=0x%x start=0x%x\n", path, handle, result);
            std::fflush(g_report);
            if (handle < 0)
                continue;
            for (const char *name :
                 {"sceAudiodecCpuInternalQueryMemSize", "sceAudiodecCpuInternalInitDecoder",
                  "sceAudiodecCpuInternalDecode"})
            {
                void *symbol = nullptr;
                const int found = sceKernelDlsym(handle, name, &symbol);
                std::fprintf(g_report, "%s lookup=0x%x available=%d\n", name, found,
                             symbol != nullptr);
            }
            std::fprintf(g_report, "unload=0x%x\n",
                         sceKernelStopUnloadModule(handle, 0, nullptr, 0, nullptr, &result));
            std::fflush(g_report);
        }
        std::fclose(g_report);
        g_report = nullptr;
        return;
    }

    const int module_result = sceSysmoduleLoadModule(kAudioModuleId);
    const int init_result = sceAudioOutInit();
    std::fprintf(g_report, "SlopFin audio capability probe\n");
    std::fprintf(g_report, "sceSysmoduleLoadModule(0x88) = 0x%x\n", module_result);
    std::fprintf(g_report, "sceAudioOutInit()            = 0x%x\n\n", init_result);

    if (std::FILE *target = std::fopen("/data/slopfin-ac3-probe", "rb"))
    {
        std::fclose(target);
        unlink("/data/slopfin-ac3-probe");
        probe_ac3();
        std::fclose(g_report);
        return;
    }

    /*
     * Output formats. Opening is necessary but not sufficient, so also push one
     * grain of silence through each: a port that accepts 8-channel writes is a
     * port that can carry surround.
     */
    std::fprintf(g_report, "-- sceAudioOutOpen / Output, grain 256 --\n");
    void *silence = slopfin::bigalloc::allocate(256 * 8 * sizeof(float));
    for (const std::uint32_t rate : {48000u, 192000u})
    {
        std::fprintf(g_report, "  at %u Hz:\n", rate);
        for (const Format &format : kFormats)
        {
            const int handle = sceAudioOutOpen(0xff, 0, 0, 256, rate, format.id);
            if (handle <= 0)
            {
                std::fprintf(g_report, "    %-14s (%u ch) open refused 0x%x\n", format.name,
                             format.channels, handle);
                continue;
            }
            int written = -1;
            if (silence != nullptr)
            {
                std::memset(silence, 0, 256 * 8 * sizeof(float));
                written = sceAudioOutOutput(handle, silence);
            }
            std::fprintf(g_report, "    %-14s (%u ch) OPEN, output %d\n", format.name,
                         format.channels, written);
            (void)sceAudioOutClose(handle);
        }
    }
    if (silence != nullptr)
        slopfin::bigalloc::release(silence);

    /* Decoder codec ids, with the parameter size each one accepts. */
    void *param = slopfin::bigalloc::allocate(kBlobBytes);
    void *stream_info = slopfin::bigalloc::allocate(kBlobBytes);
    void *pcm_buffer = slopfin::bigalloc::allocate(kPcmBytes);
    void *feed = slopfin::bigalloc::allocate(kFeedBytes);
    if (param == nullptr || stream_info == nullptr || pcm_buffer == nullptr || feed == nullptr)
    {
        std::fprintf(g_report, "\nout of memory for the decoder probe\n");
        (void)std::fclose(g_report);
        return;
    }

    std::fprintf(g_report, "\n-- sceAudiodec codec ids --\n");
    for (std::uint32_t codec = 0; codec < 32; ++codec)
    {
        const int library = sceAudiodecInitLibrary(codec);
        if (library < 0)
        {
            /* A distinct error means the id is known but unavailable here. */
            if (library != static_cast<int>(0x807f0001))
                std::fprintf(g_report, "  id %-3u known but unavailable 0x%x\n", codec, library);
            continue;
        }

        AuInfo au{sizeof(AuInfo), feed, 0};
        PcmItem pcm{sizeof(PcmItem), pcm_buffer, kPcmBytes};
        std::uint32_t accepted = 0;
        std::uint32_t word = 0;
        int tail = -1;
        const int handle =
            create_by_search(codec, param, stream_info, &au, &pcm, accepted, word, tail);
        if (handle < 0)
        {
            std::fprintf(g_report, "  id %-3u library OK, no parameter shape accepted\n", codec);
            (void)sceAudiodecTermLibrary(codec);
            continue;
        }
        std::fprintf(g_report, "  id %-3u library OK, param size %u word %u tail %d, handle %d\n",
                     codec, accepted, word, tail, handle);

        /* Feed each real stream and see which one this codec understands. */
        for (const Sample &sample : kSamples)
        {
            std::FILE *file = std::fopen(sample.path, "rb");
            if (file == nullptr)
                continue;
            const std::size_t got = std::fread(feed, 1, kFeedBytes, file);
            (void)std::fclose(file);
            if (got == 0)
                continue;

            Control control{param, stream_info, &au, &pcm};
            au.size = sizeof(AuInfo);
            au.address = feed;
            au.length = static_cast<std::uint32_t>(got);
            pcm.size = sizeof(PcmItem);
            pcm.address = pcm_buffer;
            pcm.length = kPcmBytes;
            std::memset(pcm_buffer, 0, kPcmBytes);
            std::memset(stream_info, 0, kBlobBytes);
            *static_cast<std::uint32_t *>(stream_info) = kAacInfoBytes;
            if (const std::uint32_t frame =
                    adts_frame_length(static_cast<const std::uint8_t *>(feed), got);
                frame != 0)
                au.length = frame;

            const int decoded = sceAudiodecDecode(handle, &control);
            if (decoded >= 0 && pcm.length > 0)
            {
                /* Report the info block too: it carries rate and channel count. */
                const std::uint32_t *info = static_cast<const std::uint32_t *>(stream_info);
                std::fprintf(g_report,
                             "        %-32s DECODED consumed %u -> %u PCM bytes"
                             " | info %u %u %u %u\n",
                             sample.label, au.length, pcm.length, info[0], info[1], info[2],
                             info[3]);
            }
            else
            {
                std::fprintf(g_report, "        %-32s no (0x%x, %u bytes out)\n", sample.label,
                             decoded, pcm.length);
            }
        }

        (void)sceAudiodecDeleteDecoder(handle);
        (void)sceAudiodecTermLibrary(codec);
    }

    /*
     * The question that actually decides surround support: the proven AAC
     * decoder is known to work at max_channels 2, so walk max_channels upward
     * against a real 5.1 stream and see how many channels come back.
     */
    std::fprintf(g_report, "\n-- AAC decoder, max_channels sweep against a real 5.1 stream --\n");
    for (std::uint32_t channels : {2u, 6u, 8u})
    {
        if (sceAudiodecInitLibrary(3) < 0)
            continue;
        AuInfo au{sizeof(AuInfo), feed, 0};
        PcmItem pcm{sizeof(PcmItem), pcm_buffer, kPcmBytes};
        std::memset(param, 0, kBlobBytes);
        auto *fields = static_cast<std::uint32_t *>(param);
        /* {size, word_size, config_number, sampling_frequency_index, max_channels, he_aac} */
        fields[0] = 24;
        fields[1] = 1;
        fields[2] = 1;
        fields[3] = 4;
        fields[4] = channels;
        fields[5] = 1;
        Control control{param, stream_info, &au, &pcm};
        const int handle = sceAudiodecCreateDecoder(&control, 3);
        if (handle < 0)
        {
            std::fprintf(g_report, "  max_channels %u: create refused 0x%x\n", channels, handle);
            (void)sceAudiodecTermLibrary(3);
            continue;
        }
        std::FILE *file = std::fopen("/data/slopfin-codec/aac_51.bin", "rb");
        if (file != nullptr)
        {
            const std::size_t got = std::fread(feed, 1, kFeedBytes, file);
            (void)std::fclose(file);
            au.size = sizeof(AuInfo);
            au.address = feed;
            const std::uint32_t frame =
                adts_frame_length(static_cast<const std::uint8_t *>(feed), got);
            au.length = frame != 0 ? frame : static_cast<std::uint32_t>(got);
            pcm.size = sizeof(PcmItem);
            pcm.address = pcm_buffer;
            pcm.length = kPcmBytes;
            std::memset(stream_info, 0, kBlobBytes);
            *static_cast<std::uint32_t *>(stream_info) = kAacInfoBytes;
            const int decoded = sceAudiodecDecode(handle, &control);
            const std::uint32_t *info = static_cast<const std::uint32_t *>(stream_info);
            std::fprintf(g_report,
                         "  max_channels %u: frame %u, decode 0x%x consumed %u -> %u PCM bytes"
                         " | info size %u rate %u channels %u heaac %u\n",
                         channels, frame, decoded, au.length, pcm.length, info[0], info[1], info[2],
                         info[3]);
        }
        (void)sceAudiodecDeleteDecoder(handle);
        (void)sceAudiodecTermLibrary(3);
    }

    slopfin::bigalloc::release(param);
    slopfin::bigalloc::release(stream_info);
    slopfin::bigalloc::release(pcm_buffer);
    slopfin::bigalloc::release(feed);
    std::fprintf(g_report, "\nprobe complete\n");
    (void)std::fclose(g_report);
    g_report = nullptr;
}
} // namespace audiocaps
