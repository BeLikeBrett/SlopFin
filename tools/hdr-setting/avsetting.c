/*
 * SlopFin - ask the system's AV settings service what it knows about HDR.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ShellUI decides whether HDR is sent with libSceAvSetting: the connected
 * monitor's HDMI information, whether that monitor supports HDR and deep
 * colour, and the current output mode. None of these calls is in the SDK's
 * stub libraries and their layouts are unpublished, so the module is loaded
 * by path, each function looked up by name, and every buffer dumped raw.
 * Every call here only reads.
 */

#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/nid.h>

int sceKernelLoadStartModule(const char *path, size_t args, const void *argp, uint32_t flags, void *opt,
                             int *result);
int sceKernelDlsym(int handle, const char *symbol, void **address);

static char g_text[65536];
static int g_used;
static uint8_t g_buf[65536];

static void say(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    g_used += vsnprintf(g_text + g_used, sizeof(g_text) - (size_t)g_used, format, args);
    va_end(args);
}

static void dump(void)
{
    size_t written = 0;
    for (size_t i = 0; i < sizeof(g_buf); ++i)
        if (g_buf[i] != 0)
            written = i + 1;
    if (written == 0)
    {
        say("    (wrote nothing)\n");
        return;
    }
    say("    wrote %zu bytes\n", written);
    const size_t show = written < 768 ? written : 768;
    for (size_t i = 0; i < show; i += 16)
    {
        say("    %04zx ", i);
        for (size_t j = i; j < i + 16 && j < show; ++j)
            say("%02x ", g_buf[j]);
        say("\n");
    }
}

static void become_shellcore(void)
{
    const pid_t self = getpid();
    static const uint8_t caps[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    (void)kernel_set_ucred_uid(self, 0);
    (void)kernel_set_ucred_caps(self, caps);
    (void)kernel_set_ucred_authid(self, 0x4801000000000013UL);
}

typedef long (*fn)(long, long, long, long);

static fn lookup(int module, const char *name)
{
    void *address = NULL;
    const int rc = sceKernelDlsym(module, name, &address);
    if (rc != 0 || address == NULL)
    {
        say("%s: not found (0x%08x)\n", name, (unsigned)rc);
        return NULL;
    }
    return (fn)address;
}

static void flush(void)
{
    const int fd = open("/data/slopfin-avsetting.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        (void)write(fd, g_text, (size_t)g_used);
        (void)close(fd);
    }
}

static fn resolve(uint32_t module, const char *name, const char *nid)
{
    const intptr_t address = kernel_dynlib_resolve(getpid(), module, nid);
    if (address == 0 || address == -1)
    {
        say("%s (%s): not resolved\n", name, nid);
        return NULL;
    }
    return (fn)address;
}

/* One call that fills a buffer: dump what it wrote, and keep a copy. */
static long fill(uint32_t module, const char *name, const char *nid, uint8_t *keep)
{
    fn f = resolve(module, name, nid);
    if (f == NULL)
        return -1;
    say("%s(buf) ...\n", name);
    flush();
    memset(g_buf, 0, sizeof(g_buf));
    const long rc = f((long)g_buf, 0, 0, 0);
    say("%s(buf) = 0x%08lx\n", name, (unsigned long)rc & 0xffffffffUL);
    dump();
    flush();
    if (keep != NULL)
        memcpy(keep, g_buf, sizeof(g_buf));
    return rc;
}

int main(void)
{
    become_shellcore();
    int result = 0;
    const int loaded = sceKernelLoadStartModule("/system/common/lib/libSceAvSetting.sprx", 0, NULL, 0, NULL, &result);
    say("load libSceAvSetting = 0x%08x (start result %d)\n", (unsigned)loaded, result);
    uint32_t module = 0;
    say("kernel_dynlib_handle = %d, handle 0x%x\n", kernel_dynlib_handle(getpid(), "libSceAvSetting.sprx", &module),
        module);
    if (module == 0)
        module = (uint32_t)loaded;
    char nid[12] = {0};
    say("nid_encode check: sceAvSettingGetHdmiMonitorInfo -> %s (table says -Mui67TZd4s)\n\n",
        nid_encode("sceAvSettingGetHdmiMonitorInfo", nid));
    flush();

    fn f;
    /* sceAvSettingInit is not called: from a payload it never returns
       (tried 2026-09-15). */
    static uint8_t monitor[65536];
    const long monitor_rc = fill(module, "sceAvSettingGetHdmiMonitorInfo", "-Mui67TZd4s", monitor);
    (void)fill(module, "sceAvSettingGetNativeHdmiMonitorInfo", "b40XbKKKDhQ", NULL);
    (void)fill(module, "sceAvSettingGetCurrentOutputMode_", "ZmPBwBD2tIY", NULL);
    (void)fill(module, "sceAvSettingGetCurrentOutputMode2_", "j-b-RFZ3gjw", NULL);
    (void)fill(module, "sceAvSettingGetCurrentDeviceInfo_", "qnI61-kCm1E", NULL);
    (void)fill(module, "sceAvControlGetCurrentOutputMode", "kECUSNedk3o", NULL);

    if (monitor_rc == 0)
    {
        if ((f = resolve(module, "sceAvControlIsHdrSupportedByMonitorInfo", "5wP7bivaX7c")))
        {
            say("sceAvControlIsHdrSupportedByMonitorInfo(monitor) = 0x%08lx\n",
                (unsigned long)f((long)monitor, 0, 0, 0) & 0xffffffffUL);
            flush();
        }
        if ((f = resolve(module, "sceAvControlIsDeepColorSupportedByMonitorInfo", "fnqFpsRrsg4")))
        {
            say("sceAvControlIsDeepColorSupportedByMonitorInfo(monitor) = 0x%08lx\n",
                (unsigned long)f((long)monitor, 0, 0, 0) & 0xffffffffUL);
            flush();
        }
    }
    if ((f = resolve(module, "sceAvControlIsColorimetryHdr", "3jnRsjnzIjI")))
    {
        say("sceAvControlIsColorimetryHdr(n), n = 0..15:");
        for (long n = 0; n < 16; ++n)
            say(" %ld=%lx", n, (unsigned long)f(n, 0, 0, 0) & 0xff);
        say("\n");
        flush();
    }
    say("\ndone\n");
    flush();
    return 0;
}
