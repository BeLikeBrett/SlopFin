/*
 * SlopFin dev tooling - pair a Remote Play client without looking at the TV.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Headless version of ps5-payload-dev/linkdev (GPL-3.0-or-later, John
 * Törnblom): the same system calls, but the account id and PIN are written to
 * /data/slopfin-rp-pair.txt instead of drawn, so a script on the PC can read
 * them over FTP and complete registration. Sent over elfldr. It waits up to
 * five minutes for a client to register, appending each state it sees.
 *
 */

#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(int *);
int sceRemoteplayInitialize(void *, size_t);
int sceRemoteplayGeneratePinCode(uint32_t *);
int sceRemoteplayConfirmDeviceRegist(int *, int *);
int sceRemoteplayNotifyPinCodeError(int);
int sceRegMgrGetInt(long, int *);
int sceRegMgrSetInt(long, int);
int sceRegMgrGetBin(long, void *, int);

#define REMOTEPLAY_rp_enable 1098973184L
#define USER_user_id(n) ((long)((n) - 1) * 65536L + 125829376L)
#define USER_account_id(n) ((long)((n) - 1) * 65536L + 125830400L)
#define OUT "/data/slopfin-rp-pair.txt"

static void note(const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    const int fd = open(OUT, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0)
    {
        (void)write(fd, line, (size_t)length);
        (void)close(fd);
    }
}

/* The PSN account id as Remote Play clients expect it: its eight bytes in base64. */
static void encode(uint64_t input, char *output)
{
    static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t *d = (const uint8_t *)&input;
    output[0] = charset[(d[0] >> 2) & 0x3F];
    output[1] = charset[((d[0] & 0x03) << 4) | ((d[1] >> 4) & 0x0F)];
    output[2] = charset[((d[1] & 0x0F) << 2) | ((d[2] >> 6) & 0x03)];
    output[3] = charset[d[2] & 0x3F];
    output[4] = charset[(d[3] >> 2) & 0x3F];
    output[5] = charset[((d[3] & 0x03) << 4) | ((d[4] >> 4) & 0x0F)];
    output[6] = charset[((d[4] & 0x0F) << 2) | ((d[5] >> 6) & 0x03)];
    output[7] = charset[d[5] & 0x3F];
    output[8] = charset[(d[6] >> 2) & 0x3F];
    output[9] = charset[((d[6] & 0x03) << 4) | ((d[7] >> 4) & 0x0F)];
    output[10] = charset[(d[7] & 0x0F) << 2];
    output[11] = '=';
    output[12] = '\0';
}

int main(void)
{
    (void)unlink(OUT);
    (void)sceUserServiceInitialize(NULL);
    note("rp init = 0x%x\n", (unsigned)sceRemoteplayInitialize(NULL, 0));

    int enabled = -1;
    (void)sceRegMgrGetInt(REMOTEPLAY_rp_enable, &enabled);
    if (enabled != 1)
        note("rp_enable was %d, set = 0x%x\n", enabled, (unsigned)sceRegMgrSetInt(REMOTEPLAY_rp_enable, 1));

    int user = 0;
    int err = sceUserServiceGetForegroundUser(&user);
    note("foreground user = 0x%x (err 0x%x)\n", (unsigned)user, (unsigned)err);
    int slot = 0;
    for (int i = 1; i <= 16; ++i)
    {
        int uid = 0;
        if (sceRegMgrGetInt(USER_user_id(i), &uid) == 0 && uid == user)
        {
            slot = i;
            break;
        }
    }
    uint64_t account = 0;
    char id[16] = {0};
    if (slot == 0 || sceRegMgrGetBin(USER_account_id(slot), &account, 8) != 0)
    {
        note("error: no account id for the foreground user\n");
        return 1;
    }
    encode(account, id);
    note("account_id %s\n", id);

    (void)sceRemoteplayNotifyPinCodeError(1);
    uint32_t pin = 0;
    if ((err = sceRemoteplayGeneratePinCode(&pin)) != 0)
    {
        note("error: generate pin 0x%x\n", (unsigned)err);
        return 1;
    }
    note("pin %08u\n", pin);

    int last = -1;
    const time_t end = time(NULL) + 300;
    while (time(NULL) < end)
    {
        int status = 0, pair_err = 0;
        err = sceRemoteplayConfirmDeviceRegist(&status, &pair_err);
        if (err != 0 || status != last)
        {
            note("state %d err 0x%x call 0x%x\n", status, (unsigned)pair_err, (unsigned)err);
            last = status;
        }
        if (err != 0 || status == 2 || status == 3)
            break;
        sleep(1);
    }
    note("done\n");
    return 0;
}
