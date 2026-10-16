/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <algorithm>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cassert>
#include <cstring>
#include <string>
#include <cstdlib>
#include <sys/stat.h>

namespace
{
bool sync_failure = false, short_writes = false, interrupt_write = false;
int sync_file(int fd)
{
    if (sync_failure)
    {
        errno = EIO;
        return -1;
    }
    return ::fsync(fd);
}
ssize_t write_file(int fd, const void *bytes, std::size_t n)
{
    if (interrupt_write)
    {
        interrupt_write = false;
        errno = EINTR;
        return -1;
    }
    return ::write(fd, bytes, short_writes ? std::max<std::size_t>(1, n / 2) : n);
}
} // namespace
#define fsync sync_file
#define write write_file
#include "update_download.hpp"
#undef fsync
#undef write

int main()
{
    using namespace slopfin::update;
    char dir[] = "/tmp/slopfin-download-XXXXXX";
    assert(mkdtemp(dir));
    const std::string path = std::string(dir) + "/release.zip";
    const std::string bytes(180000, 'z');
    auto run = [&](std::size_t expected, bool fail_network = false)
    {
        std::size_t sent = 0, reported = 0;
        return receive_archive(
            path, expected,
            [&](void *out, std::size_t capacity) -> long
            {
                if (fail_network && sent > 0)
                    return -1;
                auto n = std::min<std::size_t>({capacity, 24000, bytes.size() - sent});
                std::memcpy(out, bytes.data() + sent, n);
                sent += n;
                return static_cast<long>(n);
            },
            [&](std::size_t count)
            {
                assert(count > reported);
                reported = count;
            });
    };
    short_writes = interrupt_write = true;
    auto r = run(bytes.size());
    assert(r.failure == DownloadFailure::none && r.received == bytes.size());
    struct stat st{};
    assert(stat(path.c_str(), &st) == 0 && st.st_size == static_cast<long>(bytes.size()));
    assert((st.st_mode & 0777) == 0600);
    short_writes = false;
    r = run(bytes.size(), true);
    assert(r.failure == DownloadFailure::network && access(path.c_str(), F_OK) < 0);
    r = run(bytes.size() + 1);
    assert(r.failure == DownloadFailure::truncated && access(path.c_str(), F_OK) < 0);
    r = run(bytes.size() - 1);
    assert(r.failure == DownloadFailure::oversized && access(path.c_str(), F_OK) < 0);
    sync_failure = true;
    r = run(bytes.size());
    assert(r.failure == DownloadFailure::sync && r.system_error == EIO);
    assert(access(path.c_str(), F_OK) < 0);
    sync_failure = false;
    const auto target = std::string(dir) + "/untouched";
    int fd = open(target.c_str(), O_CREAT | O_WRONLY, 0600);
    assert(fd >= 0);
    close(fd);
    assert(symlink(target.c_str(), path.c_str()) == 0);
    r = run(bytes.size());
    assert(r.failure == DownloadFailure::open);
    assert(stat(target.c_str(), &st) == 0 && st.st_size == 0);
    unlink(path.c_str());
    unlink(target.c_str());
    rmdir(dir);
}
