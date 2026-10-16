/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_UPDATE_DOWNLOAD_HPP
#define SLOPFIN_UPDATE_DOWNLOAD_HPP

#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace slopfin::update
{
enum class DownloadFailure
{
    none,
    open,
    network,
    truncated,
    oversized,
    write,
    sync,
    close
};
struct DownloadResult
{
    DownloadFailure failure = DownloadFailure::none;
    std::size_t received = 0;
    int system_error = 0;
};

template <class Read, class Progress>
DownloadResult receive_archive(const std::string &path, std::size_t expected, Read read_chunk,
                               Progress progress) noexcept
{
    DownloadResult result;
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return {DownloadFailure::open, 0, errno};
    char buffer[65536];
    for (;;)
    {
        const long n = read_chunk(buffer, sizeof(buffer));
        if (n < 0)
        {
            result.failure = DownloadFailure::network;
            break;
        }
        if (n == 0)
        {
            if (result.received != expected)
                result.failure = DownloadFailure::truncated;
            break;
        }
        if (static_cast<std::size_t>(n) > expected - result.received)
        {
            result.failure = DownloadFailure::oversized;
            break;
        }
        std::size_t have = 0;
        while (have < static_cast<std::size_t>(n))
        {
            const auto written = write(fd, buffer + have, static_cast<std::size_t>(n) - have);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
            {
                result.failure = DownloadFailure::write;
                result.system_error = written < 0 ? errno : EIO;
                break;
            }
            have += static_cast<std::size_t>(written);
        }
        if (result.failure != DownloadFailure::none)
            break;
        result.received += static_cast<std::size_t>(n);
        progress(result.received);
    }
    if (result.failure == DownloadFailure::none && fsync(fd) < 0)
    {
        result.failure = DownloadFailure::sync;
        result.system_error = errno;
    }
    if (close(fd) < 0 && result.failure == DownloadFailure::none)
    {
        result.failure = DownloadFailure::close;
        result.system_error = errno;
    }
    if (result.failure != DownloadFailure::none)
        unlink(path.c_str());
    return result;
}
} // namespace slopfin::update
#endif
