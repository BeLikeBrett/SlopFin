/*
 * SlopFin dev tooling - what the console knows about HDMI audio output.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Read-only. Dumps libSceAudioOut's "Ex" (AV playback) monitor and output
 * information and the AUDIOOUT registry entries to /data/slopfin-audio-probe.txt.
 * Argument shapes come from disassembling the console's own libSceAudioOut:
 * ExGetMonitorInfo requires (0, _, buffer, 0x400), ExGetOutputInfo (0, _,
 * buffer, 0x18).
 */

#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int sceAudioOutInit(void);
int sceAudioOutExGetMonitorInfo(int zero, long unused, void *buffer, int size);
int sceAudioOutExGetOutputInfo(int zero, long unused, void *buffer, int size);
int sceAudioOutSysGetHdmiMonitorInfo(void *buffer);
int sceAudioOutSysGetMonitorInfo(void *buffer);
int sceAudioOutSysGetOutputInfo(void *buffer);
int sceAudioOutSysGetSystemInfo(void *buffer);
int sceRegMgrGetInt(long, int *);

static char g_text[65536];
static int g_used;
static uint8_t g_buf[16384];

static void say(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    g_used += vsnprintf(g_text + g_used, sizeof(g_text) - (size_t)g_used, format, args);
    va_end(args);
}

static void flush(void)
{
    const int fd = open("/data/slopfin-audio-probe.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        (void)write(fd, g_text, (size_t)g_used);
        (void)close(fd);
    }
}

static void dump(const char *name, int rc)
{
    size_t written = 0;
    for (size_t i = 0; i < sizeof(g_buf); ++i)
        if (g_buf[i] != 0)
            written = i + 1;
    say("%s = 0x%08x, %zu bytes\n", name, (unsigned)rc, written);
    for (size_t i = 0; i < written && i < 1024; i += 16)
    {
        say("  %04zx ", i);
        for (size_t j = i; j < i + 16 && j < written; ++j)
            say("%02x ", g_buf[j]);
        say("\n");
    }
    flush();
}

int main(void)
{
    static const struct
    {
        const char *name;
        long key;
    } keys[] = {
        {"mode", 184614912},          {"headphone_out", 184745984}, {"connector_type", 184942592},
        {"codec", 185008128},         {"sound_format", 185073664},  {"config_options", 185139200},
        {"speaker_setting", 185270272}, {"speaker_layout", 185335808}, {"speaker_type", 185401344},
        {"virtual_surround_hdmi", 185466880},
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
    {
        int value = -1;
        const int rc = sceRegMgrGetInt(keys[i].key, &value);
        say("AUDIOOUT_%-22s rc=0x%08x %d\n", keys[i].name, (unsigned)rc, value);
    }
    say("\nsceAudioOutInit = 0x%08x\n\n", (unsigned)sceAudioOutInit());
    flush();

    memset(g_buf, 0, sizeof(g_buf));
    dump("sceAudioOutExGetMonitorInfo(0, 0, buf, 0x400)", sceAudioOutExGetMonitorInfo(0, 0, g_buf, 0x400));
    memset(g_buf, 0, sizeof(g_buf));
    dump("sceAudioOutExGetOutputInfo(0, 0, buf, 0x18)", sceAudioOutExGetOutputInfo(0, 0, g_buf, 0x18));
    memset(g_buf, 0, sizeof(g_buf));
    dump("sceAudioOutSysGetOutputInfo(buf)", sceAudioOutSysGetOutputInfo(g_buf));
    memset(g_buf, 0, sizeof(g_buf));
    dump("sceAudioOutSysGetSystemInfo(buf)", sceAudioOutSysGetSystemInfo(g_buf));
    memset(g_buf, 0, sizeof(g_buf));
    dump("sceAudioOutSysGetMonitorInfo(buf)", sceAudioOutSysGetMonitorInfo(g_buf));
    memset(g_buf, 0, sizeof(g_buf));
    dump("sceAudioOutSysGetHdmiMonitorInfo(buf)", sceAudioOutSysGetHdmiMonitorInfo(g_buf));
    say("done\n");
    flush();
    return 0;
}
