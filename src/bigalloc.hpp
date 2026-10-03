/*
 * SlopFin - large-buffer allocator backed by flexible memory.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The process heap this runtime provides is only a handful of megabytes, which
 * is nowhere near enough for decoded artwork. Flexible memory is plentiful
 * (around 1.7 GiB free), so anything image-sized is mapped directly instead of
 * going through malloc.
 */

#ifndef SLOPFIN_BIGALLOC_HPP
#define SLOPFIN_BIGALLOC_HPP

#include <cstddef>

namespace slopfin::bigalloc
{

void *allocate(std::size_t bytes) noexcept;
void *reallocate(void *pointer, std::size_t bytes) noexcept;
void release(void *pointer) noexcept;

} // namespace slopfin::bigalloc

#endif
