/*
 * SlopFin - display and sound capability capture.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Argument shapes come from disassembling the console's own libSceAudioOut
 * (tools/bitstream/README.md). ExGetMonitorInfo and ExGetOutputInfo returned
 * the TV's audio capabilities from inside SlopFin on 2026-09-15; from a
 * payload they are refused. Only calls already proven safe from this process
 * are made: the Sys family's argument lists are unconfirmed, and a wrong guess
 * could write somewhere it should not.
 */

#include "av_capture.hpp"

#include "bigalloc.hpp"
#include "gfx.hpp"
#include "trace.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

extern "C"
{
    int sceAudioOutInit();
    int sceAudioOutExGetMonitorInfo(int zero, long unused, void *buffer, int size);
    int sceAudioOutExGetOutputInfo(int zero, long unused, void *buffer, int size);
    int sceRegMgrGetInt(long key, int *value);
}

namespace
{
using namespace slopfin;

/* Generous, so a call whose structure is larger than expected still lands in
   memory of our own; the report says how far each one wrote. */
constexpr std::size_t kBuffer = 64u * 1024u;

class Report
{
  public:
    void say(const char *format, ...) noexcept __attribute__((format(printf, 2, 3)))
    {
        char line[512];
        va_list args;
        va_start(args, format);
        const int length = std::vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        if (length > 0)
            text_.append(line, static_cast<std::size_t>(
                                   std::min<int>(length, static_cast<int>(sizeof(line)) - 1)));
    }

    /* The call's result, how far into the buffer it wrote, and those bytes. */
    void dump(const char *call, int result, const std::uint8_t *bytes, std::size_t size) noexcept
    {
        std::size_t written = 0;
        for (std::size_t i = 0; i < size; ++i)
            if (bytes[i] != 0)
                written = i + 1;
        say("%s = 0x%08x, %zu bytes written\n", call, static_cast<unsigned>(result), written);
        hex(bytes, std::min<std::size_t>(written, 4096));
        say("\n");
    }

    void hex(const std::uint8_t *bytes, std::size_t size) noexcept
    {
        for (std::size_t i = 0; i < size; i += 16)
        {
            say("  %04zx ", i);
            for (std::size_t j = i; j < i + 16 && j < size; ++j)
                say("%02x ", bytes[j]);
            say("\n");
        }
    }

    void append_file(const char *path, const char *heading) noexcept
    {
        say("== %s (%s)\n", heading, path);
        const int file = ::open(path, O_RDONLY);
        if (file < 0)
        {
            say("  not written\n\n");
            return;
        }
        char chunk[4096];
        long got = 0;
        while ((got = ::read(file, chunk, sizeof(chunk))) > 0)
            text_.append(chunk, static_cast<std::size_t>(got));
        (void)::close(file);
        say("\n");
    }

    [[nodiscard]] const std::string &text() const noexcept
    {
        return text_;
    }

  private:
    std::string text_;
};

/* The display's own name, from the EDID's monitor-name descriptor, so each
   report says which screen it came from without anyone having to remember. */
std::string edid_name(const std::uint8_t *edid, std::size_t size) noexcept
{
    if (size < 128)
        return {};
    for (std::size_t at = 54; at + 18 <= 126; at += 18)
    {
        const std::uint8_t *block = edid + at;
        if (block[0] != 0 || block[1] != 0 || block[2] != 0 || block[3] != 0xfc)
            continue;
        std::string name;
        for (std::size_t i = 5; i < 18 && block[i] != 0x0a; ++i)
            if (block[i] >= 0x20 && block[i] < 0x7f)
                name.push_back(static_cast<char>(block[i]));
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        return name;
    }
    return {};
}

std::size_t read_file(const char *path, std::uint8_t *into, std::size_t capacity) noexcept
{
    const int file = ::open(path, O_RDONLY);
    if (file < 0)
        return 0;
    std::size_t total = 0;
    long got = 0;
    while (total < capacity && (got = ::read(file, into + total, capacity - total)) > 0)
        total += static_cast<std::size_t>(got);
    (void)::close(file);
    return total;
}

/* The first unused /data/slopfin-av-report-N.txt, so a report per display
   accumulates instead of each overwriting the last. */
std::string next_path() noexcept
{
    for (int n = 1; n < 100; ++n)
    {
        const std::string path = "/data/slopfin-av-report-" + std::to_string(n) + ".txt";
        if (::access(path.c_str(), F_OK) != 0)
            return path;
    }
    return "/data/slopfin-av-report-99.txt";
}
} // namespace

namespace slopfin::av_capture
{

std::string run() noexcept
{
    auto *buffer = static_cast<std::uint8_t *>(bigalloc::allocate(kBuffer));
    if (buffer == nullptr)
        return "Not enough memory to run the check.";
    trace::mark("av capture: begin");

    /* The video side, including the raw EDID when the console hands it over.
       It writes its own files; they are folded into the report below. */
    (void)::unlink("/data/slopfin-edid.bin");
    gfx::report_display_info();

    Report report;
    report.say("SlopFin display & sound report\n");
    report.say("build %s %s\n", __DATE__, __TIME__);

    const std::size_t edid_size = read_file("/data/slopfin-edid.bin", buffer, kBuffer);
    const std::string name = edid_name(buffer, edid_size);
    report.say("display %s\n\n", name.empty() ? "(no name reported)" : name.c_str());

    /* The PS5's own Sound settings, as the system keeps them. */
    static const struct
    {
        const char *name;
        long key;
    } kKeys[] = {
        {"mode", 184614912},
        {"headphone_out", 184745984},
        {"connector_type", 184942592},
        {"codec", 185008128},
        {"sound_format", 185073664},
        {"config_options", 185139200},
        {"speaker_setting", 185270272},
        {"speaker_layout", 185335808},
        {"speaker_type", 185401344},
        {"virtual_surround_hdmi", 185466880},
    };
    report.say("== PS5 sound settings (registry)\n");
    for (const auto &key : kKeys)
    {
        int value = -1;
        const int result = sceRegMgrGetInt(key.key, &value);
        report.say("  AUDIOOUT_%-22s 0x%08x %d\n", key.name, static_cast<unsigned>(result), value);
    }
    report.say("\nsceAudioOutInit = 0x%08x\n\n", static_cast<unsigned>(sceAudioOutInit()));

    report.say("== Audio output capabilities\n");
    std::memset(buffer, 0, kBuffer);
    report.dump("sceAudioOutExGetMonitorInfo(0, 0, buf, 0x400)",
                sceAudioOutExGetMonitorInfo(0, 0, buffer, 0x400), buffer, 0x400);
    std::memset(buffer, 0, kBuffer);
    report.dump("sceAudioOutExGetOutputInfo(0, 0, buf, 0x18)",
                sceAudioOutExGetOutputInfo(0, 0, buffer, 0x18), buffer, 0x18);

    report.append_file("/data/slopfin-display.txt", "Video output");
    std::memset(buffer, 0, kBuffer);
    const std::size_t edid_again = read_file("/data/slopfin-edid.bin", buffer, kBuffer);
    report.say("== EDID, %zu bytes\n", edid_again);
    report.hex(buffer, edid_again);
    bigalloc::release(buffer);

    const std::string path = next_path();
    const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file < 0)
        return "Could not write the report to /data.";
    const std::string &text = report.text();
    const long wrote = ::write(file, text.data(), text.size());
    (void)::close(file);
    if (wrote != static_cast<long>(text.size()))
        return "The report was cut short; /data may be full.";
    trace::mark("av capture: wrote " + path);
    const std::string file_name = path.substr(path.rfind('/') + 1);
    return (name.empty() ? std::string{"Saved "} : "Saved for " + name + ": ") + file_name;
}

} // namespace slopfin::av_capture
