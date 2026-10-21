/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_STORAGE_REQUEST_H
#define SLOPFIN_STORAGE_REQUEST_H
#include <stddef.h>
#include <string.h>

#define SF_STORAGE_REQUEST_PREFIX "SLOPFIN-STORAGE-2\n"
typedef struct
{
    int pid, app_fd, result_fd;
} sf_storage_request;

static inline int sf_storage_parse_request(const char *text, size_t size, sf_storage_request *out)
{
    const size_t prefix = sizeof(SF_STORAGE_REQUEST_PREFIX) - 1;
    if (!text || !out || size <= prefix || size >= 96 ||
        memcmp(text, SF_STORAGE_REQUEST_PREFIX, prefix))
        return -1;
    size_t at = prefix;
    int fields[3];
    for (unsigned i = 0; i < 3; ++i)
    {
        const unsigned limit = i == 0 ? 0x7fffffffU : 1023U;
        unsigned value = 0;
        const size_t begin = at;
        while (at < size && text[at] >= '0' && text[at] <= '9')
        {
            const unsigned digit = (unsigned)(text[at++] - '0');
            if (value > (limit - digit) / 10)
                return -1;
            value = value * 10 + digit;
        }
        if (at == begin || at >= size || text[at++] != '\n' || (i == 0 && value == 0))
            return -1;
        fields[i] = (int)value;
    }
    if (at != size || fields[1] == fields[2])
        return -1;
    out->pid = fields[0];
    out->app_fd = fields[1];
    out->result_fd = fields[2];
    return 0;
}
#endif
