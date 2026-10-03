/*
 * SlopFin - let the app see the console's whole filesystem.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * An application runs chrooted into a sandbox, where the media gallery
 * (/user/av_contents) and USB drives (/mnt/usb*) do not exist. The profile
 * picture picker needs them. SlopFin sends this payload to elfldr on the
 * console itself; it reads SlopFin's pid from /data/slopfin-sandbox-pid and
 * points that process's root and jail directories at the real root vnode,
 * which is what homebrew launchers do for their own apps. The result goes to
 * /data/slopfin-sandbox-result. (Opus 5, 2026-09-15.)
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ps5/kernel.h>

int main(void)
{
    char text[32] = {0};
    const int in = open("/data/slopfin-sandbox-pid", O_RDONLY);
    if (in < 0)
        return 1;
    (void)read(in, text, sizeof(text) - 1);
    (void)close(in);
    const pid_t pid = (pid_t)atoi(text);

    char result[96];
    int length;
    if (pid <= 0)
        length = snprintf(result, sizeof(result), "bad pid\n");
    else
    {
        const intptr_t root = kernel_get_root_vnode();
        const int rootdir = kernel_set_proc_rootdir(pid, root);
        const int jaildir = kernel_set_proc_jaildir(pid, root);
        length = snprintf(result, sizeof(result), "pid %d root %d jail %d\n", (int)pid, rootdir, jaildir);
    }
    const int out = open("/data/slopfin-sandbox-result", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out >= 0)
    {
        (void)write(out, result, (size_t)length);
        (void)close(out);
    }
    return 0;
}
