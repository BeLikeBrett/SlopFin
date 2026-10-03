/*
 * SlopFin - launch payload.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * CheatRunner is not always resident, and when it is not there is no way to
 * start the app remotely. This payload does the one thing needed: ask the
 * launcher to start a title by id. Sent over elfldr on port 9021, it makes the
 * development tools self-sufficient.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ps5/kernel.h>

int sceUserServiceInitialize(void *);
void sceUserServiceTerminate(void);
int sceLncUtilLaunchApp(const char *title_id, char **argv, void *options);
int sceLncUtilKillLocalProcess(const char *reason);
int sceSystemServiceLaunchApp(const char *title_id, char **argv, void *options);
int sceLncUtilKillApp(unsigned int app_id);
int sceLncUtilGetAppId(const char *title_id);

__attribute__((constructor)) static void start(void)
{
    sceUserServiceInitialize(0);
}

/*
 * The launcher refuses a request from an ordinary process: both entry points
 * return 0x8094000c until the caller carries ShellCore's credentials. This
 * raises the payload's own credentials only, which is what every homebrew
 * launcher does to start a title.
 */
static int become_shellcore(void)
{
    const pid_t self = getpid();
    static const uint8_t caps[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    static const uint8_t attrs[32] = {0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (kernel_set_ucred_uid(self, 0))
        return -1;
    (void)kernel_set_ucred_ruid(self, 0);
    (void)kernel_set_ucred_svuid(self, 0);
    (void)kernel_set_ucred_caps(self, caps);
    (void)kernel_set_ucred_attrs(self, attrs);
    return kernel_set_ucred_authid(self, 0x4801000000000013UL);
}

int main(int argc, char **argv)
{
    const char *title = argc > 1 ? argv[1] : "PPSA99001";
    printf("escalate = %d, authid now 0x%lx\n", become_shellcore(),
           kernel_get_ucred_authid(getpid()));
    fflush(stdout);

    /* Options is a 1024-byte block in the launcher's ABI; zeroed means the
       defaults, which is what the system menu itself passes. */
    unsigned char options[1024];
    memset(options, 0, sizeof(options));

    /*
     * A crashed instance leaves its process in place, and the launcher then
     * refuses to start the title again with 0x8094000c. So clear any existing
     * instance first; not finding one is the normal case and not an error.
     */
    const int existing = sceLncUtilGetAppId(title);
    if (existing > 0)
    {
        const int killed = sceLncUtilKillApp((unsigned int)existing);
        printf("killed stale app id %d = 0x%x\n", existing, killed);
        fflush(stdout);
        sleep(3);
    }

    int result = sceLncUtilLaunchApp(title, NULL, options);
    printf("sceLncUtilLaunchApp(%s) = 0x%x\n", title, result);
    fflush(stdout);
    if (result < 0)
    {
        memset(options, 0, sizeof(options));
        result = sceSystemServiceLaunchApp(title, NULL, options);
        printf("sceSystemServiceLaunchApp(%s) = 0x%x\n", title, result);
        fflush(stdout);
    }
    return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

__attribute__((destructor)) static void finish(void)
{
    sceUserServiceTerminate();
}
