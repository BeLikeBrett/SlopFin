/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>

int sceKernelOpen(const char *path, int flags, unsigned short mode);
long sceKernelWrite(int fd, const void *bytes, size_t size);
int sceKernelFsync(int fd);
int sceKernelClose(int fd);
int sceKernelUsleep(uint32_t microseconds);

static void record_startup(void)
{
    static const char marker[] = "SLOPFIN_PKG_CANARY_V1\n";
    const int fd =
        sceKernelOpen("/download0/slopfin-pkg-canary-v1.txt", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return;
    size_t written = 0;
    while (written < sizeof(marker) - 1)
    {
        const long count = sceKernelWrite(fd, marker + written, sizeof(marker) - 1 - written);
        if (count <= 0 || (size_t)count > sizeof(marker) - 1 - written)
            break;
        written += (size_t)count;
    }
    if (written == sizeof(marker) - 1)
        (void)sceKernelFsync(fd);
    (void)sceKernelClose(fd);
}

#ifndef SLOPFIN_CANARY_TEST
/* Direct kernel calls avoid app constructors, shared settings and UI.
   The platform still requires its packaged clean-room companion module.
   Remain alive after the one startup marker so the shell owns termination. */
__attribute__((noreturn, visibility("default"))) void _start(void *parameters, void *teardown)
{
    (void)parameters;
    (void)teardown;
    record_startup();
    for (;;)
        (void)sceKernelUsleep(1000000);
}
#endif
