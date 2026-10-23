/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#if defined(SLOPFIN_SOFTWARE_AUDIO) || defined(SLOPFIN_BITMAP_SUBTITLES)
#include "bigalloc.hpp"
#include <cstddef>
extern "C"
{
    // FFmpeg's allocator prefix keeps decoder-owned buffers out of the small heap.
    void *slopfin_av_malloc(std::size_t size)
    {
        if (size > 256u * 1024u * 1024u)
            return nullptr;
        return slopfin::bigalloc::allocate(size);
    }
    void *slopfin_av_realloc(void *p, std::size_t size)
    {
        if (size > 256u * 1024u * 1024u)
            return nullptr;
        if (!size)
        {
            slopfin::bigalloc::release(p);
            return nullptr;
        }
        return slopfin::bigalloc::reallocate(p, size);
    }
    void slopfin_av_free(void *p)
    {
        slopfin::bigalloc::release(p);
    }
}
#endif
