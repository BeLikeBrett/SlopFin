/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "update/files.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

#ifndef SLOPFIN_HOST
extern "C" int _openat(int, const char *, int, ...);
#endif

namespace slopfin::app_files
{
class Directory
{
    std::atomic<int> descriptor_{-1};
    /* Where the directory was captured, and what it is, for the fallbacks. */
    std::string root_;
    std::string sandboxes_;
    dev_t device_ = 0;
    ino_t inode_ = 0;
    mutable std::mutex fallback_mutex_;
    mutable std::string found_; /* the directory's path after a root change */
    /* Files read while the captured path still worked. After the storage
       helper changes this process's root, an installed package can neither
       name them nor duplicate a descriptor to them. */
    struct Kept
    {
        std::string path;
        bool used = false;
        bool has_bytes = false;
        std::string bytes;
        char sha256[65] = {};
    };
    static constexpr std::size_t kKept = 8;

  public:
    int kept_count() const noexcept
    {
        std::lock_guard<std::mutex> guard(kept_mutex_);
        int count = 0;
        for (const Kept &slot : kept_)
            count += slot.used ? 1 : 0;
        return count;
    }

  private:
    Kept kept_[kKept];
    mutable std::mutex kept_mutex_;

    int relative_open(int parent, const char *name, int flags) const noexcept
    {
        if (refuse_relative)
        {
            errno = EPERM;
            return -1;
        }
#ifdef SLOPFIN_HOST
        return ::openat(parent, name, flags);
#else
        return _openat(parent, name, flags);
#endif
    }

    bool same_directory(const std::string &path) const noexcept
    {
        struct stat st = {};
        return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_dev == device_ &&
               st.st_ino == inode_;
    }

    /*
     * An installed package's sandbox refuses relative opens. Before the
     * storage helper changes this process's root the captured path still
     * names the same directory; after it, the directory is found again under
     * the sandbox mounts by its identity, never by name alone.
     */
    std::string fallback_root() const noexcept
    {
        std::lock_guard<std::mutex> guard(fallback_mutex_);
        if (!found_.empty() && same_directory(found_))
            return found_;
        found_.clear();
        if (!root_.empty() && same_directory(root_))
            return found_ = root_;
        if (sandboxes_.empty())
            return {};
        /* A sandbox's own app0, or the package mount behind it. */
        const auto scan = [this](const std::string &folder, const char *suffix)
        {
            DIR *dir = ::opendir(folder.c_str());
            if (dir == nullptr)
                return false;
            while (const dirent *entry = ::readdir(dir))
            {
                const std::string_view name(entry->d_name);
                if (name.empty() || name == "." || name == ".." || name.size() > 64)
                    continue;
                const std::string candidate = folder + "/" + std::string(name) + suffix;
                if (same_directory(candidate))
                {
                    found_ = candidate;
                    break;
                }
            }
            (void)::closedir(dir);
            return !found_.empty();
        };
        if (!scan(sandboxes_, "/app0"))
            (void)scan(sandboxes_ + "/pfsmnt", "");
        return found_;
    }

  public:
    /* Tests only: behave as an installed package's sandbox does. */
    bool refuse_relative = false;

    Directory() = default;
    Directory(const Directory &) = delete;
    Directory &operator=(const Directory &) = delete;
    ~Directory()
    {
        const int fd = descriptor();
        if (fd >= 0)
            (void)::close(fd);
    }
    int descriptor() const noexcept
    {
        return descriptor_.load(std::memory_order_acquire);
    }
    bool capture(const char *root, const char *sandboxes = nullptr) noexcept
    {
        if (descriptor() >= 0)
            return true;
        const int fd = ::open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (fd < 0)
            return false;
        struct stat st = {};
        if (::fstat(fd, &st) != 0)
        {
            (void)::close(fd);
            return false;
        }
        root_ = root;
        sandboxes_ = sandboxes != nullptr ? sandboxes : "";
        device_ = st.st_dev;
        inode_ = st.st_ino;
        int expected = -1;
        if (!descriptor_.compare_exchange_strong(expected, fd, std::memory_order_release))
            (void)::close(fd);
        return true;
    }
    int open(std::string_view path) const noexcept
    {
        if (path.empty() || path.size() > 200 || path.front() == '/' || path.back() == '/' ||
            path.find('\0') != std::string_view::npos)
        {
            errno = EINVAL;
            return -1;
        }
        for (std::size_t at = 0; at < path.size();)
        {
            const auto end = path.find('/', at);
            const auto part = path.substr(at, end == path.npos ? path.size() - at : end - at);
            if (part.empty() || part == "." || part == "..")
            {
                errno = EINVAL;
                return -1;
            }
            at += part.size() + 1;
        }
        int parent = descriptor();
        if (parent < 0)
        {
            errno = ENOENT;
            return -1;
        }
        bool owned = false;
        for (std::size_t at = 0;;)
        {
            const auto end = path.find('/', at);
            const bool last = end == path.npos;
            char part[201];
            const auto length = last ? path.size() - at : end - at;
            std::memcpy(part, path.data() + at, length);
            part[length] = '\0';
            const int fd =
                relative_open(parent, part, O_RDONLY | O_NOFOLLOW | (last ? 0 : O_DIRECTORY));
            const int error = errno;
            if (owned)
                (void)::close(parent);
            if (fd < 0 && (error == EPERM || error == EACCES || error == ENOSYS))
                return path_open(path);
            if (fd < 0 || last)
            {
                errno = error;
                return fd;
            }
            parent = fd;
            owned = true;
            at = end + 1;
        }
    }

    /*
     * Opens `path` now and keeps it, so a later open() still reaches it after
     * this process's root has changed. Call before the storage helper runs.
     */
    bool keep(std::string_view path, std::size_t max_bytes = 1024 * 1024) noexcept
    {
        int fd = open(path);
        /* Nothing has changed this process's root yet, so the captured path
           itself is still right, whatever identity a package mount reports. */
        if (fd < 0 && !root_.empty() && path.find("..") == std::string_view::npos &&
            path.front() != '/')
            fd = ::open((root_ + "/" + std::string(path)).c_str(), O_RDONLY);
        if (fd < 0)
            return false;
        Kept kept;
        kept.path = std::string(path);
        bool ok = sf_update_hash_fd(fd, kept.sha256) == 0 && ::lseek(fd, 0, SEEK_SET) == 0;
        struct stat st = {};
        if (ok && ::fstat(fd, &st) == 0 && st.st_size >= 0 &&
            static_cast<std::size_t>(st.st_size) <= max_bytes)
        {
            kept.bytes.resize(static_cast<std::size_t>(st.st_size));
            std::size_t have = 0;
            while (have < kept.bytes.size())
            {
                const ssize_t got = ::read(fd, kept.bytes.data() + have, kept.bytes.size() - have);
                if (got <= 0)
                    break;
                have += static_cast<std::size_t>(got);
            }
            kept.has_bytes = have == kept.bytes.size();
            ok = kept.has_bytes;
        }
        (void)::close(fd);
        if (!ok && !kept.sha256[0])
            return false;
        kept.used = true;
        std::lock_guard<std::mutex> guard(kept_mutex_);
        for (Kept &slot : kept_)
            if (!slot.used)
            {
                slot = std::move(kept);
                return true;
            }
        return false;
    }

    /* A kept file's bytes; false when it was not kept whole. */
    bool kept_bytes(std::string_view path, std::string &out) const noexcept
    {
        std::lock_guard<std::mutex> guard(kept_mutex_);
        for (const Kept &slot : kept_)
            if (slot.used && slot.has_bytes && slot.path == path)
            {
                out = slot.bytes;
                return true;
            }
        return false;
    }

    /* A kept file's SHA-256, as lowercase hex. */
    bool kept_hash(std::string_view path, char hex[65]) const noexcept
    {
        std::lock_guard<std::mutex> guard(kept_mutex_);
        for (const Kept &slot : kept_)
            if (slot.used && slot.sha256[0] && slot.path == path)
            {
                std::memcpy(hex, slot.sha256, 65);
                return true;
            }
        return false;
    }

  private:
    /* The same validated relative path, opened by name under the directory. */
    int path_open(std::string_view path) const noexcept
    {
        const std::string base = fallback_root();
        if (base.empty())
        {
            errno = EPERM;
            return -1;
        }
        const std::string full = base + "/" + std::string(path);
        return ::open(full.c_str(), O_RDONLY | O_NOFOLLOW);
    }
};

// Capture before any helper changes this process's root. The descriptor keeps
// package assets reachable without assuming a writable homebrew-folder copy.
inline Directory application;
inline bool capture() noexcept
{
#ifdef SLOPFIN_HOST
    return application.capture(".");
#else
    static std::once_flag once;
    const bool captured = application.capture("/app0", "/mnt/sandbox");
    /* Everything read from the app after storage recovery. A sandbox that
       refuses relative opens, as an installed package's does, cannot reach
       them by name once the process root has changed. */
    if (captured)
        std::call_once(once,
                       []
                       {
                           for (const char *path :
                                {"assets/ca-certificates.pem", "assets/slopfin-sandbox.bin",
                                 "assets/slopfin-update.bin", "assets/slopfin-pkg-update.bin",
                                 "assets/slopfin-storage.bin", "sce_sys/param.json"})
                               (void)application.keep(path);
                           /* The updater only compares its hash. */
                           (void)application.keep("eboot.bin", 0);
                       });
    return captured;
#endif
}
inline int open(std::string_view relative) noexcept
{
    return capture() ? application.open(relative) : -1;
}

/*
 * An installed package's sce_sys is readable only by the system, so its
 * param.json could not be kept; the folder build's is an ordinary file.
 * (statfs was not used: the console's structure is larger than the SDK's.)
 */
inline bool installed_package() noexcept
{
#ifdef SLOPFIN_HOST
    return false;
#else
    char hex[65];
    return capture() && !application.kept_hash("sce_sys/param.json", hex);
#endif
}

/* A file's whole contents, up to `max` bytes: kept at startup when it was,
   otherwise read now. Empty when it is missing or larger. */
inline std::string contents(std::string_view relative, std::size_t max) noexcept
{
    std::string out;
    if (capture() && application.kept_bytes(relative, out))
        return out.size() <= max ? out : std::string{};
    const int fd = open(relative);
    if (fd < 0)
        return {};
    struct stat st = {};
    if (::fstat(fd, &st) == 0 && st.st_size >= 0 && static_cast<std::size_t>(st.st_size) <= max)
    {
        out.resize(static_cast<std::size_t>(st.st_size));
        std::size_t have = 0;
        while (have < out.size())
        {
            const ssize_t got = ::read(fd, out.data() + have, out.size() - have);
            if (got <= 0)
                break;
            have += static_cast<std::size_t>(got);
        }
        if (have != out.size())
            out.clear();
    }
    (void)::close(fd);
    return out;
}

/* A file's SHA-256 as lowercase hex: kept at startup when it was. */
inline bool hash(std::string_view relative, char hex[65]) noexcept
{
    if (capture() && application.kept_hash(relative, hex))
        return true;
    const int fd = open(relative);
    const bool ok = fd >= 0 && sf_update_hash_fd(fd, hex) == 0;
    if (fd >= 0)
        (void)::close(fd);
    return ok;
}
} // namespace slopfin::app_files
