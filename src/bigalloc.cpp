/*
 * SlopFin - large-buffer allocator backed by flexible memory.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "bigalloc.hpp"

#include <cstdint>
#include <cstring>

extern "C"
{
    int sceKernelMapNamedFlexibleMemory(void **address, std::size_t length, int protection,
                                        int flags, const char *name);
    int sceKernelMunmap(void *address, std::size_t length);
}

namespace
{
constexpr int kProtReadWrite = 0x03;
constexpr std::size_t kPageSize = 16u * 1024u;
/* Every block carries its mapped length so free and realloc know the size. */
constexpr std::size_t kHeaderBytes = kPageSize;

struct Header
{
    std::size_t mapped;
    std::size_t requested;
};

std::size_t round_up(std::size_t value, std::size_t multiple) noexcept
{
    return (value + multiple - 1u) & ~(multiple - 1u);
}
} // namespace

namespace slopfin::bigalloc
{

void *allocate(std::size_t bytes) noexcept
{
    if (bytes == 0)
        bytes = 1;
    const std::size_t mapped = round_up(bytes + kHeaderBytes, kPageSize);
    void *base = nullptr;
    if (sceKernelMapNamedFlexibleMemory(&base, mapped, kProtReadWrite, 0, "slopfin") < 0 ||
        base == nullptr)
        return nullptr;

    auto *header = static_cast<Header *>(base);
    header->mapped = mapped;
    header->requested = bytes;
    return static_cast<std::uint8_t *>(base) + kHeaderBytes;
}

void *reallocate(void *pointer, std::size_t bytes) noexcept
{
    if (pointer == nullptr)
        return allocate(bytes);
    auto *base = static_cast<std::uint8_t *>(pointer) - kHeaderBytes;
    const Header previous = *reinterpret_cast<Header *>(base);
    if (bytes <= previous.mapped - kHeaderBytes)
    {
        reinterpret_cast<Header *>(base)->requested = bytes;
        return pointer;
    }

    void *fresh = allocate(bytes);
    if (fresh == nullptr)
        return nullptr;
    std::memcpy(fresh, pointer, previous.requested < bytes ? previous.requested : bytes);
    release(pointer);
    return fresh;
}

void release(void *pointer) noexcept
{
    if (pointer == nullptr)
        return;
    auto *base = static_cast<std::uint8_t *>(pointer) - kHeaderBytes;
    const std::size_t mapped = reinterpret_cast<Header *>(base)->mapped;
    (void)sceKernelMunmap(base, mapped);
}

} // namespace slopfin::bigalloc
