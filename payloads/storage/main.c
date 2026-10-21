/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../src/update/files.h"
#include "../../src/update/storage_request.h"
#include <dirent.h>
#ifndef SLOPFIN_STORAGE_TEST
#include <ps5/kernel.h>
#endif

static int sandbox_name(const char *name)
{
    return strlen(name) == 13 && !strncmp(name, "PPSA", 4) && strspn(name + 4, "0123456789") == 5 &&
           name[9] == '_' && strspn(name + 10, "0123456789") == 3;
}

static int recover(const char *sandbox)
{
    if (!sandbox_name(sandbox))
        return -1;
    char path[128], text[96];
    snprintf(path, sizeof(path), "/mnt/sandbox/%s/download0/slopfin-storage-request", sandbox);
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    const ssize_t count = fd < 0 ? -1 : read(fd, text, sizeof(text));
    if (fd >= 0)
        close(fd);
    sf_storage_request request;
    if (count <= 0 || sf_storage_parse_request(text, (size_t)count, &request) < 0 ||
        !kernel_get_proc(request.pid))
        return -1;
    snprintf(path, sizeof(path), "/mnt/sandbox/%s/app0", sandbox);
    const int app = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    snprintf(path, sizeof(path), "/mnt/sandbox/%s", sandbox);
    const int directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    snprintf(path, sizeof(path), "/mnt/sandbox/%s/download0/slopfin-storage-result", sandbox);
    const int result = open(path, O_WRONLY | O_NOFOLLOW);
    struct stat metadata;
    const intptr_t app_vnode = app < 0 ? 0 : kernel_get_proc_file(getpid(), app);
    const intptr_t result_vnode = result < 0 ? 0 : kernel_get_proc_file(getpid(), result);
    const intptr_t sandbox_vnode = directory < 0 ? 0 : kernel_get_proc_file(getpid(), directory);
    const intptr_t original_root = kernel_get_proc_rootdir(request.pid);
    const intptr_t original_jail = kernel_get_proc_jaildir(request.pid);
    const intptr_t root = kernel_get_root_vnode();
    const int valid = app_vnode && result_vnode && sandbox_vnode && root && original_jail &&
                      original_root == sandbox_vnode && result >= 0 &&
                      fstat(result, &metadata) == 0 && S_ISREG(metadata.st_mode) &&
                      app_vnode == kernel_get_proc_file(request.pid, request.app_fd) &&
                      result_vnode == kernel_get_proc_file(request.pid, request.result_fd);
    int ready = 0;
    if (valid)
    {
        snprintf(path, sizeof(path), "/mnt/sandbox/%s/download0/slopfin-storage-request", sandbox);
        unlink(path);
        if (kernel_set_proc_jaildir(request.pid, root) == 0)
        {
            ready = kernel_set_proc_rootdir(request.pid, root) == 0;
            if (!ready)
            {
                kernel_set_proc_rootdir(request.pid, original_root);
                kernel_set_proc_jaildir(request.pid, original_jail);
            }
        }
        const char *response = ready ? "ready" : "unsupported";
        const size_t size = strlen(response);
        size_t written = 0;
        while (written < size)
        {
            const ssize_t part = write(result, response + written, size - written);
            if (part < 0 && errno == EINTR)
                continue;
            if (part <= 0 || (size_t)part > size - written)
                break;
            written += (size_t)part;
        }
        fsync(result);
    }
    if (app >= 0)
        close(app);
    if (directory >= 0)
        close(directory);
    if (result >= 0)
        close(result);
    return ready ? 0 : -1;
}
int main(void)
{
    DIR *dir = opendir("/mnt/sandbox");
    if (!dir)
        return 1;
    int ready = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)))
        if (recover(entry->d_name) == 0)
            ready = 1;
    closedir(dir);
    return ready ? 0 : 1;
}
