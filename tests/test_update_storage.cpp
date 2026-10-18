/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "update/files.h"
#include <cassert>
#include <string>
#include <dirent.h>
#include <cstdlib>

namespace
{
std::string pid_text = "123", response;
bool matches = true, exists = true, root_failure = false;
int changes = 0;
int storage_open(const char *path, int)
{
    return std::string(path).find("-pid") != std::string::npos ? 10 : 11;
}
ssize_t storage_read(int fd, void *bytes, size_t size)
{
    assert(fd == 10 && pid_text.size() <= size);
    memcpy(bytes, pid_text.data(), pid_text.size());
    return static_cast<ssize_t>(pid_text.size());
}
ssize_t storage_write(int fd, const void *bytes, size_t size)
{
    assert(fd == 11);
    response.assign(static_cast<const char *>(bytes), size);
    return static_cast<ssize_t>(size);
}
int storage_close(int)
{
    return 0;
}
int storage_unlink(const char *)
{
    return 0;
}
int storage_sync(int)
{
    return 0;
}
int storage_hash(const char *path, char hex[65])
{
    memset(hex, matches || std::string(path).find("/mnt/sandbox/") == 0 ? '0' : '1', 64);
    hex[64] = 0;
    return 0;
}
intptr_t kernel_get_proc(pid_t pid)
{
    return exists && pid == 123 ? 1000 : 0;
}
intptr_t kernel_get_root_vnode()
{
    return 2000;
}
int kernel_set_proc_jaildir(pid_t pid, intptr_t root)
{
    assert(pid == 123 && root == 2000);
    ++changes;
    return 0;
}
int kernel_set_proc_rootdir(pid_t pid, intptr_t root)
{
    assert(pid == 123 && root == 2000);
    ++changes;
    return root_failure ? -1 : 0;
}
void reset()
{
    pid_text = "123";
    response.clear();
    matches = exists = true;
    root_failure = false;
    changes = 0;
}
} // namespace

#define SLOPFIN_STORAGE_TEST
#define main storage_payload_main
#define open storage_open
#define read storage_read
#define write storage_write
#define close storage_close
#define unlink storage_unlink
#define fsync storage_sync
#define sf_update_hash_file storage_hash
#include "../payloads/storage/main.c"
#undef main

int main()
{
    reset();
    assert(recover("PPSA99001_000") == 0 && changes == 2 && response == "ready");
    reset();
    matches = false;
    assert(recover("PPSA99001_000") < 0 && changes == 0 && response == "unsupported");
    reset();
    exists = false;
    assert(recover("PPSA99001_000") < 0 && changes == 0 && response.empty());
    for (const char *invalid : {"", "-1", "123oops", "2147483648"})
    {
        reset();
        pid_text = invalid;
        assert(recover("PPSA99001_000") < 0 && changes == 0 && response.empty());
    }
    reset();
    root_failure = true;
    assert(recover("PPSA99001_000") < 0 && response == "unsupported");
}
