/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "image_cache.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <sys/time.h>

namespace cache = slopfin::image_cache;

static std::vector<unsigned char> picture(std::size_t size, unsigned char seed)
{
    std::vector<unsigned char> bytes(size);
    for (std::size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<unsigned char>(seed + i * 7);
    return bytes;
}

static void set_age(const std::filesystem::path &file, long seconds_ago)
{
    timeval times[2];
    gettimeofday(&times[0], nullptr);
    times[0].tv_sec -= seconds_ago;
    times[1] = times[0];
    assert(utimes(file.c_str(), times) == 0);
}

int main()
{
    namespace fs = std::filesystem;
    char temporary[] = "/tmp/slopfin-image-cache-XXXXXX";
    const char *folder = mkdtemp(temporary);
    assert(folder);
    const fs::path directory = fs::path(folder) / "cache" / "images";
    std::vector<unsigned char> out;

    /* Nothing works before configuration. */
    cache::store("Primary|a|t|300", picture(10, 1).data(), 10);
    assert(!cache::load("Primary|a|t|300", out));

    assert(cache::configure(directory.string(), 1u << 20));
    assert(fs::is_directory(directory));
    assert(!cache::load("Primary|a|t|300", out));

    /* Round trip. */
    const auto first = picture(5000, 3);
    cache::store("Primary|a|t|300", first.data(), first.size());
    assert(cache::load("Primary|a|t|300", out) && out == first);
    assert(cache::held_bytes() == 6 + 2 + 15 + first.size());

    /* A different key is a miss, even one that shares a file name. */
    assert(!cache::load("Primary|a|t2|300", out) && out.empty());
    const fs::path stored = directory / cache::detail::name_for("Primary|a|t|300");
    assert(fs::exists(stored));
    fs::copy_file(stored, directory / cache::detail::name_for("Backdrop|b|t|300"));
    assert(!cache::load("Backdrop|b|t|300", out));

    /* Replacing keeps one copy and an exact count. */
    const auto second = picture(3000, 9);
    cache::store("Primary|a|t|300", second.data(), second.size());
    assert(cache::load("Primary|a|t|300", out) && out == second);

    /* Truncated, empty and foreign files are misses. */
    fs::resize_file(stored, 6 + 2 + 15);
    assert(!cache::load("Primary|a|t|300", out));
    fs::resize_file(stored, 3);
    assert(!cache::load("Primary|a|t|300", out));
    {
        std::ofstream junk(directory / cache::detail::name_for("Primary|c|t|300"));
        junk << "not an image cache file";
    }
    assert(!cache::load("Primary|c|t|300", out));

    /* Invalid input is refused without writing. */
    cache::store("", first.data(), first.size());
    cache::store("Primary|d|t|300", nullptr, 10);
    cache::store("Primary|d|t|300", first.data(), 0);
    cache::store(std::string(1025, 'k'), first.data(), first.size());
    assert(!cache::load("Primary|d|t|300", out));

    /* No temporaries are left behind. */
    for (const auto &entry : fs::directory_iterator(directory))
        assert(entry.path().filename().string()[0] != '.');

    /* Over the cap, the oldest files go first, down to three quarters. */
    fs::remove_all(directory);
    assert(cache::configure(directory.string(), 100'000));
    const auto big = picture(20'000, 5);
    for (int i = 0; i < 4; ++i)
    {
        const std::string key = "Primary|" + std::to_string(i) + "|t|300";
        cache::store(key, big.data(), big.size());
        set_age(directory / cache::detail::name_for(key), 100 - i * 10);
    }
    assert(cache::held_bytes() < 100'000);
    cache::store("Primary|4|t|300", big.data(), big.size());
    cache::store("Primary|5|t|300", big.data(), big.size());
    assert(cache::held_bytes() <= 100'000);
    assert(!cache::load("Primary|0|t|300", out));
    assert(!cache::load("Primary|1|t|300", out));
    assert(cache::load("Primary|5|t|300", out) && out == big);

    /* A fresh run counts what an earlier one left. */
    const std::uint64_t held = cache::held_bytes();
    assert(cache::configure(directory.string(), 100'000));
    assert(cache::held_bytes() == held);

    fs::remove_all(folder);
    return 0;
}
