/*
 * SlopFin - settings persistence regression tests.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "config.hpp"
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
    settings.host = "https://media.example.com/jellyfin";
    settings.port = 443;
    settings.token = "test-token";
    settings.user_id = "test-user";
    settings.report_server = "https://reports.example.com:8443";
    settings.autoplay_series_overrides["show"] = false;
    settings.subtitle_offsets["episode"] = 1.5;
    slopfin::config::save();
    struct stat metadata = {};
    assert(stat(path.c_str(), &metadata) == 0 && (metadata.st_mode & 0777) == 0600);
    slopfin::config::load();
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

    // Older files do not require a report receiver or the newer preferences.
    {
        std::ofstream legacy(path);
        legacy << R"({"deviceId":"legacy","port":70000})";
    }
    slopfin::config::load();
    assert(settings.device_id == "legacy" && settings.port == 8096);
    assert(settings.report_server.empty() && settings.autoplay_series_overrides.empty());
    const auto receiver = slopfin::diagnostics::destination("http://reports.example.com:8103/base");
    assert(receiver.valid() && receiver.port == 8103 &&
           receiver.host == "http://reports.example.com/base");
    std::filesystem::remove_all(directory);
    std::cout << "config: all checks passed\n";
}
