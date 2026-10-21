/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "update/files.h"
#include "update/storage_request.h"
#include <cassert>
#include <string>
#include <dirent.h>

namespace
{
std::string request_text, response;
bool exists, app_matches, result_matches, root_matches, regular;
int fail_set, changes, removed, opened, closed;
intptr_t current_root, current_jail;
int storage_open(const char *path, int flags)
{
    assert(!(flags & O_TRUNC));
    ++opened;
    const std::string name(path);
    if (name.ends_with("-request"))
        return 10;
    if (name.ends_with("-result"))
        return 11;
    return name.ends_with("/app0") ? 12 : 13;
}
ssize_t storage_read(int fd, void *bytes, size_t size)
{
    assert(fd == 10);
    const size_t count = std::min(size, request_text.size());
    memcpy(bytes, request_text.data(), count);
    return static_cast<ssize_t>(count);
}
ssize_t storage_write(int fd, const void *bytes, size_t size)
{
    assert(fd == 11);
    const size_t part = std::min(size, size_t{2});
    response.append(static_cast<const char *>(bytes), part);
    return static_cast<ssize_t>(part);
}
int storage_close(int)
{
    ++closed;
    return 0;
}
int storage_unlink(const char *)
{
    ++removed;
    return 0;
}
int storage_sync(int)
{
    return 0;
}
int storage_stat(int fd, struct stat *metadata)
{
    assert(fd == 11);
    metadata->st_mode = regular ? S_IFREG : S_IFDIR;
    return 0;
}
intptr_t kernel_get_proc(pid_t pid)
{
    return exists && pid == 123 ? 1000 : 0;
}
intptr_t kernel_get_proc_file(pid_t pid, int fd)
{
    if (pid == 123)
        return fd == 20 ? (app_matches ? 3000 : 1) : (result_matches ? 4000 : 1);
    assert(pid == getpid());
    return fd == 12 ? 3000 : (fd == 11 ? 4000 : 5000);
}
intptr_t kernel_get_proc_rootdir(pid_t pid)
{
    assert(pid == 123);
    return root_matches ? current_root : 1;
}
intptr_t kernel_get_proc_jaildir(pid_t pid)
{
    assert(pid == 123);
    return current_jail;
}
intptr_t kernel_get_root_vnode()
{
    return 9000;
}
int set_directory(pid_t pid, intptr_t root, intptr_t &directory)
{
    assert(pid == 123);
    if (++changes == fail_set)
        return -1;
    directory = root;
    return 0;
}
int kernel_set_proc_jaildir(pid_t pid, intptr_t root)
{
    return set_directory(pid, root, current_jail);
}
int kernel_set_proc_rootdir(pid_t pid, intptr_t root)
{
    return set_directory(pid, root, current_root);
}
void reset()
{
    request_text = SF_STORAGE_REQUEST_PREFIX "123\n20\n21\n";
    response.clear();
    exists = app_matches = result_matches = root_matches = regular = true;
    fail_set = changes = removed = opened = closed = 0;
    current_root = 5000;
    current_jail = 6000;
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
#define fstat storage_stat
#include "../payloads/storage/main.c"
#undef main

int main()
{
    for (const char *name : {"PPSA99001_000", "PPSA99005_001"})
    {
        reset();
        assert(recover(name) == 0 && changes == 2 && response == "ready");
        assert(current_root == 9000 && current_jail == 9000 && removed == 1);
        assert(opened == closed);
    }
    for (bool *invalid : {&exists, &app_matches, &result_matches, &root_matches, &regular})
    {
        reset();
        *invalid = false;
        assert(recover("PPSA99005_000") < 0 && changes == 0 && response.empty());
        assert(removed == 0 && opened == closed);
    }
    for (int step : {1, 2})
    {
        reset();
        fail_set = step;
        assert(recover("PPSA99005_000") < 0 && response == "unsupported");
        assert(current_root == 5000 && current_jail == 6000 && opened == closed);
    }
    for (const char *invalid :
         {"", "123", SF_STORAGE_REQUEST_PREFIX "0\n20\n21\n",
          SF_STORAGE_REQUEST_PREFIX "2147483648\n20\n21\n",
          SF_STORAGE_REQUEST_PREFIX "123\n1024\n21\n", SF_STORAGE_REQUEST_PREFIX "123\n20\n20\n",
          SF_STORAGE_REQUEST_PREFIX "123\n-1\n21\n", SF_STORAGE_REQUEST_PREFIX "123\n20\n21",
          SF_STORAGE_REQUEST_PREFIX "123\n20\n21\nx"})
    {
        reset();
        request_text = invalid;
        assert(recover("PPSA99005_000") < 0 && changes == 0 && response.empty());
        assert(opened == closed);
    }
    reset();
    request_text += '\0';
    assert(recover("PPSA99005_000") < 0 && changes == 0);
    for (const char *invalid :
         {"../PPSA99001_000", "PPSA99001_00", "PPSA99001_0000", "PPSA9900x_000", "PPSA99001_00x"})
    {
        reset();
        assert(recover(invalid) < 0 && changes == 0 && opened == 0);
    }
}
