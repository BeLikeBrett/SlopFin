/*
 * SlopFin - HDMI audio bitstream experiment (development only).
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console's disc player sends Dolby and DTS to HDMI untouched. The calls it
 * would use are exported by libSceAudioOut as the "Ex" family; their argument
 * shapes here come from disassembling the console's own library (2026-09-15):
 *
 *   sceAudioOutExConfigureOutput(0, _, mode, device, _)
 *       builds an output-mode request -- mode 0 AC-3 5.1, 1 AAC 5.1, 2 DTS 5.1,
 *       3 E-AC-3 7.1 at 192 kHz, 4 and 10 eight-channel 192 kHz types,
 *       5..8 PCM, 255 back to default -- and passes it to the AV control
 *       service (sceAvControlInit, GetMonitorInfo, ChangeOutputMode).
 *   sceAudioOutExOpen(user, mode) opens port type 6 at that mode's rate.
 *
 * From a payload process both were refused (0x809b00ff, 0x80260011), even with
 * ShellCore credentials; the AV control service appears to want a real
 * application, which is why this runs inside SlopFin.
 */

#include "bitstream_probe.hpp"

#include "bigalloc.hpp"
#include "images.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C"
{
    int sceUserServiceGetForegroundUser(int *user);
    int sceAudioOutInit();
    int sceAudioOutOutput(int handle, const void *samples);
    int sceAudioOutClose(int handle);
    int sceAudioOutExConfigureOutput(int zero, long unused, int mode, int device, long unused2);
    int sceAudioOutExOpen(int user, int mode);
    int sceAudioOutExClose(int handle);
    int sceAudioOutExPtOpen(int user, int variant, int index, std::uint32_t grain,
                            std::uint32_t rate, std::uint32_t format);
    int sceAudioOutExPtClose(int handle);
    int sceAudioOutOpenEx(int user, int type, int index, int flag, std::uint32_t grain,
                          std::uint32_t rate, std::uint32_t format);
    int sceAudioOutSysConfigureOutput(int bus, int flags, int mode, int device, long unused);
    int sceAudioOutSysOpen(int user, int index);
    int sceAudioOutSysClose(int handle);
    int sceAudioOutExGetOutputInfo(int zero, long unused, void *buffer, int size);
    int scePthreadCreate(void **thread, const void *attr, void *(*entry)(void *), void *argument,
                         const char *name);
    int scePthreadSetaffinity(void *thread, std::uint64_t mask);
    int scePthreadDetach(void *thread);
    int sceKernelGettimeofday(void *timeval);
}

namespace bitstream_probe
{
namespace
{
struct Settings
{
    int mode = 0;
    int device = 255;
    int user = 255; /* 0 = foreground user */
    int port = 0;   /* 0 = ExOpen, 1 = ExPtOpen, 2 = OpenEx as a type-6 port with `format`,
                       3 = the Sys pair the disc player uses (SysConfigureOutput + SysOpen) */
    int seconds = 15;
    int words = 2;  /* 16-bit words per frame: 2, or 8 for high-bit-rate streams */
    int format = 1; /* port sample format for ports 1 and 2: 1 S16 stereo, 2 S16 8ch */
    /* For eight-word frames, which input word each output channel carries:
       "01324567" swaps the third and fourth. The console's 8-channel order is
       not HDMI's, and a high-bit-rate stream split across channels only
       survives if the words land in HDMI's order. */
    char map[16] = "01234567";
    char file[32] = "ac3"; /* /data/slopfin-bitstream-<file>.spdif */
};

Settings g_settings;
bool g_running = false;

void say(const char *format, ...) noexcept
{
    char line[512];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    const int fd = open("/data/slopfin-bitstream.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0 && length > 0)
    {
        (void)write(fd, line, static_cast<std::size_t>(length));
        (void)close(fd);
    }
}

void output_info(const char *when) noexcept
{
    std::uint8_t info[24] = {};
    const int rc = sceAudioOutExGetOutputInfo(0, 0, info, sizeof(info));
    say("output info %-8s 0x%08x:", when, static_cast<unsigned>(rc));
    for (std::uint8_t byte : info)
        say(" %02x", byte);
    say("\n");
}

void *run(void *) noexcept
{
    const Settings s = g_settings;
    (void)unlink("/data/slopfin-bitstream.txt");
    say("mode %d device %d user %d port %d seconds %d words %d file %s\n", s.mode, s.device, s.user,
        s.port, s.seconds, s.words, s.file);

    int user = 255;
    if (s.user == 0)
        (void)sceUserServiceGetForegroundUser(&user);
    say("init 0x%08x, user 0x%x\n", static_cast<unsigned>(sceAudioOutInit()),
        static_cast<unsigned>(user));

    char path[96];
    (void)std::snprintf(path, sizeof(path), "/data/slopfin-bitstream-%s.spdif", s.file);
    const int fd = open(path, O_RDONLY);
    struct stat st = {};
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0)
    {
        say("no %s\n", path);
        g_running = false;
        return nullptr;
    }
    const auto size = static_cast<std::size_t>(st.st_size);
    auto *data = static_cast<std::uint8_t *>(slopfin::bigalloc::allocate(size));
    std::size_t have = 0;
    while (data != nullptr && have < size)
    {
        const ssize_t got = read(fd, data + have, size - have);
        if (got <= 0)
            break;
        have += static_cast<std::size_t>(got);
    }
    (void)close(fd);
    say("read %zu bytes\n", have);
    if (data != nullptr && s.words == 8 && std::strcmp(s.map, "01234567") != 0 &&
        std::strlen(s.map) == 8)
    {
        int order[8];
        for (int k = 0; k < 8; ++k)
            order[k] = std::clamp(s.map[k] - '0', 0, 7);
        for (std::size_t at = 0; at + 16 <= have; at += 16)
        {
            std::uint8_t frame[16];
            std::memcpy(frame, data + at, 16);
            for (int k = 0; k < 8; ++k)
                std::memcpy(data + at + 2 * k, frame + 2 * order[k], 2);
        }
        say("channel map %s applied\n", s.map);
    }

    /* Rates and grains per mode, from the library's own ExOpen table -- or,
       for the Sys pair, SysOpen's: index 5 (MAT, TrueHD) and 7 run at 768 kHz,
       3 and 6 at 192 kHz, the rest at 48 kHz. */
    const bool high_rate =
        s.port == 3 ? (s.mode == 3 || s.mode == 6) : (s.mode == 3 || s.mode == 4 || s.mode == 10);
    const bool hbr = s.port == 3 && (s.mode == 5 || s.mode == 7);
    const std::uint32_t grain = high_rate || hbr ? 1024 : 256;
    const std::uint32_t rate = hbr ? 768000 : high_rate ? 192000 : 48000;

    output_info("before");
    if (s.port == 3)
        say("SysConfigureOutput(1, 0, mode %d, device %d) = 0x%08x\n", s.mode, s.device,
            static_cast<unsigned>(sceAudioOutSysConfigureOutput(1, 0, s.mode, s.device, 0)));
    else
        say("ExConfigureOutput(mode %d, device %d) = 0x%08x\n", s.mode, s.device,
            static_cast<unsigned>(sceAudioOutExConfigureOutput(0, 0, s.mode, s.device, 0)));
    output_info("config");

    /* ExOpen's stereo 192 kHz port carries E-AC-3's 4x stream but cannot
     * carry TrueHD's 8x stream. OpenEx lets the caller choose the format. */
    int handle = -1;
    if (s.port == 0)
        handle = sceAudioOutExOpen(user, s.mode);
    else if (s.port == 3)
        handle = sceAudioOutSysOpen(0xff, s.mode);
    else if (s.port == 1)
        handle = sceAudioOutExPtOpen(user, 1, 0, grain, rate, static_cast<std::uint32_t>(s.format));
    else
        handle =
            sceAudioOutOpenEx(user, 6, 0, 0, grain, rate, static_cast<std::uint32_t>(s.format));
    say("port %d format %d = 0x%08x\n", s.port, s.format, static_cast<unsigned>(handle));
    output_info("opened");

    if (handle >= 0 && data != nullptr)
    {
        const std::size_t chunk = 2u * static_cast<std::size_t>(s.words) * grain;
        const std::size_t limit = static_cast<std::size_t>(s.seconds) * rate / grain;
        int errors = 0;
        int first_error = 0;
        std::size_t chunks = 0;
        struct
        {
            std::int64_t seconds;
            std::int64_t microseconds;
        } began{}, ended{};
        (void)sceKernelGettimeofday(&began);
        for (std::size_t at = 0; chunks < limit; ++chunks, at += chunk)
        {
            if (at + chunk > have)
                at = 0;
            const int rc = sceAudioOutOutput(handle, data + at);
            if (rc < 0 && errors++ == 0)
                first_error = rc;
            if (errors > 50)
                break;
            if (chunks == limit / 2)
                output_info("playing");
        }
        (void)sceKernelGettimeofday(&ended);
        const double took = static_cast<double>(ended.seconds - began.seconds) +
                            static_cast<double>(ended.microseconds - began.microseconds) / 1e6;
        say("output %zu chunks of %zu bytes in %.2f s (expected %.2f s), %d errors (first "
            "0x%08x)\n",
            chunks, chunk, took, static_cast<double>(chunks) * grain / rate, errors,
            static_cast<unsigned>(first_error));
        say("close = 0x%08x\n", static_cast<unsigned>(s.port == 0   ? sceAudioOutExClose(handle)
                                                      : s.port == 1 ? sceAudioOutExPtClose(handle)
                                                      : s.port == 3 ? sceAudioOutSysClose(handle)
                                                                    : sceAudioOutClose(handle)));
    }
    say("restore = 0x%08x\n",
        static_cast<unsigned>(s.port == 3 ? sceAudioOutSysConfigureOutput(1, 0, 255, 255, 0)
                                          : sceAudioOutExConfigureOutput(0, 0, 255, 255, 0)));
    output_info("after");
    say("done\n");
    if (data != nullptr)
        slopfin::bigalloc::release(data);
    g_running = false;
    return nullptr;
}
} // namespace

void start(const char *arguments) noexcept
{
    if (g_running)
        return;
    Settings s;
    if (arguments != nullptr)
        (void)std::sscanf(arguments, "%d %d %d %d %d %d %31s %d %15s", &s.mode, &s.device, &s.user,
                          &s.port, &s.seconds, &s.words, s.file, &s.format, s.map);
    g_settings = s;
    g_running = true;
    void *thread = nullptr;
    if (scePthreadCreate(&thread, nullptr, run, nullptr, "slopfin-bitstream") != 0)
    {
        g_running = false;
        return;
    }
    if (const std::uint64_t spare = slopfin::images::spare_cores(); spare != 0)
        (void)scePthreadSetaffinity(thread, spare);
    (void)scePthreadDetach(thread);
}
} // namespace bitstream_probe
