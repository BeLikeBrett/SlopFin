/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Artwork kept on the console's SSD between runs, so a launch shows the
 * library at once instead of fetching every poster from the server again.
 *
 * A file holds one encoded image exactly as the server sent it, behind a
 * header naming its key. Keys include the server's image tag, which changes
 * whenever the artwork does, so a file is never stale: it is either the same
 * picture or never asked for again. Files are written to a temporary name and
 * renamed, so a run that ends mid-write leaves nothing half-written behind.
 * The folder is held under a size cap, the oldest files going first.
 *
 * Called only from the image workers, never from the render thread.
 */
#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifndef SLOPFIN_HOST
extern "C" int sceKernelRename(const char *, const char *);
extern "C" int sceKernelGetdents(int fd, char *buffer, int size);
#endif

namespace slopfin::image_cache
{
namespace detail
{
/* The console's libc has no rename; its kernel call does the same. */
inline int rename_file(const char *from, const char *to) noexcept
{
#ifdef SLOPFIN_HOST
    return std::rename(from, to);
#else
    return sceKernelRename(from, to);
#endif
}

/*
 * Calls visit(name) for each entry. The console's libc cannot list a folder;
 * its kernel call returns FreeBSD dirents: fileno u32, reclen u16, type u8,
 * namlen u8, name.
 */
template <typename Visit> void each_entry(const std::string &directory, Visit visit)
{
#ifdef SLOPFIN_HOST
    DIR *dir = ::opendir(directory.c_str());
    if (dir == nullptr)
        return;
    while (const dirent *entry = ::readdir(dir))
        visit(std::string(entry->d_name));
    (void)::closedir(dir);
#else
    const int fd = ::open(directory.c_str(), O_RDONLY);
    if (fd < 0)
        return;
    /* Some console filesystems refuse a read smaller than one of their blocks. */
    static char buffer[65536];
    for (;;)
    {
        const int got = sceKernelGetdents(fd, buffer, sizeof(buffer));
        if (got <= 0)
            break;
        for (int at = 0; at + 8 <= got;)
        {
            std::uint16_t length = 0;
            std::memcpy(&length, buffer + at + 4, sizeof(length));
            if (length < 8 || at + length > got)
                break;
            const auto name_length = static_cast<unsigned char>(buffer[at + 7]);
            visit(std::string(buffer + at + 8, std::min<int>(name_length, length - 8)));
            at += length;
        }
    }
    (void)::close(fd);
#endif
}

inline constexpr char kMagic[6] = {'S', 'F', 'I', 'C', '1', '\n'};
inline constexpr std::size_t kMaxImage = 8u * 1024u * 1024u;

struct State
{
    std::mutex mutex;
    std::string directory;
    std::uint64_t cap = 0;
    std::uint64_t total = 0;
    bool scanned = false;
};
inline State &state() noexcept
{
    static State s;
    return s;
}

/* FNV-1a, 64-bit: only the file name; the stored key settles identity. */
inline std::string name_for(const std::string &key) noexcept
{
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char c : key)
    {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char text[24];
    std::snprintf(text, sizeof(text), "%016llx.img", static_cast<unsigned long long>(hash));
    return text;
}

inline bool read_all(int fd, void *buffer, std::size_t size) noexcept
{
    auto *at = static_cast<unsigned char *>(buffer);
    while (size)
    {
        const ssize_t got = ::read(fd, at, size);
        if (got <= 0)
            return false;
        at += got;
        size -= static_cast<std::size_t>(got);
    }
    return true;
}

inline bool write_all(int fd, const void *buffer, std::size_t size) noexcept
{
    const auto *at = static_cast<const unsigned char *>(buffer);
    while (size)
    {
        const ssize_t sent = ::write(fd, at, size);
        if (sent <= 0)
            return false;
        at += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

struct File
{
    std::string name;
    std::uint64_t size;
    std::int64_t used;
};

/* Call with the mutex held. Counts the folder once, then keeps the count. */
inline void scan_locked(State &s) noexcept
{
    if (s.scanned)
        return;
    s.scanned = true;
    s.total = 0;
    each_entry(s.directory,
               [&s](const std::string &name)
               {
                   struct stat st = {};
                   if (!name.empty() && name[0] != '.' &&
                       ::stat((s.directory + "/" + name).c_str(), &st) == 0 && S_ISREG(st.st_mode))
                       s.total += static_cast<std::uint64_t>(st.st_size);
               });
}

/* Call with the mutex held. Removes the oldest until the folder is back
   under three quarters of its cap. */
inline void prune_locked(State &s) noexcept
{
    if (s.total <= s.cap)
        return;
    std::vector<File> files;
    each_entry(s.directory,
               [&](const std::string &name)
               {
                   struct stat st = {};
                   if (name.empty() || name[0] == '.' ||
                       ::stat((s.directory + "/" + name).c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                       return;
                   files.push_back({name, static_cast<std::uint64_t>(st.st_size),
                                    static_cast<std::int64_t>(st.st_mtime)});
               });
    std::sort(files.begin(), files.end(),
              [](const File &a, const File &b) { return a.used < b.used; });
    std::uint64_t total = 0;
    for (const File &f : files)
        total += f.size;
    const std::uint64_t target = s.cap / 4 * 3;
    for (const File &f : files)
    {
        if (total <= target)
            break;
        if (::unlink((s.directory + "/" + f.name).c_str()) == 0)
            total -= f.size;
    }
    s.total = total;
}
} // namespace detail

/* Where the cache lives and how large it may grow. Creates the folder. */
inline bool configure(const std::string &directory, std::uint64_t cap_bytes) noexcept
{
    auto &s = detail::state();
    std::lock_guard<std::mutex> guard(s.mutex);
    s.directory = directory;
    s.cap = cap_bytes;
    s.scanned = false;
    std::string partial;
    for (std::size_t at = 1; at <= directory.size(); ++at)
        if (at == directory.size() || directory[at] == '/')
        {
            partial = directory.substr(0, at);
            if (::mkdir(partial.c_str(), 0700) != 0 && errno != EEXIST)
                return false;
        }
    return true;
}

/* Bytes the folder holds, as counted. */
inline std::uint64_t held_bytes() noexcept
{
    auto &s = detail::state();
    std::lock_guard<std::mutex> guard(s.mutex);
    detail::scan_locked(s);
    return s.total;
}

/* The image stored under `key`, if there is one. */
inline bool load(const std::string &key, std::vector<unsigned char> &out) noexcept
{
    out.clear();
    auto &s = detail::state();
    std::string path;
    {
        std::lock_guard<std::mutex> guard(s.mutex);
        if (s.directory.empty() || key.empty() || key.size() > 1024)
            return false;
        path = s.directory + "/" + detail::name_for(key);
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return false;
    struct stat st = {};
    char magic[sizeof(detail::kMagic)];
    std::uint16_t length = 0;
    std::string stored;
    bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
              detail::read_all(fd, magic, sizeof(magic)) &&
              !std::memcmp(magic, detail::kMagic, sizeof(magic)) &&
              detail::read_all(fd, &length, sizeof(length)) && length == key.size();
    if (ok)
    {
        stored.resize(length);
        ok = detail::read_all(fd, stored.data(), length) && stored == key;
    }
    const std::uint64_t header = sizeof(detail::kMagic) + sizeof(length) + key.size();
    if (ok)
    {
        const auto size = static_cast<std::uint64_t>(st.st_size);
        ok = size > header && size - header <= detail::kMaxImage;
        if (ok)
        {
            out.resize(static_cast<std::size_t>(size - header));
            ok = detail::read_all(fd, out.data(), out.size());
        }
    }
    (void)::close(fd);
    if (!ok)
    {
        out.clear();
        return false;
    }
    return true;
}

/* Keeps `data` under `key`, replacing any earlier copy. */
inline void store(const std::string &key, const unsigned char *data, std::size_t size) noexcept
{
    auto &s = detail::state();
    std::string directory;
    {
        /* Counted before this file lands, so it is not counted twice. */
        std::lock_guard<std::mutex> guard(s.mutex);
        detail::scan_locked(s);
        directory = s.directory;
    }
    if (directory.empty() || key.empty() || key.size() > 1024 || data == nullptr || size == 0 ||
        size > detail::kMaxImage)
        return;
    const std::string name = detail::name_for(key);
    const std::string path = directory + "/" + name;
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), ".%d.%p.tmp", static_cast<int>(::getpid()),
                  static_cast<const void *>(data));
    const std::string temporary = directory + "/." + name + suffix;
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0)
        return;
    const auto length = static_cast<std::uint16_t>(key.size());
    const bool written = detail::write_all(fd, detail::kMagic, sizeof(detail::kMagic)) &&
                         detail::write_all(fd, &length, sizeof(length)) &&
                         detail::write_all(fd, key.data(), key.size()) &&
                         detail::write_all(fd, data, size);
    const bool closed = ::close(fd) == 0;
    struct stat replaced = {};
    const bool existed = ::stat(path.c_str(), &replaced) == 0 && S_ISREG(replaced.st_mode);
    if (!written || !closed || detail::rename_file(temporary.c_str(), path.c_str()) != 0)
    {
        (void)::unlink(temporary.c_str());
        return;
    }
    std::lock_guard<std::mutex> guard(s.mutex);
    const auto old_size = existed ? static_cast<std::uint64_t>(replaced.st_size) : 0u;
    s.total -= std::min(s.total, old_size);
    s.total += sizeof(detail::kMagic) + sizeof(length) + key.size() + size;
    detail::prune_locked(s);
}
} // namespace slopfin::image_cache
