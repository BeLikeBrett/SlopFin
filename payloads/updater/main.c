/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../src/update/transaction.h"
#include <ps5/kernel.h>
#include <signal.h>
#include <time.h>
int sceUserServiceInitialize(void *);
void sceUserServiceTerminate(void);
int sceLncUtilLaunchApp(const char *, char **, void *);
int sceSystemServiceLaunchApp(const char *, char **, void *);
int sceLncUtilGetAppId(const char *);
static const char *base = "/data/slopfin/update";
static void result(const char *message)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/result", base);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (fd >= 0)
    {
        write(fd, message, strlen(message));
        fsync(fd);
        close(fd);
    }
}
static int become_shellcore(void)
{
    const pid_t self = getpid();
    static const uint8_t caps[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    static const uint8_t attrs[32] = {0x80};
    if (kernel_set_ucred_uid(self, 0) || kernel_set_ucred_ruid(self, 0) ||
        kernel_set_ucred_svuid(self, 0) || kernel_set_ucred_caps(self, caps) ||
        kernel_set_ucred_attrs(self, attrs))
        return -1;
    return kernel_set_ucred_authid(self, 0x4801000000000013UL);
}
int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    char lock[512];
    snprintf(lock, sizeof(lock), "%s/install.lock", base);
    int lockfd = open(lock, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (lockfd < 0)
        return 1;
    close(lockfd);
    if (become_shellcore() < 0)
    {
        result("Could not start the update helper. Your app was not changed.");
        unlink(lock);
        return 1;
    }
    const intptr_t root = kernel_get_root_vnode();
    if (kernel_set_proc_rootdir(getpid(), root) || kernel_set_proc_jaildir(getpid(), root))
    {
        result("Could not access the app folder. Your app was not changed.");
        unlink(lock);
        return 1;
    }
    char source[131] = {0}, installed[65], metadata[65];
    int identity = open("/data/slopfin/update/source", O_RDONLY | O_NOFOLLOW);
    ssize_t identity_size = identity < 0 ? -1 : read(identity, source, sizeof(source) - 1);
    if (identity >= 0)
        close(identity);
    source[64] = 0;
    source[129] = 0;
    if (identity_size != 130 ||
        sf_update_hash_file("/data/homebrew/PPSA99001/eboot.bin", installed) < 0 ||
        sf_update_hash_file("/data/homebrew/PPSA99001/sce_sys/param.json", metadata) < 0 ||
        strcmp(source, installed) || strcmp(source + 65, metadata))
    {
        result("This install does not match the folder build. Use your package installer or "
               "install the release folder.");
        unlink(lock);
        return 1;
    }
    char pid_text[32] = {0};
    int pid_fd = open("/data/slopfin/update/pid", O_RDONLY | O_NOFOLLOW);
    ssize_t pid_size = pid_fd < 0 ? -1 : read(pid_fd, pid_text, sizeof(pid_text) - 1);
    if (pid_fd >= 0)
        close(pid_fd);
    char *pid_end = NULL;
    const long app_pid = strtol(pid_text, &pid_end, 10);
    if (pid_size <= 0 || app_pid <= 0 || app_pid > 0x7fffffff || *pid_end != 0)
    {
        result("Could not identify the running app. No files were changed.");
        unlink(lock);
        return 1;
    }
    sf_update_transaction *t = calloc(1, sizeof(*t));
    if (!t || sf_update_prepare(t, "/data/slopfin/update/staged", "/data/homebrew/PPSA99001",
                                "/data/slopfin/update/backup") < 0)
    {
        result("Could not prepare the update. Check free space and try again. Your app was not "
               "changed.");
        free(t);
        unlink(lock);
        return 1;
    }
    sceUserServiceInitialize(NULL);
    result("ready");
    /* The app closes its decoder and VideoOut before any installed file changes. */
    int stopped = 0;
    for (unsigned i = 0; i < 60; ++i)
    {
        if (kill((pid_t)app_pid, 0) < 0 && errno == ESRCH && sceLncUtilGetAppId("PPSA99001") <= 0)
        {
            stopped = 1;
            break;
        }
        struct timespec wait = {0, 500000000};
        nanosleep(&wait, NULL);
    }
    if (!stopped)
    {
        result("The app did not close. No files were changed. Try again.");
        free(t);
        unlink(lock);
        sceUserServiceTerminate();
        return 1;
    }
    const int applied = sf_update_apply(t, -1);
    free(t);
    if (applied == 0)
        result("Update installed successfully.");
    else if (applied == -1)
        result("The update could not be installed. Your previous version was restored.");
    else
        result(
            "The update and recovery failed. Restore the folder from /data/slopfin/update/backup.");
    unlink(lock);
    if (applied != -2)
    {
        unsigned char options[1024] = {0};
        int launched = sceLncUtilLaunchApp("PPSA99001", NULL, options);
        if (launched < 0)
        {
            memset(options, 0, sizeof(options));
            launched = sceSystemServiceLaunchApp("PPSA99001", NULL, options);
        }
        if (launched < 0)
            result(applied == 0 ? "Update installed. Open SlopFin from your launcher."
                                : "Previous version restored. Open SlopFin from your launcher.");
    }
    sceUserServiceTerminate();
    return applied != 0;
}
