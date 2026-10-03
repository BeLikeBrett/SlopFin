/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "update_package.hpp"
#include "update/sha256.h"
#include "bigalloc.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <cerrno>
#include <sys/stat.h>

/* Match the PS5 application's restricted path-based metadata calls. */
extern "C" int lstat(const char *, struct stat *) noexcept
{
    errno = EPERM;
    return -1;
}
extern "C" int stat(const char *, struct stat *) noexcept
{
    errno = EPERM;
    return -1;
}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../vendor/stb_image.h"
#pragma GCC diagnostic pop
namespace slopfin::bigalloc
{
void *allocate(std::size_t n) noexcept
{
    return std::malloc(n);
}
void release(void *p) noexcept
{
    std::free(p);
}
} // namespace slopfin::bigalloc
std::string hash(std::string_view text)
{
    sf_sha256 s;
    sf_sha_init(&s);
    sf_sha_add(&s, text.data(), text.size());
    char out[65];
    sf_sha_hex(&s, out);
    return out;
}
int main(int argc, char **argv)
{
    using namespace slopfin::update;
    if (argc == 4)
    {
        std::ifstream file(argv[1], std::ios::binary);
        std::ostringstream buf;
        buf << file.rdbuf();
        const auto bytes = buf.str();
        Release r;
        r.version = argv[3];
        r.digest = hash(bytes);
        r.size = bytes.size();
        std::string error;
        bool ok = stage_archive(argv[1], argv[2], r, error);
        if (!ok)
            std::fprintf(stderr, "%s\n", error.c_str());
        return ok ? 0 : 1;
    }
    assert(hash("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(hash("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(hash(std::string(1000000, 'a')) ==
           "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    const auto input = std::string(510, 'x');
    sf_sha256 split;
    sf_sha_init(&split);
    for (char c : input)
        sf_sha_add(&split, &c, 1);
    char output[65];
    sf_sha_hex(&split, output);
    assert(hash(input) == output);
    assert(version_number("01.000.000") == 1000000);
    assert(version_number("02.100.001") > version_number("01.999.999"));
    for (auto bad : {"1.0.0", "01.000.0000", "01.00a.001", "../01.000.001", "00.000.000"})
        assert(!version_number(bad));
    assert(trusted_download("https://release-assets.githubusercontent.com/path?signature=abc"));
    for (auto bad :
         {"http://github.com/file", "https://github.com.evil/file", "https://github.com@evil/file",
          "https://github.com:443/file", "https://github.com/file\r\nOther: x"})
        assert(!trusted_download(bad));
    const std::string asset =
        "{\"name\":\"SlopFin-01.000.001-folder.zip\",\"state\":\"uploaded\",\"size\":2048,"
        "\"browser_download_url\":\"https://github.com/BeLikeBrett/SlopFin/releases/download/"
        "01.000.001/SlopFin-01.000.001-folder.zip\",\"digest\":\"sha256:" +
        std::string(64, 'a') + "\"}";
    auto feed = "[{\"tag_name\":\"01.000.001\",\"draft\":false,\"prerelease\":true,\"assets\":[" +
                asset + "]}]";
    assert(select_release(feed, "01.000.000").version == "01.000.001");
    assert(select_release(feed, "01.000.001").version.empty());
    assert(select_release(feed, "Unknown").version.empty());
    feed.replace(feed.find("\"draft\":false"), 13, "\"draft\":true");
    assert(select_release(feed, "01.000.000").version.empty());
}
