/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <string.h>
#define SLOPFIN_CANARY_TEST
#include "../tools/native-pkg/canary/main.c"

static int open_result, writes, syncs, closes;
static long write_result;
static char output[64];
static size_t output_size;

int sceKernelOpen(const char *path, int flags, unsigned short mode)
{
    assert(!strcmp(path, "/download0/slopfin-pkg-canary-v1.txt"));
    assert(flags == (O_WRONLY | O_CREAT | O_TRUNC) && mode == 0600);
    return open_result;
}
long sceKernelWrite(int fd, const void *bytes, size_t size)
{
    assert(fd == 7);
    ++writes;
    if (write_result <= 0 || (size_t)write_result > size)
        return write_result;
    memcpy(output + output_size, bytes, (size_t)write_result);
    output_size += (size_t)write_result;
    return write_result;
}
int sceKernelFsync(int fd)
{
    assert(fd == 7);
    ++syncs;
    return 0;
}
int sceKernelClose(int fd)
{
    assert(fd == 7);
    ++closes;
    return 0;
}
static void reset(void)
{
    open_result = 7;
    write_result = 1;
    writes = syncs = closes = 0;
    output_size = 0;
    memset(output, 0, sizeof(output));
}
int main(void)
{
    reset();
    record_startup();
    assert(!strcmp(output, "SLOPFIN_PKG_CANARY_V1\n") && syncs == 1 && closes == 1);
    reset();
    open_result = -1;
    record_startup();
    assert(writes == 0 && syncs == 0 && closes == 0);
    for (long result = -1; result <= 0; ++result)
    {
        reset();
        write_result = result;
        record_startup();
        assert(writes == 1 && syncs == 0 && closes == 1);
    }
    reset();
    write_result = 100;
    record_startup();
    assert(writes == 1 && syncs == 0 && closes == 1);
}
