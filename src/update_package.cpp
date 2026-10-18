/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "update_package.hpp"
#include "update/files.h"
#include "bigalloc.hpp"
#include "json.hpp"
#include "../vendor/stb_image.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <memory>
#include <vector>
namespace slopfin::update
{
std::uint64_t version_number(std::string_view t) noexcept
{
    if (t.size() >= 4 && t.size() <= 9 && t[0] >= '1' && t[0] <= '9')
    {
        const auto dot = t.find('.');
        if ((dot == 1 || dot == 2) && t.size() - dot - 1 >= 2 && t.size() - dot - 1 <= 6)
        {
            unsigned major = 0, release = 0;
            for (std::size_t i = 0; i < t.size(); ++i)
            {
                if (i == dot)
                    continue;
                if (t[i] < '0' || t[i] > '9')
                    return 0;
                auto &part = i < dot ? major : release;
                part = part * 10 + static_cast<unsigned>(t[i] - '0');
            }
            return major * 1000000ULL + release;
        }
    }
    if (t.size() != 10 || t[2] != '.' || t[6] != '.')
        return 0;
    std::uint64_t n = 0;
    for (unsigned i = 0; i < t.size(); ++i)
    {
        if (i == 2 || i == 6)
            continue;
        if (t[i] < '0' || t[i] > '9')
            return 0;
        n = n * 10 + static_cast<unsigned>(t[i] - '0');
    }
    return n;
}
std::string display_version(std::string_view text) noexcept
{
    if (!version_number(text))
        return std::string(text);
    const auto number = version_number(text);
    const std::string release = std::to_string(number % 1000000);
    return std::to_string(number / 1000000) + "." + (release.size() < 2 ? "0" : "") + release;
}
bool trusted_download(std::string_view url) noexcept
{
    if (!url.starts_with("https://") || url.find_first_of("\r\n\\") != std::string_view::npos)
        return false;
    const auto slash = url.find('/', 8);
    if (slash == std::string_view::npos)
        return false;
    const auto host = url.substr(8, slash - 8);
    return host == "github.com" || host == "release-assets.githubusercontent.com" ||
           host == "objects.githubusercontent.com";
}
std::string release_notes(std::string_view markdown) noexcept
{
    markdown = markdown.substr(0, 4096);
    std::string result;
    int lines = 0;
    while (!markdown.empty() && lines < 6 && result.size() < 1024)
    {
        const auto end = markdown.find('\n');
        std::string line(markdown.substr(0, end));
        markdown = end == std::string_view::npos ? std::string_view{} : markdown.substr(end + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty() || line.starts_with('#') || line.starts_with("!["))
            continue;
        const bool bullet = line.starts_with("- ") || line.starts_with("* ");
        if (bullet)
            line.erase(0, 2);
        line.erase(std::remove_if(line.begin(), line.end(),
                                  [](char c)
                                  {
                                      return c == '*' || c == '`' ||
                                             (static_cast<unsigned char>(c) < 32 && c != '\t');
                                  }),
                   line.end());
        for (std::size_t at = 0; (at = line.find('[', at)) != std::string::npos;)
        {
            const auto label = line.find("](", at);
            const auto close = label == std::string::npos ? label : line.find(')', label + 2);
            if (close == std::string::npos)
                break;
            line.replace(at, close - at + 1, line.substr(at + 1, label - at - 1));
        }
        const std::string prefix = (result.empty() ? std::string{} : "\n") + (bullet ? "• " : "");
        if (result.size() + prefix.size() >= 1024)
            break;
        result += prefix;
        result += line.substr(0, std::min<std::size_t>(300, 1024 - result.size()));
        ++lines;
    }
    return result;
}

Release select_release(std::string_view feed, std::string_view current) noexcept
{
    Release best;
    auto root = json::parse(feed);
    if (!root || root->type != json::Type::array)
        return best;
    std::uint64_t number = version_number(current);
    if (!number)
        return best;
    for (const auto &r : root->elements)
    {
        const auto tag = r->str("tag_name");
        const auto candidate = version_number(tag);
        if (r->flag("draft", true) || candidate <= number)
            continue;
        const auto *assets = r->find("assets");
        if (!assets || assets->type != json::Type::array)
            continue;
        const std::string name = "SlopFin-" + std::string(tag) + "-folder.zip";
        const std::string expected = "https://github.com/BeLikeBrett/SlopFin/releases/download/" +
                                     std::string(tag) + "/" + name;
        for (const auto &a : assets->elements)
        {
            const auto digest = a->str("digest");
            const auto size = a->num("size");
            if (a->str("name") != name || a->str("browser_download_url") != expected ||
                a->str("state") != "uploaded" || size < 1024 || size > 32 * 1024 * 1024 ||
                size != std::floor(size) || digest.size() != 71 || !digest.starts_with("sha256:"))
                continue;
            if (!std::all_of(digest.begin() + 7, digest.end(), [](char c)
                             { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
                continue;
            char platform[11];
            std::snprintf(platform, sizeof(platform), "%02u.%03u.%03u",
                          static_cast<unsigned>(candidate / 1000000),
                          static_cast<unsigned>(candidate / 1000 % 1000),
                          static_cast<unsigned>(candidate % 1000));
            best = {platform,
                    expected,
                    std::string(digest.substr(7)),
                    static_cast<std::size_t>(size),
                    r->flag("prerelease"),
                    release_notes(r->str("body"))};
            number = candidate;
        }
    }
    return best;
}
namespace
{
struct Buffer
{
    unsigned char *data = nullptr;
    ~Buffer()
    {
        bigalloc::release(data);
    }
};
std::uint16_t u16(const unsigned char *p)
{
    return static_cast<std::uint16_t>(p[0] | p[1] << 8);
}
std::uint32_t u32(const unsigned char *p)
{
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
std::uint32_t crc(const unsigned char *p, std::size_t n)
{
    static const auto table = []
    {
        std::array<std::uint32_t, 256> t{};
        for (unsigned i = 0; i < 256; ++i)
        {
            std::uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0);
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xffffffff;
    for (std::size_t i = 0; i < n; ++i)
        c = table[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}
bool write_file(const std::string &path, const void *data, std::size_t size)
{
    return sf_update_write(path.c_str(), data, size) == 0;
}
struct Entry
{
    std::string path;
    std::uint32_t offset, packed, size, crc;
    std::uint16_t method, flags;
};
} // namespace
bool stage_archive(const std::string &archive, const std::string &stage, const Release &release,
                   std::string &error) noexcept
{
    auto fail = [&](std::string_view message)
    {
        error = message;
        return false;
    };
    const std::string manifest_path = stage + "/manifest";
    if (sf_update_dirs(manifest_path.c_str()) < 0)
        return fail("Could not create the update staging folder.");
    (void)unlink(manifest_path.c_str());
    char hash[65];
    if (sf_update_hash_file(archive.c_str(), hash) < 0 || release.digest != hash)
        return fail("The download did not match GitHub's checksum. Please try again.");
    struct stat st = {};
    int fd = open(archive.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return fail("Could not read the update.");
    if (fstat(fd, &st) < 0 || st.st_size < 22 ||
        static_cast<std::size_t>(st.st_size) != release.size || release.size > 32u * 1024u * 1024u)
    {
        close(fd);
        return fail("The download was incomplete.");
    }
    Buffer zip;
    zip.data = static_cast<unsigned char *>(bigalloc::allocate(release.size));
    if (!zip.data)
    {
        close(fd);
        return fail("Not enough memory to prepare the update.");
    }
    std::size_t have = 0;
    while (have < release.size)
    {
        auto n = read(fd, zip.data + have, release.size - have);
        if (n <= 0)
            break;
        have += static_cast<std::size_t>(n);
    }
    close(fd);
    if (have != release.size)
        return fail("Could not read the update.");
    const unsigned char *p = zip.data;
    std::size_t end = release.size - 22;
    const std::size_t minimum = release.size > 65557 ? release.size - 65557 : 0;
    while (u32(p + end) != 0x06054b50 || end + 22 + u16(p + end + 20) != release.size)
    {
        if (end == minimum)
            return fail("The update archive is invalid.");
        --end;
    }
    const unsigned count = u16(p + end + 10);
    const std::size_t central = u32(p + end + 16), central_size = u32(p + end + 12);
    if (u16(p + end + 4) || u16(p + end + 6) || u16(p + end + 8) != count || !count ||
        count > 256 || central > end || central_size != end - central)
        return fail("Unsupported update archive.");
    std::vector<Entry> entries;
    std::size_t at = central, total = 0;
    const std::string root = "PPSA99001/";
    for (unsigned i = 0; i < count; ++i)
    {
        if (at > end || end - at < 46 || u32(p + at) != 0x02014b50)
            return fail("The update file list is invalid.");
        const unsigned len = u16(p + at + 28), extra = u16(p + at + 30), comment = u16(p + at + 32);
        const std::size_t next = at + 46 + len + extra + comment;
        if (next > end || len == 0 || len > root.size() + 200)
            return fail("The update file list is invalid.");
        std::string name(reinterpret_cast<const char *>(p + at + 46), len);
        const unsigned mode = u32(p + at + 38) >> 16;
        if (!name.starts_with(root) || name.find('\0') != std::string::npos ||
            (mode & 0170000) == 0120000)
            return fail("Unsafe path in the update.");
        std::string path = name.substr(root.size());
        if (!path.empty() && path.back() == '/')
            path.pop_back();
        const bool directory = name.back() == '/';
        if (path.empty())
        {
            if (!directory)
                return fail("Unsafe path in the update.");
            at = next;
            continue;
        }
        if (!sf_update_path(path.c_str()) &&
            !(directory && (path == "assets" || path == "sce_sys" || path == "sce_module")))
            return fail("Unsafe path in the update.");
        if (directory)
        {
            at = next;
            continue;
        }
        Entry e{path,
                u32(p + at + 42),
                u32(p + at + 20),
                u32(p + at + 24),
                u32(p + at + 16),
                u16(p + at + 10),
                u16(p + at + 8)};
        if ((mode & 0170000) != 0 && (mode & 0170000) != 0100000)
            return fail("Unsupported file in the update.");
        if (e.flags & ~0x0808u || (e.method != 0 && e.method != 8) ||
            e.size > 64u * 1024u * 1024u || e.packed > release.size || u16(p + at + 34) != 0)
            return fail("Unsupported file in the update.");
        total += e.size;
        if (total > 128u * 1024u * 1024u)
            return fail("The update is too large.");
        for (const auto &old : entries)
            if (old.path == path)
                return fail("Duplicate file in the update.");
        entries.push_back(e);
        at = next;
    }
    if (at != end)
        return fail("The update file list is invalid.");
    std::string manifest;
    bool executable = false, metadata = false, runtime = false;
    for (const auto &e : entries)
    {
        const std::size_t local = e.offset;
        if (local > central || central - local < 30 || u32(p + local) != 0x04034b50 ||
            u16(p + local + 6) != e.flags || u16(p + local + 8) != e.method)
            return fail("The update file header is invalid.");
        const unsigned len = u16(p + local + 26), extra = u16(p + local + 28);
        const std::size_t start = local + 30 + len + extra;
        if (start > central || e.packed > central - start || len != root.size() + e.path.size() ||
            std::string_view(reinterpret_cast<const char *>(p + local + 30), len) != root + e.path)
            return fail("The update file header is invalid.");
        Buffer file;
        file.data =
            static_cast<unsigned char *>(bigalloc::allocate(std::max<std::size_t>(e.size, 1)));
        if (!file.data)
            return fail("Not enough memory to unpack the update.");
        if (e.method == 0)
        {
            if (e.packed != e.size)
                return fail("Invalid stored update file.");
            std::memcpy(file.data, p + start, e.size);
        }
        else if (stbi_zlib_decode_noheader_buffer(
                     reinterpret_cast<char *>(file.data), static_cast<int>(e.size),
                     reinterpret_cast<const char *>(p + start),
                     static_cast<int>(e.packed)) != static_cast<int>(e.size))
            return fail("Could not unpack the update.");
        if (crc(file.data, e.size) != e.crc)
            return fail("A file in the update is damaged.");
        if (e.path == "sce_sys/param.json")
        {
            auto param = json::parse(std::string_view(reinterpret_cast<char *>(file.data), e.size));
            if (!param || param->str("titleId") != "PPSA99001" ||
                param->str("contentVersion") != release.version)
                return fail("This update does not match SlopFin.");
            metadata = true;
        }
        if (e.path == "eboot.bin")
            executable = e.size > 1024;
        if (e.path == "sce_module/libc.prx")
            runtime = e.size > 1024;
        if (!write_file(stage + "/" + e.path, file.data, e.size))
            return fail(errno == ENOSPC ? "Not enough free space to unpack the update."
                                        : "Could not write update file " + e.path + " (error " +
                                              std::to_string(errno) + "). Download again.");
        sf_sha256 s;
        sf_sha_init(&s);
        sf_sha_add(&s, file.data, e.size);
        sf_sha_hex(&s, hash);
        manifest += std::string(hash) + " " + e.path + "\n";
    }
    static constexpr const char *required[] = {"assets/font-regular.ttf", "assets/font-medium.ttf",
                                               "assets/font-bold.ttf", "assets/slopfin-update.bin",
                                               "assets/slopfin-sandbox.bin"};
    for (const char *path : required)
        if (std::none_of(entries.begin(), entries.end(),
                         [path](const Entry &e) { return e.path == path && e.size >= 1024; }))
            return fail("Required application assets are missing.");
    if (!executable || !metadata || !runtime)
        return fail("Required application files are missing.");
    if (!write_file(stage + "/manifest", manifest.data(), manifest.size()))
        return fail("Could not finish preparing the update.");
    return true;
}
} // namespace slopfin::update
