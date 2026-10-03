/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_UPDATE_TRANSACTION_H
#define SLOPFIN_UPDATE_TRANSACTION_H
#include "files.h"
#include <stdlib.h>

typedef struct
{
    char hash[65], path[201];
    int existed;
} sf_update_entry;
typedef struct
{
    sf_update_entry entries[256];
    unsigned count;
    char stage[256], target[256], backup[256];
} sf_update_transaction;
static inline int sf_update_join(char out[512], const char *base, const char *path)
{
    return snprintf(out, 512, "%s/%s", base, path) >= 512 ? -1 : 0;
}
static inline int sf_update_prepare(sf_update_transaction *t, const char *stage, const char *target,
                                    const char *backup)
{
    memset(t, 0, sizeof(*t));
    if (strlen(stage) > 255 || strlen(target) > 255 || strlen(backup) > 255)
        return -1;
    memcpy(t->stage, stage, strlen(stage) + 1);
    memcpy(t->target, target, strlen(target) + 1);
    memcpy(t->backup, backup, strlen(backup) + 1);
    char file[512];
    sf_update_join(file, stage, "manifest");
    int fd = open(file, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    char text[70000];
    ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0 || n == (ssize_t)sizeof(text) - 1)
        return -1;
    text[n] = 0;
    char *cursor = text;
    int eboot = 0, param = 0, runtime = 0;
    while (*cursor)
    {
        char *end = strchr(cursor, '\n');
        if (!end || end - cursor < 66 || end - cursor > 266 || t->count == 256)
            return -1;
        *end = 0;
        if (cursor[64] != ' ')
            return -1;
        for (unsigned i = 0; i < 64; ++i)
            if (!((cursor[i] >= '0' && cursor[i] <= '9') || (cursor[i] >= 'a' && cursor[i] <= 'f')))
                return -1;
        if (!sf_update_path(cursor + 65))
            return -1;
        for (unsigned i = 0; i < t->count; ++i)
            if (!strcmp(t->entries[i].path, cursor + 65))
                return -1;
        sf_update_entry *e = &t->entries[t->count++];
        memcpy(e->hash, cursor, 64);
        memcpy(e->path, cursor + 65, strlen(cursor + 65) + 1);
        eboot |= !strcmp(e->path, "eboot.bin");
        param |= !strcmp(e->path, "sce_sys/param.json");
        runtime |= !strcmp(e->path, "sce_module/libc.prx");
        sf_update_join(file, stage, e->path);
        char hash[65];
        if (sf_update_dirs(file) < 0 || sf_update_hash_file(file, hash) < 0 ||
            strcmp(e->hash, hash))
            return -1;
        cursor = end + 1;
    }
    if (!eboot || !param || !runtime)
        return -1;
    /* Keep the executable and version metadata until all other files are installed. */
    for (unsigned i = 0; i < t->count; ++i)
        for (unsigned j = i + 1; j < t->count; ++j)
        {
            int a = !strcmp(t->entries[i].path, "eboot.bin")            ? 1
                    : !strcmp(t->entries[i].path, "sce_sys/param.json") ? 2
                                                                        : 0;
            int b = !strcmp(t->entries[j].path, "eboot.bin")            ? 1
                    : !strcmp(t->entries[j].path, "sce_sys/param.json") ? 2
                                                                        : 0;
            if (a > b)
            {
                sf_update_entry temp = t->entries[i];
                t->entries[i] = t->entries[j];
                t->entries[j] = temp;
            }
        }
    for (unsigned i = 0; i < t->count; ++i)
    {
        sf_update_entry *e = &t->entries[i];
        sf_update_join(file, target, e->path);
        if (sf_update_dirs(file) < 0)
            return -1;
        struct stat st;
        if (lstat(file, &st) == 0)
        {
            if (!S_ISREG(st.st_mode))
                return -1;
            e->existed = 1;
            char saved[512];
            sf_update_join(saved, backup, e->path);
            if (sf_update_copy(file, saved) < 0)
                return -1;
            char before[65], after[65];
            if (sf_update_hash_file(file, before) < 0 || sf_update_hash_file(saved, after) < 0 ||
                strcmp(before, after))
                return -1;
        }
        else if (errno != ENOENT)
            return -1;
    }
    return 0;
}
/* fail_after is a host-test fault injection; production passes -1. */
static inline int sf_update_apply(sf_update_transaction *t, int fail_after)
{
    unsigned applied = 0;
    for (; applied < t->count; ++applied)
    {
        sf_update_entry *e = &t->entries[applied];
        char source[512], target[512], temp[512], hash[65];
        sf_update_join(source, t->stage, e->path);
        sf_update_join(target, t->target, e->path);
        if (snprintf(temp, sizeof(temp), "%s.slopfin-new", target) >= (int)sizeof(temp))
            break;
        if ((int)applied == fail_after || sf_update_hash_file(source, hash) < 0 ||
            strcmp(e->hash, hash) || sf_update_copy(source, temp) < 0 ||
            chmod(temp, !strcmp(e->path, "eboot.bin") || !strncmp(e->path, "sce_module/", 11)
                            ? 0755
                            : 0644) < 0 ||
            sf_update_hash_file(temp, hash) < 0 || strcmp(e->hash, hash))
        {
            unlink(temp);
            break;
        }
        if (rename(temp, target) < 0)
        {
            unlink(temp);
            break;
        }
    }
    if (applied == t->count)
    {
        sync();
        return 0;
    }
    int rollback = 0;
    while (applied)
    {
        sf_update_entry *e = &t->entries[--applied];
        char target[512], saved[512], temp[512];
        sf_update_join(target, t->target, e->path);
        sf_update_join(saved, t->backup, e->path);
        snprintf(temp, sizeof(temp), "%s.slopfin-old", target);
        if (e->existed)
        {
            if (sf_update_copy(saved, temp) < 0 || rename(temp, target) < 0)
                rollback = -1;
        }
        else if (unlink(target) < 0)
            rollback = -1;
    }
    sync();
    return rollback ? -2 : -1;
}
#endif
