/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "update.hpp"
#include "update_package.hpp"
#include "update_download.hpp"
#include "trace.hpp"
#include "update/files.h"
#include "update/storage_request.h"
#include "update/pkg_request.h"
#include "app_files.hpp"
#include "background.hpp"
#include "http.hpp"
#include "json.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
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
std::string small_descriptor(int fd, std::size_t max)
{
    struct stat st = {};
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
std::string small_file(const std::string &path, std::size_t max = 65536)
{
    return small_descriptor(open(path.c_str(), O_RDONLY | O_NOFOLLOW), max);
}
[[maybe_unused]] std::string small_application_file(std::string_view path, std::size_t max = 65536)
{
    return app_files::contents(path, max);
}
[[maybe_unused]] int application_hash(std::string_view path, char hex[65])
{
    return app_files::hash(path, hex) ? 0 : -1;
}
#ifndef SLOPFIN_HOST
extern "C" int sceKernelGetAppInfo(int pid, void *info);
#endif
/* This process's title ID, as the system registered it (PPSA plus five digits). */
[[maybe_unused]] bool running_title(char out[10])
{
#ifdef SLOPFIN_HOST
    (void)out;
    return false;
#else
    /* 0x60 bytes; the title ID starts at offset 16, as measured on hardware. */
    alignas(8) unsigned char info[0x60] = {};
    if (sceKernelGetAppInfo(getpid(), info) != 0)
        return false;
    std::memcpy(out, info + 16, 9);
    out[9] = 0;
    return sf_pkg_title_valid(out);
#endif
}
/* The version this build was made as, from param.json at build time. */
std::string compiled_version()
{
#ifdef SLOPFIN_CONTENT_VERSION
#define SLOPFIN_TEXT_(x) #x
#define SLOPFIN_TEXT(x) SLOPFIN_TEXT_(x)
    const std::string digits = SLOPFIN_TEXT(SLOPFIN_CONTENT_VERSION);
    if (digits.size() == 8)
        return digits.substr(0, 2) + "." + digits.substr(2, 3) + "." + digits.substr(5, 3);
#endif
    return {};
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
            const auto param = json::parse(small_application_file("sce_sys/param.json"));
#endif
                       std::lock_guard<std::mutex> lock(mutex);
                       const std::string compiled = compiled_version();
                       state.current = param && !param->str("contentVersion").empty()
                                           ? std::string(param->str("contentVersion"))
                                       : !compiled.empty() ? compiled
                                                           : "Unknown";
                       state.message = small_file(base() + "/result", 512);
                       if (state.message == "ready")
                           state.message =
                               "The previous update did not finish. Check again to retry.";
                       /* Said once, on the launch after the update, not forever. */
                       if (!state.message.empty())
                           (void)unlink((base() + "/result").c_str());
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
    state.phase = state.phase == Phase::installing ? Phase::ready : Phase::error;
    state.message = std::move(message);
}
void run_update(std::function<void()> job) noexcept
{
    if (!background::run(std::move(job), background::Lane::updates))
        failure("The update worker could not start. Restart SlopFin and try again.");
}
[[maybe_unused]] bool save(const std::string &path, std::string_view text)
{
    return sf_update_write(path.c_str(), text.data(), text.size()) == 0;
}
std::string storage_error(int code)
{
    switch (code)
    {
    case ENOSPC:
        return "There is not enough free space to create the update folder.";
    case ENOENT:
        return "App data storage is unavailable to this launch. Install the folder release "
               "manually.";
    case EACCES:
    case EPERM:
        return "The PS5 denied access to update storage. Check the app data folder permissions.";
    default:
        return "Could not prepare update storage (error " + std::to_string(code) + ").";
    }
}
[[maybe_unused]] bool shared_storage_ready(const std::string &directory)
{
    const std::string probe = directory + "/.access-" + std::to_string(getpid());
    if (sf_update_dirs(probe.c_str()) < 0 || (unlink(probe.c_str()) < 0 && errno != ENOENT))
        return false;
    const int fd = open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0)
        return false;
    const bool saved = fsync(fd) == 0;
    const int closed = close(fd);
    unlink(probe.c_str());
    if (!saved || closed < 0)
        return false;
    const int config = open((directory + "/config.json").c_str(), O_RDONLY | O_NOFOLLOW);
    if (config < 0)
        return errno == ENOENT;
    struct stat metadata = {};
    const bool readable = fstat(config, &metadata) == 0 && S_ISREG(metadata.st_mode);
    close(config);
    return readable;
}
bool recover_storage()
{
#ifdef SLOPFIN_HOST
    return false;
#else
    /* Each early return says why: without shared data this goes to the
       kernel log, the only diagnostics an installed package has here. */
    const auto fail = [](const char *step)
    {
        trace::mark(std::string("storage: recovery stopped at ") + step + ", errno " +
                    std::to_string(errno));
        return false;
    };
    const auto payload = small_application_file("assets/slopfin-storage.bin", 256 * 1024);
    if (payload.empty())
        return fail("reading the helper");
    if ((unlink("/download0/slopfin-storage-result") < 0 && errno != ENOENT) ||
        (unlink("/download0/slopfin-storage-request") < 0 && errno != ENOENT))
        return fail("clearing download0");
    /* The descriptor remains readable when the helper changes this process's root. */
    const int result =
        open("/download0/slopfin-storage-result", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
    if (result < 0)
        return fail("creating the result in download0");
    const int request =
        open("/download0/slopfin-storage-request", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    const std::string identity = std::string(SF_STORAGE_REQUEST_PREFIX) + std::to_string(getpid()) +
                                 "\n" + std::to_string(app_files::application.descriptor()) + "\n" +
                                 std::to_string(result) + "\n";
    sf_storage_request parsed = {};
    const bool prepared =
        fchmod(result, 0666) == 0 && request >= 0 &&
        sf_storage_parse_request(identity.data(), identity.size(), &parsed) == 0 &&
        write(request, identity.data(), identity.size()) == static_cast<ssize_t>(identity.size()) &&
        fchmod(request, 0644) == 0;
    if (request >= 0)
        close(request);
    bool ready = false;
    if (!prepared)
        (void)fail("writing the request");
    else if (!http::send_bytes("127.0.0.1", 9021, payload))
        (void)fail("sending the helper to the ELF loader");
    else
        for (unsigned i = 0; i < 50; ++i)
        {
            char response[16] = {0};
            lseek(result, 0, SEEK_SET);
            const auto count = read(result, response, sizeof(response) - 1);
            if (count > 0)
            {
                ready = std::string_view(response, static_cast<std::size_t>(count)) == "ready";
                break;
            }
            timespec wait{0, 100000000};
            nanosleep(&wait, nullptr);
        }
    close(result);
    trace::mark(ready ? "storage: recovery helper reported ready"
                      : "storage: recovery helper did not report ready");
    return ready;
#endif
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
bool prepare_storage() noexcept
{
#ifdef SLOPFIN_HOST
    return true;
#else
    if (!app_files::capture())
        return false;
    if (shared_storage_ready("/data/slopfin"))
        return true;
    trace::mark("storage: /data/slopfin is not reachable, errno " + std::to_string(errno));
    return recover_storage() && shared_storage_ready("/data/slopfin");
#endif
}
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
                failure(request.certificate_errors()
                            ? "GitHub's certificate could not be verified. Install the latest "
                              "complete SlopFin folder."
                            : "Could not reach GitHub. Check your connection and try again.");
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
            const auto found = select_release(feed, current, app_files::installed_package());
            std::lock_guard<std::mutex> lock(mutex);
            release = found;
            state.version = found.version;
            state.notes = found.notes;
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
            const bool package = app_files::installed_package();
            const std::string folder = base(),
                              path = folder + (package ? "/release.pkg" : "/release.zip");
            unlink((folder + "/staged/manifest").c_str());
            if (sf_update_dirs(path.c_str()) < 0)
            {
                const auto code = errno;
                trace::mark("update: prepare storage errno=" + std::to_string(code));
                if ((code != EACCES && code != EPERM && code != ENOENT) || !recover_storage())
                {
                    failure(storage_error(code));
                    return;
                }
                if (sf_update_dirs(path.c_str()) < 0)
                {
                    failure(storage_error(errno));
                    return;
                }
                trace::mark("update: recovered app data access");
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
            if (package)
            {
                /* The system installer takes the package whole; it only has
                   to be exactly the file GitHub published. */
                char digest[65];
                if (sf_update_hash_file(path.c_str(), digest) < 0 || selected.digest != digest)
                {
                    unlink(path.c_str());
                    failure("The downloaded package did not match GitHub's checksum. Your app "
                            "is unchanged. Try again.");
                    return;
                }
            }
            else if (!stage_archive(path, folder + "/staged", selected, error))
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
            std::string payload;
            unlink((folder + "/close-only").c_str());
            unlink((folder + "/closed").c_str());
            if (app_files::installed_package())
            {
                /* The helper installs this title's package over itself. It
                   needs the running title and the download it must match. */
                Release selected;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    selected = release;
                }
                sf_pkg_request request{};
                request.pid = getpid();
                if (!running_title(request.title))
                {
                    failure("Could not identify this installed app. Your app was not changed.");
                    return;
                }
                std::snprintf(request.sha256, sizeof(request.sha256), "%s",
                              selected.digest.c_str());
                std::snprintf(request.version, sizeof(request.version), "%s",
                              selected.version.c_str());
                request.size = selected.size;
                char text[SF_PKG_REQUEST_MAX];
                const int length = sf_pkg_request_format(&request, text, sizeof(text));
                if (length < 0 || !save(folder + "/pkg-request", std::string(text, length)))
                {
                    failure("Could not prepare the update request. Your app was not changed.");
                    return;
                }
                payload = small_application_file("assets/slopfin-pkg-update.bin", 1024 * 1024);
            }
            else
            {
                /* Require the running folder to match before replacing anything. */
                char executable[65], param[65];
                if (application_hash("eboot.bin", executable) < 0 ||
                    application_hash("sce_sys/param.json", param) < 0 ||
                    !save(folder + "/source", std::string(executable) + "\n" + param + "\n") ||
                    !save(folder + "/pid", std::to_string(getpid())))
                {
                    failure("Could not identify this app folder. Your app was not changed.");
                    return;
                }
                payload = small_application_file("assets/slopfin-update.bin", 1024 * 1024);
            }
            if (payload.empty())
            {
                failure("The update helper is missing. Install the full release.");
                return;
            }
            /* A helper writes 'ready' only after validating files and saving a backup. */
            unlink((folder + "/result").c_str());
            if (!http::send_bytes("127.0.0.1", 9021, payload))
            {
                failure(
                    "ELF Loader (elfldr) is not running on port 9021. Start ELF Loader from your "
                    "jailbreak tools, then retry Install. The verified download is saved.");
                return;
            }
            for (unsigned i = 0; i < 300; ++i)
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
bool finish_close() noexcept
{
#ifdef SLOPFIN_HOST
    return true;
#else
    const std::string folder = base();
    const std::string pid = std::to_string(getpid());
    if (!save(folder + "/closed", pid + "\n"))
        return false;
    /* An update helper is already waiting for this acknowledgement. */
    if (restart.load() || access((folder + "/install.lock").c_str(), F_OK) == 0)
        return true;
    /* Tooling's ordinary quit uses the same helper in close-only mode. */
    char executable[65], param[65];
    if (application_hash("eboot.bin", executable) < 0 ||
        application_hash("sce_sys/param.json", param) < 0 ||
        !save(folder + "/source", std::string(executable) + "\n" + param + "\n") ||
        !save(folder + "/pid", pid) || !save(folder + "/close-only", pid + "\n"))
        return false;
    const auto payload = small_application_file("assets/slopfin-update.bin", 1024 * 1024);
    return !payload.empty() && http::send_bytes("127.0.0.1", 9021, payload);
#endif
}
} // namespace slopfin::update
