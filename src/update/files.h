/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_UPDATE_FILES_H
#define SLOPFIN_UPDATE_FILES_H
#include "sha256.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

static inline int sf_update_path(const char *p)
{
    if (!p || !*p || strlen(p) > 200 || p[0] == '/' || strstr(p, "..") || strstr(p, "//"))
        return 0;
    for (const char *c = p; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
              *c == '/' || *c == '-' || *c == '_' || *c == '.'))
            return 0;
    if (!strcmp(p, "eboot.bin") || !strcmp(p, "LICENSE") || !strcmp(p, "NOTICE.md"))
        return 1;
    return !strncmp(p, "assets/", 7) || !strncmp(p, "sce_sys/", 8) ||
           !strncmp(p, "sce_module/", 11);
}
static inline int sf_update_dirs(const char *path)
{
    char tmp[512];
    if (strlen(path) >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, strlen(path) + 1);
    for (char *p = tmp + 1; *p; ++p)
        if (*p == '/')
        {
            *p = 0;
            /* The PS5 app sandbox denies lstat; metadata on opened files is available. */
            int fd = open(tmp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            if (fd < 0 && errno == ENOENT)
            {
                if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
                    return -1;
                fd = open(tmp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            }
            if (fd < 0)
                return -1;
            struct stat s;
            const int ok = fstat(fd, &s) == 0 && S_ISDIR(s.st_mode);
            close(fd);
            if (!ok)
                return -1;
            *p = '/';
        }
    return 0;
}
static inline int sf_update_hash_file(const char *path, char hex[65])
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        st.st_size > 64 * 1024 * 1024)
    {
        close(fd);
        return -1;
    }
    sf_sha256 s;
    sf_sha_init(&s);
    unsigned char buf[32768];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        sf_sha_add(&s, buf, (size_t)n);
    close(fd);
    if (n < 0)
        return -1;
    sf_sha_hex(&s, hex);
    return 0;
}
static inline int sf_update_copy(const char *from, const char *to)
{
    if (sf_update_dirs(to) < 0)
        return -1;
    int in = open(from, O_RDONLY | O_NOFOLLOW);
    if (in < 0)
        return -1;
    struct stat st;
    if (fstat(in, &st) < 0 || !S_ISREG(st.st_mode))
    {
        close(in);
        return -1;
    }
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (out < 0)
    {
        close(in);
        return -1;
    }
    unsigned char buf[32768];
    ssize_t n;
    int ok = 1;
    while ((n = read(in, buf, sizeof(buf))) > 0)
    {
        ssize_t have = 0;
        while (have < n)
        {
            ssize_t w = write(out, buf + have, (size_t)(n - have));
            if (w <= 0)
            {
                ok = 0;
                break;
            }
            have += w;
        }
        if (!ok)
            break;
    }
    if (n < 0 || fchmod(out, st.st_mode & 0777) < 0 || fsync(out) < 0)
        ok = 0;
    close(in);
    if (close(out) < 0)
        ok = 0;
    return ok ? 0 : -1;
}
#endif
