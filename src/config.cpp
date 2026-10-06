/*
 * SlopFin - persisted settings.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.hpp"

#include "json.hpp"

#include <cstdio>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#ifdef SLOPFIN_HOST
#include <filesystem>
#endif
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" int sceKernelGetProcessTime();
#ifndef SLOPFIN_HOST
extern "C" int sceKernelRename(const char *, const char *);
#endif

namespace
{
/*
 * The console keeps settings in /data; a workstation has no such place, so the
 * host build follows the XDG convention. Both hold the same file, which is why
 * a token copied off the console works in the preview unchanged.
 */
#ifdef SLOPFIN_HOST
const std::string &host_directory() noexcept
{
    static const std::string directory = []
    {
        if (const char *explicit_dir = std::getenv("SLOPFIN_DATA"); explicit_dir != nullptr)
            return std::string{explicit_dir};
        if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr)
            return std::string{xdg} + "/slopfin";
        if (const char *home = std::getenv("HOME"); home != nullptr)
            return std::string{home} + "/.config/slopfin";
        return std::string{".slopfin"};
    }();
    return directory;
}
const char *directory() noexcept
{
    return host_directory().c_str();
}
const std::string &config_path() noexcept
{
    static const std::string path = host_directory() + "/config.json";
    return path;
}
#else
constexpr const char *directory() noexcept
{
    return "/data/slopfin";
}
const std::string &config_path() noexcept
{
    static const std::string path = "/data/slopfin/config.json";
    return path;
}
#endif

slopfin::config::Settings g_settings;
bool g_loaded = false;

extern "C" int mkdir(const char *path, unsigned int mode);

/* A stable per-console identity; Jellyfin lists it as the device. */
std::string invent_device_id() noexcept
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string id = "ps5-";
    auto seed = static_cast<std::uint32_t>(sceKernelGetProcessTime());
    for (int i = 0; i < 12; ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        id.push_back(kHex[(seed >> 24) & 0x0fu]);
    }
    return id;
}
} // namespace

namespace slopfin::config
{

Settings &current() noexcept
{
    if (!g_loaded)
        load();
    return g_settings;
}

void load() noexcept
{
    g_loaded = true;
    g_settings = Settings{};
    const int file = ::open(config_path().c_str(), O_RDONLY);
    const bool missing = file < 0 && errno == ENOENT;
    bool parsed = false;
    bool migrated = false;
    if (file >= 0)
    {
        constexpr std::size_t kMaxConfigBytes = 1024u * 1024u;
        std::string text;
        char chunk[4096];
        bool complete = false;
        while (text.size() <= kMaxConfigBytes)
        {
            const auto bytes = ::read(file, chunk, sizeof(chunk));
            if (bytes == 0)
            {
                complete = true;
                break;
            }
            if (bytes < 0)
            {
                if (errno == EINTR)
                    continue;
                break;
            }
            text.append(chunk, static_cast<std::size_t>(bytes));
        }
        (void)::close(file);
        const json::ValuePtr root = complete ? json::parse(text) : nullptr;
        if (root != nullptr && root->type == json::Type::object)
        {
            parsed = true;
            const std::string_view host = root->str("host");
            if (!host.empty())
                g_settings.host = std::string{host};
            const double port = root->num("port", 0);
            if (port >= 1 && port <= 65535)
                g_settings.port = static_cast<int>(port);
            g_settings.report_server = std::string{root->str("reportServer")};
            g_settings.token = std::string{root->str("token")};
            g_settings.user_id = std::string{root->str("userId")};
            g_settings.device_id = std::string{root->str("deviceId")};
            g_settings.trigger_feedback = root->num("triggerFeedback", 1.0) != 0.0;
            g_settings.audio_passthrough = root->num("audioPassthrough", 1.0) != 0.0;
            g_settings.software_audio = root->num("softwareAudio", 0.0) != 0.0;
            g_settings.dv_hdr10_base = root->num("dvHdr10Base", 0.0) != 0.0;
#ifndef SLOPFIN_HOST
            // Carry marker-based choices forward once; saved settings take precedence.
            if (root->find("audioPassthrough") == nullptr &&
                access("/data/slopfin-no-bitstream", F_OK) == 0)
            {
                g_settings.audio_passthrough = false;
                migrated = true;
            }
            if (root->find("softwareAudio") == nullptr &&
                access("/data/slopfin-software-audio", F_OK) == 0)
            {
                g_settings.software_audio = true;
                migrated = true;
            }
            if (root->find("dvHdr10Base") == nullptr && access("/data/slopfin-dv-hdr10", F_OK) == 0)
            {
                g_settings.dv_hdr10_base = true;
                migrated = true;
            }
#endif
            if (const json::Value *look = root->find("subtitleLook"); look != nullptr)
            {
                auto &c = g_settings.subtitle_look;
                c.size = static_cast<int>(look->num("size", c.size));
                c.font = static_cast<int>(look->num("font", c.font));
                c.bold = static_cast<int>(look->num("bold", c.bold));
                c.colour = static_cast<int>(look->num("colour", c.colour));
                c.edge = static_cast<int>(look->num("edge", c.edge));
                c.background = static_cast<int>(look->num("background", c.background));
                c.box = static_cast<int>(look->num("box", c.box));
                c.spacing = static_cast<int>(look->num("spacing", c.spacing));
                c.position = static_cast<int>(look->num("position", c.position));
                c.picture = static_cast<int>(look->num("picture", c.picture));
            }
            if (const json::Value *play = root->find("autoplay"); play != nullptr)
            {
                auto &c = g_settings.autoplay_look;
                c.enabled = play->num("enabled", c.enabled ? 1.0 : 0.0) != 0.0;
                c.ask_after = static_cast<int>(play->num("askAfter", c.ask_after));
                c.ask_idle_minutes =
                    static_cast<int>(play->num("askIdleMinutes", c.ask_idle_minutes));
            }
            if (const json::Value *series = root->find("autoplaySeries"); series != nullptr)
                for (const auto &[id, value] : series->fields)
                    if (value != nullptr && !id.empty())
                        g_settings.autoplay_series_overrides[id] = value->number_or(0.0) != 0.0;
            if (const json::Value *offsets = root->find("subtitleOffsets"); offsets != nullptr)
                for (const auto &[item, value] : offsets->fields)
                    if (value != nullptr && value->type == json::Type::number && !item.empty())
                        g_settings.subtitle_offsets[item] = value->number;
        }
    }
    if (g_settings.device_id.empty())
    {
        g_settings.device_id = invent_device_id();
        if (missing || parsed)
            save();
    }
    else if (migrated)
        save();
}

void save() noexcept
{
#ifdef SLOPFIN_HOST
    std::error_code error;
    std::filesystem::create_directories(directory(), error);
    if (error)
        return;
#else
    (void)mkdir(directory(), 0700);
#endif
    std::string text = "{\n";
    text += "  \"host\": \"" + json::escape(g_settings.host) + "\",\n";
    text += "  \"port\": " + std::to_string(g_settings.port) + ",\n";
    text += "  \"reportServer\": \"" + json::escape(g_settings.report_server) + "\",\n";
    text += "  \"token\": \"" + json::escape(g_settings.token) + "\",\n";
    text += "  \"userId\": \"" + json::escape(g_settings.user_id) + "\",\n";
    text += "  \"deviceId\": \"" + json::escape(g_settings.device_id) + "\",\n";
    text +=
        "  \"triggerFeedback\": " + std::string{g_settings.trigger_feedback ? "1" : "0"} + ",\n";
    text +=
        "  \"audioPassthrough\": " + std::string{g_settings.audio_passthrough ? "1" : "0"} + ",\n";
    text += "  \"softwareAudio\": " + std::string{g_settings.software_audio ? "1" : "0"} + ",\n";
    text += "  \"dvHdr10Base\": " + std::string{g_settings.dv_hdr10_base ? "1" : "0"} + ",\n";
    {
        const auto &c = g_settings.subtitle_look;
        text +=
            "  \"subtitleLook\": {\"size\": " + std::to_string(c.size) +
            ", \"font\": " + std::to_string(c.font) + ", \"bold\": " + std::to_string(c.bold) +
            ", \"colour\": " + std::to_string(c.colour) + ", \"edge\": " + std::to_string(c.edge) +
            ", \"background\": " + std::to_string(c.background) +
            ", \"box\": " + std::to_string(c.box) + ", \"spacing\": " + std::to_string(c.spacing) +
            ", \"position\": " + std::to_string(c.position) +
            ", \"picture\": " + std::to_string(c.picture) + "},\n";
    }
    {
        const auto &c = g_settings.autoplay_look;
        text += "  \"autoplay\": {\"enabled\": " + std::string{c.enabled ? "1" : "0"} +
                ", \"askAfter\": " + std::to_string(c.ask_after) +
                ", \"askIdleMinutes\": " + std::to_string(c.ask_idle_minutes) + "},\n";
    }
    text += "  \"autoplaySeries\": {";
    {
        bool first_series = true;
        for (const auto &[series, enabled] : g_settings.autoplay_series_overrides)
        {
            text += std::string{first_series ? "\n" : ",\n"} + "    \"" + json::escape(series) +
                    "\": " + (enabled ? "1" : "0");
            first_series = false;
        }
        text += first_series ? "},\n" : "\n  },\n";
    }
    text += "  \"subtitleOffsets\": {";
    bool first = true;
    for (const auto &[item, offset] : g_settings.subtitle_offsets)
    {
        char number[32];
        std::snprintf(number, sizeof(number), "%.1f", offset);
        text += std::string{first ? "\n" : ",\n"} + "    \"" + json::escape(item) + "\": " + number;
        first = false;
    }
    text += first ? "}\n" : "\n  }\n";
    text += "}\n";
    const std::string temporary = config_path() + ".tmp";
    // Removing a stale temporary file also restores restrictive permissions.
    (void)::unlink(temporary.c_str());
    const int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (file < 0)
        return;
    std::size_t offset = 0;
    while (offset < text.size())
    {
        const auto bytes = ::write(file, text.data() + offset, text.size() - offset);
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes <= 0)
            break;
        offset += static_cast<std::size_t>(bytes);
    }
    const bool complete = ::close(file) == 0 && offset == text.size();
#ifdef SLOPFIN_HOST
    const bool replaced = complete && std::rename(temporary.c_str(), config_path().c_str()) == 0;
#else
    const bool replaced =
        complete && sceKernelRename(temporary.c_str(), config_path().c_str()) == 0;
#endif
    if (!replaced)
        (void)::unlink(temporary.c_str());
}

double subtitle_offset(const std::string &item_id) noexcept
{
    const auto found = g_settings.subtitle_offsets.find(item_id);
    return found == g_settings.subtitle_offsets.end() ? 0.0 : found->second;
}

void set_subtitle_offset(const std::string &item_id, double offset) noexcept
{
    if (item_id.empty())
        return;
    if (offset > -0.05 && offset < 0.05)
        g_settings.subtitle_offsets.erase(item_id);
    else
        g_settings.subtitle_offsets[item_id] = offset;
}

bool autoplay_enabled(const std::string &series_id) noexcept
{
    if (!series_id.empty())
        if (const auto found = g_settings.autoplay_series_overrides.find(series_id);
            found != g_settings.autoplay_series_overrides.end())
            return found->second;
    return g_settings.autoplay_look.enabled;
}

void set_autoplay_enabled(const std::string &series_id, bool enabled) noexcept
{
    if (series_id.empty())
        return;
    g_settings.autoplay_series_overrides[series_id] = enabled;
}

} // namespace slopfin::config
