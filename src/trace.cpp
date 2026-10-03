/*
 * SlopFin - startup trace.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "trace.hpp"

#include <cstdio>

extern "C"
{
    int open(const char *path, int flags, ...);
    long write(int descriptor, const void *buffer, unsigned long bytes);
    int close(int descriptor);
}

namespace
{
constexpr int kWriteOnly = 0x0001;
constexpr int kCreate = 0x0200;
constexpr int kTruncate = 0x0400;
constexpr int kAppend = 0x0008;
#ifdef SLOPFIN_HOST
constexpr const char *kPath = "slopfin-trace.txt";
#else
constexpr const char *kPath = "/data/slopfin-trace.txt";
#endif

/* Opened and closed per line so a hard crash cannot lose buffered output. */
void append(std::string_view text, bool truncate) noexcept
{
    const int flags = kWriteOnly | kCreate | (truncate ? kTruncate : kAppend);
    const int descriptor = open(kPath, flags, 0666);
    if (descriptor < 0)
        return;
    (void)write(descriptor, text.data(), text.size());
    (void)write(descriptor, "\n", 1);
    (void)close(descriptor);
}
} // namespace

namespace slopfin::trace
{

void begin() noexcept
{
    /* Record the build stamp so captures can be matched to the running binary. */
    append("main entered, built " __DATE__ " " __TIME__, true);
}

void mark(std::string_view stage) noexcept
{
    append(stage, false);
}

} // namespace slopfin::trace
