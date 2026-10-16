/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "update.hpp"
#include "update_package.hpp"
#include "update_download.hpp"
#include "trace.hpp"
#include "update/files.h"
#include "background.hpp"
#include "http.hpp"
#include "json.hpp"
#include <atomic>
#include <mutex>
#include <memory>
#include <cstdlib>
#include <ctime>
namespace slopfin::update
{
namespace
{
std::mutex mutex;
Status state;
Release release;
std::atomic<bool> restart{false};
std::string base()
{
#ifdef SLOPFIN_HOST
    if (const char *dir = std::getenv("SLOPFIN_DATA"))
        return std::string(dir) + "/update";
    return ".slopfin/update";
#else
    return "/data/slopfin/update";
#endif
}
std::string small_file(const std::string &path, std::size_t max = 65536)
{
    struct stat st = {};
    int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return {};
    if (fstat(fd, &st) < 0 || st.st_size < 0 || static_cast<std::size_t>(st.st_size) > max)
    {
        close(fd);
        return {};
    }
    std::string text(static_cast<std::size_t>(st.st_size), '\0');
    std::size_t have = 0;
    while (have < text.size())
    {
        auto n = read(fd, text.data() + have, text.size() - have);
        if (n <= 0)
            break;
        have += static_cast<std::size_t>(n);
    }
    close(fd);
    if (have != text.size())
        return {};
    return text;
}
void initialize()
{
    static std::once_flag once;
    std::call_once(once,
                   []
                   {
#ifdef SLOPFIN_HOST
                       const auto param = json::parse(small_file("sce_sys/param.json"));
#else
            const auto param = json::parse(small_file("/app0/sce_sys/param.json"));
#endif
                       std::lock_guard<std::mutex> lock(mutex);
                       state.current =
                           param ? std::string(param->str("contentVersion")) : "Unknown";
                       state.message = small_file(base() + "/result", 512);
                       if (state.message == "ready")
                           state.message =
                               "The previous update did not finish. Check again to retry.";
                   });
}
bool busy(Phase p)
{
    return p == Phase::checking || p == Phase::downloading || p == Phase::installing;
}
void failure(std::string message)
{
    trace::mark("update: " + message);
    std::lock_guard<std::mutex> lock(mutex);
    state.phase = Phase::error;
    state.message = std::move(message);
}
void run_update(std::function<void()> job) noexcept
{
    if (!background::run(std::move(job), background::Lane::updates))
        failure("The update worker could not start. Restart SlopFin and try again.");
}
[[maybe_unused]] bool save(const std::string &path, std::string_view text)
{
    if (sf_update_dirs(path.c_str()) < 0)
        return false;
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return false;
    std::size_t have = 0;
    while (have < text.size())
    {
        auto n = write(fd, text.data() + have, text.size() - have);
        if (n <= 0)
            break;
        have += static_cast<std::size_t>(n);
    }
    const bool ok = have == text.size() && fsync(fd) == 0;
    const int closed = close(fd);
    return ok && closed == 0;
}
std::unique_ptr<http::WebRequest> open_download(std::string url)
{
    for (unsigned i = 0; i < 5; ++i)
    {
        if (!trusted_download(url))
            return {};
        auto request = std::make_unique<http::WebRequest>();
        if (!request->open(url, "GET", {}, {}, {}, true))
            return {};
        if (request->status() == 200)
            return request;
        if (request->status() != 301 && request->status() != 302 && request->status() != 303 &&
            request->status() != 307 && request->status() != 308)
            return {};
        url = request->location();
    }
    return {};
}
} // namespace
Status status() noexcept
{
    initialize();
    std::lock_guard<std::mutex> lock(mutex);
    return state;
}
void check() noexcept
{
    initialize();
    std::string current;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (busy(state.phase))
            return;
        if (!version_number(state.current))
        {
            state.phase = Phase::error;
            state.message = "This build has no valid version. Install a release folder first.";
            return;
        }
        state.phase = Phase::checking;
        state.message = "Checking GitHub releases...";
        state.received = state.total = 0;
        current = state.current;
    }
    run_update(
        [current]
        {
            http::WebRequest request;
            if (!request.open(
                    "https://api.github.com/repos/BeLikeBrett/SlopFin/releases?per_page=8", "GET",
                    {"Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28"}, {},
                    {}))
            {
                failure("Could not reach GitHub. Check your connection and try again.");
                return;
            }
            if (request.status() != 200)
            {
                failure(request.status() == 403 || request.status() == 429
                            ? "GitHub is limiting requests. Try again later."
                            : "GitHub could not return the releases. Try again later.");
                return;
            }
            std::string feed;
            char buf[16384];
            long n;
            while ((n = request.read(buf, sizeof(buf))) > 0)
            {
                if (feed.size() + static_cast<std::size_t>(n) > 1024u * 1024u)
                {
                    failure("GitHub returned an unexpected release list.");
                    return;
                }
                feed.append(buf, static_cast<std::size_t>(n));
            }
            if (n < 0 || !json::parse(feed))
            {
                failure("The release check was interrupted. Try again.");
                return;
            }
            const auto found = select_release(feed, current);
            std::lock_guard<std::mutex> lock(mutex);
            release = found;
            state.version = found.version;
            state.total = found.size;
            state.preview = found.preview;
            state.phase = found.version.empty() ? Phase::current : Phase::available;
            state.message = found.version.empty() ? "You have the latest compatible release."
                                                  : "A new SlopFin release is available.";
        });
}
void download() noexcept
{
    Release selected;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (busy(state.phase) || release.version.empty())
            return;
        selected = release;
        state.phase = Phase::downloading;
        state.received = 0;
        state.total = selected.size;
        state.message = "Downloading from GitHub...";
    }
    run_update(
        [selected]
        {
            const std::string folder = base(), path = folder + "/release.zip";
            unlink((folder + "/staged/manifest").c_str());
            if (sf_update_dirs(path.c_str()) < 0)
            {
                const auto code = errno;
                trace::mark("update: prepare storage errno=" + std::to_string(code));
                failure(code == ENOSPC
                            ? "There is not enough free space to create the update folder."
                            : "The PS5 denied access to the update folder. Reinstall the current "
                              "folder build and retry.");
                return;
            }
            DownloadResult downloaded;
            for (unsigned attempt = 0; attempt < 3; ++attempt)
            {
                auto request = open_download(selected.url);
                if (!request)
                {
                    downloaded.failure = DownloadFailure::network;
                    continue;
                }
                downloaded = receive_archive(
                    path, selected.size,
                    [&](void *bytes, std::size_t size) { return request->read(bytes, size); },
                    [&](std::size_t received)
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        state.received = received;
                    });
                trace::mark("update: download attempt=" + std::to_string(attempt + 1) +
                            " bytes=" + std::to_string(downloaded.received) + "/" +
                            std::to_string(selected.size) +
                            " failure=" + std::to_string(static_cast<int>(downloaded.failure)) +
                            " errno=" + std::to_string(downloaded.system_error));
                if (downloaded.failure != DownloadFailure::network &&
                    downloaded.failure != DownloadFailure::truncated)
                    break;
                std::lock_guard<std::mutex> lock(mutex);
                state.received = 0;
                state.message = "Connection interrupted. Retrying the download...";
            }
            if (downloaded.failure != DownloadFailure::none)
            {
                switch (downloaded.failure)
                {
                case DownloadFailure::network:
                case DownloadFailure::truncated:
                    failure("The GitHub download was interrupted. Your current app is unchanged. "
                            "Check your connection and try again.");
                    break;
                case DownloadFailure::oversized:
                    failure(
                        "GitHub returned a file with the wrong size. Check again for a release.");
                    break;
                default:
                    failure(downloaded.system_error == ENOSPC
                                ? "There is not enough free space to save the update. Free some "
                                  "space and retry."
                            : downloaded.system_error == EACCES || downloaded.system_error == EPERM
                                ? "The PS5 denied access to update storage. Reinstall the current "
                                  "folder build and retry."
                                : "The PS5 could not finish saving the update. Your app is "
                                  "unchanged; see Diagnostics.");
                    break;
                }
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                state.message = "Verifying and preparing the update...";
            }
            std::string error;
            if (!stage_archive(path, folder + "/staged", selected, error))
            {
                failure(error);
                return;
            }
            std::lock_guard<std::mutex> lock(mutex);
            state.phase = Phase::ready;
            state.message = "Download verified. Your sign-in and preferences will be kept.";
        });
}
void install() noexcept
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (state.phase != Phase::ready)
            return;
#ifdef SLOPFIN_HOST
        state.message = "Install and restart is available on the PS5 folder build.";
        return;
#else
        state.phase = Phase::installing;
        state.message = "Preparing to close SlopFin and install...";
#endif
    }
#ifndef SLOPFIN_HOST
    run_update(
        []
        {
            const std::string folder = base();
            /* Require the running folder to match before replacing anything. */
            char executable[65], param[65];
            if (sf_update_hash_file("/app0/eboot.bin", executable) < 0 ||
                sf_update_hash_file("/app0/sce_sys/param.json", param) < 0 ||
                !save(folder + "/source", std::string(executable) + "\n" + param + "\n") ||
                !save(folder + "/pid", std::to_string(getpid())))
            {
                failure("Could not identify this app folder. Your app was not changed.");
                return;
            }
            const auto payload = small_file("/app0/assets/slopfin-update.bin", 1024 * 1024);
            if (payload.empty())
            {
                failure("The update helper is missing. Install the full release folder.");
                return;
            }
            /* A helper writes 'ready' only after validating files and saving a backup. */
            unlink((folder + "/result").c_str());
            if (!http::send_bytes("127.0.0.1", 9021, payload))
            {
                failure("The payload loader (elfldr, port 9021) did not answer. Start it and try "
                        "again.");
                return;
            }
            for (unsigned i = 0; i < 150; ++i)
            {
                const auto result = small_file(folder + "/result", 512);
                if (result == "ready")
                {
                    restart.store(true);
                    return;
                }
                if (!result.empty())
                {
                    failure(result);
                    return;
                }
                timespec wait{0, 200000000};
                nanosleep(&wait, nullptr);
            }
            failure("The update helper did not answer. Close SlopFin and check the payload loader "
                    "before retrying.");
        });
#endif
}
bool restart_requested() noexcept
{
    return restart.load();
}
} // namespace slopfin::update
