/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_UPDATE_CLOSE_H
#define SLOPFIN_UPDATE_CLOSE_H
#include "files.h"

static inline int sf_update_pid_marker(const char *path, long pid)
{
    char actual[32], expected[32];
    const int length = snprintf(expected, sizeof(expected), "%ld\n", pid);
    const int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return 0;
    const ssize_t count = read(fd, actual, sizeof(actual));
    close(fd);
    return pid > 0 && length > 0 && count == length && !memcmp(actual, expected, length);
}

int sceKernelGetAppInfo(int, void *);
int sceLncUtilGetAppId(const char *);
int sceLncUtilKillApp(unsigned int);
/* Called only by a privileged helper, after the render thread's acknowledgement. */
static inline int sf_update_close_stopped(long pid, const char *title, const char *marker)
{
    if (!sf_update_pid_marker(marker, pid))
        return -1;
    unsigned char info[0x60] = {0};
    unsigned int app_id = 0;
    if (sceKernelGetAppInfo((int)pid, info) != 0 || memcmp(info + 16, title, 9))
        return -1;
    memcpy(&app_id, info, sizeof(app_id));
    const int current = sceLncUtilGetAppId(title);
    if (current <= 0 || app_id != (unsigned int)current)
        return -1;
    return sceLncUtilKillApp(app_id);
}
#endif
