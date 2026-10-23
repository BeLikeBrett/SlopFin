/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "update/close.h"
#include <assert.h>
#include <stdlib.h>
static int calls = 0, wrong_title = 0, wrong_app = 0;
int sceKernelGetAppInfo(int pid, void *output)
{
    assert(pid == 123);
    unsigned char *info = output;
    const unsigned id = wrong_app ? 9 : 7;
    memcpy(info, &id, sizeof(id));
    memcpy(info + 16, wrong_title ? "PPSA99999" : "PPSA99001", 9);
    return 0;
}
int sceLncUtilGetAppId(const char *title)
{
    assert(!strcmp(title, "PPSA99001"));
    return 7;
}
int sceLncUtilKillApp(unsigned id)
{
    assert(id == 7);
    ++calls;
    return 0;
}
int main(void)
{
    char path[] = "/tmp/slopfin-close-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    assert(sf_update_close_stopped(123, "PPSA99001", path) < 0 && calls == 0);
    assert(sf_update_write(path, "122\n", 4) == 0);
    assert(sf_update_close_stopped(123, "PPSA99001", path) < 0 && calls == 0);
    assert(sf_update_write(path, "123\nextra", 9) == 0);
    assert(sf_update_close_stopped(123, "PPSA99001", path) < 0 && calls == 0);
    assert(sf_update_write(path, "123\n", 4) == 0);
    wrong_title = 1;
    assert(sf_update_close_stopped(123, "PPSA99001", path) < 0 && calls == 0);
    wrong_title = 0;
    wrong_app = 1;
    assert(sf_update_close_stopped(123, "PPSA99001", path) < 0 && calls == 0);
    wrong_app = 0;
    assert(sf_update_close_stopped(123, "PPSA99001", path) == 0 && calls == 1);
    unlink(path);
    assert(sf_update_close_stopped(123, "PPSA99001", path) < 0 && calls == 1);
}
