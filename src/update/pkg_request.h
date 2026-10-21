/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The package-update request SlopFin hands its helper: which process asked,
 * for which installed title, and what the downloaded package must be. Shared
 * by the app, which writes it, and the helper, which trusts nothing else.
 */
#ifndef SLOPFIN_UPDATE_PKG_REQUEST_H
#define SLOPFIN_UPDATE_PKG_REQUEST_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SF_PKG_REQUEST_MAGIC "SLOPFIN-PKG-UPDATE-1\n"
#define SF_PKG_REQUEST_MAX 512

typedef struct
{
    long pid;
    char title[10];   /* PPSA plus five digits */
    char sha256[65];  /* lowercase hex */
    uint64_t size;    /* bytes */
    char version[11]; /* NN.NNN.NNN */
} sf_pkg_request;

static inline int sf_pkg_digits(const char *text, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        if (text[i] < '0' || text[i] > '9')
            return 0;
    return 1;
}

static inline int sf_pkg_title_valid(const char *title)
{
    return strlen(title) == 9 && !memcmp(title, "PPSA", 4) && sf_pkg_digits(title + 4, 5);
}

static inline int sf_pkg_version_valid(const char *version)
{
    return strlen(version) == 10 && version[2] == '.' && version[6] == '.' &&
           sf_pkg_digits(version, 2) && sf_pkg_digits(version + 3, 3) &&
           sf_pkg_digits(version + 7, 3);
}

static inline int sf_pkg_sha_valid(const char *hex)
{
    if (strlen(hex) != 64)
        return 0;
    for (size_t i = 0; i < 64; ++i)
        if (!((hex[i] >= '0' && hex[i] <= '9') || (hex[i] >= 'a' && hex[i] <= 'f')))
            return 0;
    return 1;
}

/* Writes the request text; returns its length, or -1 for invalid fields. */
static inline int sf_pkg_request_format(const sf_pkg_request *r, char *out, size_t cap)
{
    if (r->pid <= 0 || r->pid > 0x7fffffffL || !sf_pkg_title_valid(r->title) ||
        !sf_pkg_sha_valid(r->sha256) || !sf_pkg_version_valid(r->version) || r->size < 1024 ||
        r->size > (uint64_t)1 << 32)
        return -1;
    const int n = snprintf(out, cap, "%s%ld\n%s\n%s\n%llu\n%s\n", SF_PKG_REQUEST_MAGIC, r->pid,
                           r->title, r->sha256, (unsigned long long)r->size, r->version);
    return n > 0 && (size_t)n < cap ? n : -1;
}

/* Reads exactly the format above, one field per line, nothing else. */
static inline int sf_pkg_request_parse(const char *text, size_t length, sf_pkg_request *out)
{
    const size_t magic = sizeof(SF_PKG_REQUEST_MAGIC) - 1;
    if (length <= magic || length >= SF_PKG_REQUEST_MAX || memchr(text, 0, length) ||
        memcmp(text, SF_PKG_REQUEST_MAGIC, magic) || text[length - 1] != '\n')
        return -1;
    const char *at = text + magic, *end = text + length;
    char field[5][72];
    for (int i = 0; i < 5; ++i)
    {
        const char *newline = (const char *)memchr(at, '\n', (size_t)(end - at));
        if (!newline || newline == at || (size_t)(newline - at) >= sizeof(field[i]))
            return -1;
        memcpy(field[i], at, (size_t)(newline - at));
        field[i][newline - at] = 0;
        at = newline + 1;
    }
    if (at != end || !sf_pkg_digits(field[0], strlen(field[0])) ||
        !sf_pkg_digits(field[3], strlen(field[3])) || strlen(field[0]) > 10 ||
        strlen(field[3]) > 11)
        return -1;
    sf_pkg_request r;
    memset(&r, 0, sizeof(r));
    r.pid = strtol(field[0], NULL, 10);
    r.size = strtoull(field[3], NULL, 10);
    if (strlen(field[1]) >= sizeof(r.title) || strlen(field[2]) >= sizeof(r.sha256) ||
        strlen(field[4]) >= sizeof(r.version))
        return -1;
    memcpy(r.title, field[1], strlen(field[1]) + 1);
    memcpy(r.sha256, field[2], strlen(field[2]) + 1);
    memcpy(r.version, field[4], strlen(field[4]) + 1);
    char check[SF_PKG_REQUEST_MAX];
    if (sf_pkg_request_format(&r, check, sizeof(check)) != (int)length ||
        memcmp(check, text, length))
        return -1; /* rejects leading zeros and anything not written by format() */
    *out = r;
    return 0;
}

/*
 * The content ID inside a finalized package: its CNT container starts on a
 * 64 KiB boundary and carries the ID at +0x40. Returns 0 when `prefix`
 * (e.g. "UP9000-PPSA99001_00-") begins it.
 */
static inline int sf_pkg_content_matches(const unsigned char *cnt, size_t length,
                                         const char *prefix)
{
    const size_t n = strlen(prefix);
    return length >= 0x40 + n &&
                   !memcmp(cnt,
                           "\x7f"
                           "CNT",
                           4) &&
                   !memcmp(cnt + 0x40, prefix, n)
               ? 0
               : -1;
}

#endif
