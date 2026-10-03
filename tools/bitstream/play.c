/*
 * SlopFin dev tooling - send a compressed audio bitstream to HDMI.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Plays an IEC 61937 file (ffmpeg -c copy -f spdif) through libSceAudioOut's
 * "Ex" AV-playback port, the path the console's disc player uses, and logs
 * every return code and the output information before, during and after to
 * /data/slopfin-bitstream.txt.
 *
 * Built per experiment: MODE is the Ex mode (0 AC-3, 2 DTS, 3 E-AC-3,
 * 4 and 10 the 192 kHz eight-channel types), DEVICE the configure byte
 * (1..3, or 255), FILE the stream on the console, CHANNELS the words per
 * frame the file is written in.
 */

#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ps5/kernel.h>

int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(int *);
int sceAudioOutInit(void);
int sceAudioOutExConfigureOutput(int zero, long unused, int mode, int device, long unused2);
int sceAudioOutExOpen(int user, int mode);
int sceAudioOutExClose(int handle);
int sceAudioOutOutput(int handle, const void *buffer);
int sceAudioOutExGetOutputInfo(int zero, long unused, void *buffer, int size);

#ifndef MODE
#define MODE 0
#endif
#ifndef DEVICE
#define DEVICE 255
#endif
#ifndef FILE_PATH
#define FILE_PATH "/data/slopfin-bitstream.spdif"
#endif
#ifndef CHANNELS
#define CHANNELS 2
#endif
#ifndef GRAIN
#define GRAIN 256
#endif

static char g_text[16384];
static int g_used;

static void say(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    g_used += vsnprintf(g_text + g_used, sizeof(g_text) - (size_t)g_used, format, args);
    va_end(args);
    const int fd = open("/data/slopfin-bitstream.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        (void)write(fd, g_text, (size_t)g_used);
        (void)close(fd);
    }
}

static void output_info(const char *when)
{
    uint8_t info[24] = {0};
    const int rc = sceAudioOutExGetOutputInfo(0, 0, info, sizeof(info));
    say("output info %-8s rc=0x%08x:", when, (unsigned)rc);
    for (size_t i = 0; i < sizeof(info); ++i)
        say(" %02x", info[i]);
    say("\n");
}

#ifndef AUTHID
#define AUTHID 0x4801000000000013UL /* ShellCore */
#endif

static void escalate(void)
{
    const pid_t self = getpid();
    static const uint8_t caps[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    static const uint8_t attrs[32] = {0x80};
    (void)kernel_set_ucred_uid(self, 0);
    (void)kernel_set_ucred_ruid(self, 0);
    (void)kernel_set_ucred_svuid(self, 0);
    (void)kernel_set_ucred_caps(self, caps);
    (void)kernel_set_ucred_attrs(self, attrs);
    (void)kernel_set_ucred_authid(self, AUTHID);
}

int main(void)
{
    escalate();
    say("mode %d device %d file %s channels %d grain %d authid 0x%lx\n", MODE, DEVICE, FILE_PATH, CHANNELS, GRAIN, kernel_get_ucred_authid(getpid()));
    (void)sceUserServiceInitialize(NULL);
    int user = 0;
    (void)sceUserServiceGetForegroundUser(&user);
    say("init 0x%08x user 0x%x\n", (unsigned)sceAudioOutInit(), (unsigned)user);

    const int fd = open(FILE_PATH, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0)
    {
        say("cannot open %s\n", FILE_PATH);
        return 1;
    }
    const size_t frame_bytes = (size_t)(CHANNELS * 2);
    const size_t chunk = frame_bytes * (size_t)GRAIN;
    uint8_t *data = malloc((size_t)st.st_size);
    size_t have = 0;
    while (data != NULL && have < (size_t)st.st_size)
    {
        const ssize_t got = read(fd, data + have, (size_t)st.st_size - have);
        if (got <= 0)
            break;
        have += (size_t)got;
    }
    (void)close(fd);
    say("read %zu bytes\n", have);

    output_info("before");
    say("configure mode %d = 0x%08x\n", MODE, (unsigned)sceAudioOutExConfigureOutput(0, 0, MODE, DEVICE, 0));
    output_info("config");
    const int handle = sceAudioOutExOpen(user, MODE);
    say("ExOpen = 0x%08x\n", (unsigned)handle);
    output_info("opened");

    if (handle >= 0 && data != NULL)
    {
        int errors = 0, first_error = 0;
        size_t chunks = 0;
        for (size_t at = 0; at + chunk <= have; at += chunk, ++chunks)
        {
            const int rc = sceAudioOutOutput(handle, data + at);
            if (rc < 0)
            {
                if (errors++ == 0)
                    first_error = rc;
                if (errors > 50)
                    break;
            }
            if (chunks == 400)
                output_info("playing");
        }
        say("output %zu chunks, %d errors (first 0x%08x)\n", chunks, errors, (unsigned)first_error);
        say("ExClose = 0x%08x\n", (unsigned)sceAudioOutExClose(handle));
    }
    say("restore = 0x%08x\n", (unsigned)sceAudioOutExConfigureOutput(0, 0, 255, 255, 0));
    output_info("after");
    say("done\n");
    return 0;
}
