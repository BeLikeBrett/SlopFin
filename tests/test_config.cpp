/*
 * SlopFin - settings persistence regression tests.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "config.hpp"
#include "playback_options.hpp"
#include "diagnostics.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int sceKernelGetProcessTime()
{
    return 42;
}

int main()
{
    char pattern[] = "/tmp/slopfin-config-test-XXXXXX";
    const char *directory = mkdtemp(pattern);
    assert(directory != nullptr);
    setenv("SLOPFIN_DATA", directory, 1);
    const auto path = std::filesystem::path(directory) / "config.json";
    slopfin::config::load();
    auto &settings = slopfin::config::current();
    assert(!settings.device_id.empty() && !settings.signed_in());
    assert(!slopfin::diagnostics::destination(settings.report_server).valid());
    // A new install sends surround sound to the TV when it lists the format.
    assert(settings.audio_passthrough);
    settings.audio_passthrough = false;
    settings.host = "https://media.example.com/jellyfin";
    settings.port = 443;
    settings.token = "test-token";
    settings.user_id = "test-user";
    settings.report_server = "https://reports.example.com:8443";
    settings.seerr_host = "https://requests.example.com/base";
    settings.seerr_port = 443;
    settings.seerr_api_key = "request-key";
    settings.seerr_auth = 2;
    settings.autoplay_series_overrides["show"] = false;
    settings.subtitle_offsets["episode"] = 1.5;
    assert(settings.subtitle_look.picture_size == 1 &&
           settings.subtitle_look.picture_position == 0);
    settings.subtitle_look.picture_size = 3;
    settings.subtitle_look.picture_position = 2;
    settings.navigation_order = {"library:a", "home", "discover"};
    settings.navigation_hidden = {"home", "discover"};
    settings.seerr_watched_requests = {"123", "456"};
    slopfin::config::save();
    struct stat metadata = {};
    assert(stat(path.c_str(), &metadata) == 0 && (metadata.st_mode & 0777) == 0600);
    slopfin::config::load();
    assert(!settings.audio_passthrough);
    assert(settings.subtitle_look.picture_size == 3 &&
           settings.subtitle_look.picture_position == 2);
    assert(settings.navigation_order ==
           (std::vector<std::string>{"library:a", "home", "discover"}));
    assert(settings.navigation_hidden == (std::vector<std::string>{"home", "discover"}));
    assert(settings.seerr_watched_requests == (std::vector<std::string>{"123", "456"}));
    assert(settings.seerr_host == "https://requests.example.com/base" &&
           settings.seerr_api_key == "request-key" && settings.seerr_auth == 2);
    slopfin::jellyfin::PlaybackRequest request;
    request.item_id = "episode";
    request.start_seconds = 12.5;
    auto effective = slopfin::player::apply_preferences(request, settings, true);
    assert(!effective.allow_bitstream && effective.allow_software_audio &&
           effective.allow_dv_hdr10_base);
    assert(effective.item_id == "episode" && effective.start_seconds == 12.5);
    assert(!slopfin::player::apply_preferences(request, settings, false).allow_software_audio);
    assert(settings.signed_in() && settings.host == "https://media.example.com/jellyfin");
    assert(settings.report_server == "https://reports.example.com:8443");
    assert(!slopfin::config::autoplay_enabled("show"));
    assert(slopfin::config::subtitle_offset("episode") == 1.5);
    assert(!std::filesystem::exists(path.string() + ".tmp"));

    // A failed replacement must leave the saved login intact.
    std::filesystem::create_directory(path.string() + ".tmp");
    settings.token = "replacement";
    slopfin::config::save();
    slopfin::config::load();
    assert(settings.token == "test-token");
    std::filesystem::remove(path.string() + ".tmp");

    {
        std::ofstream file(path);
        file << "{invalid";
    }
    slopfin::config::load();
    assert(!settings.signed_in());
    std::ifstream file(path);
    assert(std::string(std::istreambuf_iterator<char>(file), {}) == "{invalid");
    file.close();

    {
        std::ofstream file(path);
        file
            << R"({"deviceId":"fixture","navigationOrder":["home","home",1,null,"settings"],"navigationHidden":["settings","discover",false,"discover"]})";
    }
    slopfin::config::load();
    assert(settings.navigation_order == std::vector<std::string>{"home"});
    assert(settings.navigation_hidden == std::vector<std::string>{"discover"});
    assert(settings.seerr_watched_requests.empty());

    // Older files do not require a report receiver or the newer preferences.
    {
        std::ofstream legacy(path);
        legacy << R"({"deviceId":"legacy","port":70000})";
    }
    slopfin::config::load();
    assert(settings.device_id == "legacy" && settings.port == 8096);
    assert(settings.audio_passthrough);
    effective = slopfin::player::apply_preferences(request, settings, true);
    assert(effective.allow_bitstream && effective.allow_software_audio &&
           effective.allow_dv_hdr10_base);

    // Files from 1.16 and earlier may hold the retired softwareAudio and
    // dvHdr10Base switches; they are ignored, and decoding stays on.
    {
        std::ofstream old_choice(path);
        old_choice << R"({"deviceId":"old","softwareAudio":0,"dvHdr10Base":0})";
    }
    slopfin::config::load();
    effective = slopfin::player::apply_preferences(request, settings, true);
    assert(effective.allow_software_audio && effective.allow_dv_hdr10_base);
    slopfin::config::save();
    {
        std::ifstream saved(path);
        const std::string text(std::istreambuf_iterator<char>(saved), {});
        assert(text.find("softwareAudio") == std::string::npos &&
               text.find("dvHdr10Base") == std::string::npos);
    }
    assert(settings.report_server.empty() && settings.autoplay_series_overrides.empty());
    const auto receiver = slopfin::diagnostics::destination("http://reports.example.com:8103/base");
    assert(receiver.valid() && receiver.port == 8103 &&
           receiver.host == "http://reports.example.com/base");
    std::filesystem::remove_all(directory);
    std::cout << "config: all checks passed\n";
}
