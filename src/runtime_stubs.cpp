/*
 * SlopFin - runtime symbols the SDK's C++ archives expect.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * libcxxabi and libunwind reference unwind-table bounds that a normal crt
 * would supply, plus libc's assert hook. The boilerplate's minimal crt does
 * not define them. Empty ranges are correct here: nothing in this app throws
 * across a frame that needs table-driven unwinding.
 *
 * Every symbol here is hidden: the FSELF converter accepts only undefined
 * dynamic symbols, so anything the app defines must stay out of the dynamic
 * table.
 */

#include <cstdlib>

extern "C"
{
#define SLOPFIN_INTERNAL __attribute__((visibility("hidden")))

    SLOPFIN_INTERNAL void *__dso_handle = nullptr;

    SLOPFIN_INTERNAL char __eh_frame_start[1] = {};
    SLOPFIN_INTERNAL char __eh_frame_end[1] = {};
    SLOPFIN_INTERNAL char __eh_frame_hdr_start[1] = {};
    SLOPFIN_INTERNAL char __eh_frame_hdr_end[1] = {};

    SLOPFIN_INTERNAL void __assert(const char *, const char *, int, const char *)
    {
        std::abort();
    }
}
