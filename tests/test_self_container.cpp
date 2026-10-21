/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../tooling/native/self_container.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>

namespace
{
void put(ps5::self::Bytes &data, std::size_t offset, std::uint64_t value, unsigned size)
{
    for (unsigned i = 0; i < size; ++i)
        data[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}
ps5::self::Bytes fixture()
{
    ps5::self::Bytes elf(0x200);
    std::memcpy(elf.data(), "\177ELF\2\1\1", 7);
    put(elf, 0x10, 0xfe10, 2);
    put(elf, 0x12, 62, 2);
    put(elf, 0x20, 0x40, 8);
    put(elf, 0x34, 0x40, 2);
    put(elf, 0x36, 0x38, 2);
    put(elf, 0x38, 2, 2);
    put(elf, 0x40, 1, 4);
    put(elf, 0x44, 5, 4);
    put(elf, 0x48, 0x100, 8);
    put(elf, 0x60, 17, 8);
    put(elf, 0x68, 17, 8);
    std::fill(elf.begin() + 0x100, elf.begin() + 0x111, 0xa5);
    put(elf, 0x78, 0x6fffff01, 4);
    put(elf, 0x80, 0x180, 8);
    put(elf, 0x98, 26, 8);
    put(elf, 0x182, 22, 2);
    std::memcpy(elf.data() + 0x184, "\10test:", 6);
    put(elf, 0x18a, 0x9000002, 8);
    put(elf, 0x192, 0x9000002, 8);
    return elf;
}
} // namespace

int main()
{
    const auto elf = fixture();
    for (const auto magic : {ps5::self::kMagic, ps5::self::kAlternateMagic})
    {
        ps5::self::SignOptions options;
        options.magic = magic;
        auto legacy = ps5::self::sign(elf, options);
        const auto image = ps5::self::parse(legacy);
        std::size_t tail = 0;
        for (const auto &segment : image.segments)
            tail =
                std::max(tail, static_cast<std::size_t>(segment.file_offset + segment.file_size));
        assert(image.file_size > tail && image.file_size - tail < 16);
        const auto expected = ps5::self::extract(legacy);
        assert(std::equal(elf.begin() + 0x180, elf.begin() + 0x19a, expected.begin() + 0x180));
        auto aligned = legacy;
        aligned.insert(aligned.begin() + tail, image.file_size - tail, 0);
        assert(ps5::self::extract(aligned) == expected);
        aligned.resize(image.file_size + 5);
        assert(ps5::self::extract(aligned) != expected);
    }
}
