/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../src/update/files.h"
#include <dirent.h>
#ifndef SLOPFIN_STORAGE_TEST
#include <ps5/kernel.h>
#endif
#include <stdlib.h>

static int recover(const char *sandbox)
{
    char path[128], text[32] = {0};
    snprintf(path, sizeof(path), "/mnt/sandbox/%s/download0/slopfin-storage-pid", sandbox);
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    ssize_t count = fd < 0 ? -1 : read(fd, text, sizeof(text) - 1);
    if (fd >= 0)
        close(fd);
    char *end = NULL;
    const long parsed = strtol(text, &end, 10);
    if (count <= 0 || parsed <= 0 || parsed > 0x7fffffff || !end || *end)
        return -1;
    unlink(path);
    const pid_t pid = (pid_t)parsed;
    if (!kernel_get_proc(pid))
        return -1;
    snprintf(path, sizeof(path), "/mnt/sandbox/%s/download0/slopfin-storage-result", sandbox);
    const int result = open(path, O_WRONLY | O_TRUNC | O_NOFOLLOW);
    if (result < 0)
        return -1;
    int valid = 1;
    const char *files[] = {"eboot.bin", "sce_sys/param.json"};
    for (unsigned i = 0; i < 2; ++i)
    {
        char running[65], installed[65];
        snprintf(path, sizeof(path), "/mnt/sandbox/%s/app0/%s", sandbox, files[i]);
        if (sf_update_hash_file(path, running) < 0)
        {
            valid = 0;
            break;
        }
        snprintf(path, sizeof(path), "/data/homebrew/PPSA99001/%s", files[i]);
        if (sf_update_hash_file(path, installed) < 0 || strcmp(running, installed))
        {
            valid = 0;
            break;
        }
    }
    const intptr_t root = kernel_get_root_vnode();
    const int ready =
        valid && kernel_set_proc_jaildir(pid, root) == 0 && kernel_set_proc_rootdir(pid, root) == 0;
    const char *response = ready ? "ready" : "unsupported";
    write(result, response, strlen(response));
    fsync(result);
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
    {
        const char *name = entry->d_name;
        if (strlen(name) == 13 && !strncmp(name, "PPSA99001_", 10) &&
            strspn(name + 10, "0123456789") == 3 && recover(name) == 0)
            ready = 1;
    }
    closedir(dir);
    return ready ? 0 : 1;
}
