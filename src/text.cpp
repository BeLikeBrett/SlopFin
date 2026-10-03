/*
 * SlopFin - TrueType text rendering.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "text.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "../vendor/stb_truetype.h"
#pragma clang diagnostic pop

namespace
{
constexpr std::size_t kFaceCount = 5; /* the Weight values; the glyph key holds three bits of it */
constexpr int kCacheSlots = 1024;

struct Face
{
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ready = false;
};

/* One rasterised glyph at one pixel size. */
struct Glyph
{
    std::uint32_t key = 0; /* codepoint | size << 21 | weight << 29 */
    bool used = false;
    int width = 0;
    int height = 0;
    int offset_x = 0;
    int offset_y = 0;
    int advance = 0;
    std::vector<unsigned char> coverage;
};

std::array<Face, kFaceCount> g_faces{};
std::array<Glyph, kCacheSlots> g_cache{};
bool g_ready = false;

bool load_face(Face &face, const char *path) noexcept
{
    std::FILE *file = std::fopen(path, "rb");
    if (file == nullptr)
        return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0)
    {
        std::fclose(file);
        return false;
    }
    face.data.resize(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(face.data.data(), 1, face.data.size(), file);
    std::fclose(file);
    if (read != face.data.size())
        return false;
    if (stbtt_InitFont(&face.info, face.data.data(),
                       stbtt_GetFontOffsetForIndex(face.data.data(), 0)) == 0)
        return false;
    face.ready = true;
    return true;
}

Face &face_for(slopfin::text::Weight weight) noexcept
{
    const auto index = static_cast<std::size_t>(weight);
    if (index < kFaceCount && g_faces[index].ready)
        return g_faces[index];
    return g_faces[0];
}

/* Decodes one UTF-8 codepoint, advancing the cursor. Invalid bytes yield U+FFFD. */
std::uint32_t next_codepoint(std::string_view text, std::size_t &cursor) noexcept
{
    if (cursor >= text.size())
        return 0;
    const auto byte = static_cast<unsigned char>(text[cursor]);
    if (byte < 0x80u)
    {
        ++cursor;
        return byte;
    }
    int extra = 0;
    std::uint32_t value = 0;
    if ((byte & 0xe0u) == 0xc0u)
    {
        extra = 1;
        value = byte & 0x1fu;
    }
    else if ((byte & 0xf0u) == 0xe0u)
    {
        extra = 2;
        value = byte & 0x0fu;
    }
    else if ((byte & 0xf8u) == 0xf0u)
    {
        extra = 3;
        value = byte & 0x07u;
    }
    else
    {
        ++cursor;
        return 0xfffdu;
    }
    if (cursor + static_cast<std::size_t>(extra) >= text.size() + 0u &&
        cursor + static_cast<std::size_t>(extra) > text.size() - 1u)
    {
        cursor = text.size();
        return 0xfffdu;
    }
    ++cursor;
    for (int i = 0; i < extra; ++i)
    {
        if (cursor >= text.size())
            return 0xfffdu;
        const auto continuation = static_cast<unsigned char>(text[cursor]);
        if ((continuation & 0xc0u) != 0x80u)
            return 0xfffdu;
        value = (value << 6) | (continuation & 0x3fu);
        ++cursor;
    }
    return value;
}

std::uint32_t cache_key(std::uint32_t codepoint, int pixel_size,
                        slopfin::text::Weight weight) noexcept
{
    return (codepoint & 0x1fffffu) | (static_cast<std::uint32_t>(pixel_size & 0xff) << 21) |
           (static_cast<std::uint32_t>(weight) << 29);
}

const Glyph *acquire(std::uint32_t codepoint, int pixel_size, slopfin::text::Weight weight) noexcept
{
    const std::uint32_t key = cache_key(codepoint, pixel_size, weight);
    const std::size_t slot = (key * 2654435761u) % kCacheSlots;

    /* Linear probe a short run; on failure evict the first probed slot. */
    for (std::size_t i = 0; i < 8; ++i)
    {
        Glyph &entry = g_cache[(slot + i) % kCacheSlots];
        if (entry.used && entry.key == key)
            return &entry;
        if (!entry.used)
            break;
    }

    Face &face = face_for(weight);
    if (!face.ready)
        return nullptr;

    Glyph glyph;
    glyph.key = key;
    glyph.used = true;
    const float scale = stbtt_ScaleForPixelHeight(&face.info, static_cast<float>(pixel_size));
    int advance = 0;
    int bearing = 0;
    stbtt_GetCodepointHMetrics(&face.info, static_cast<int>(codepoint), &advance, &bearing);
    glyph.advance = static_cast<int>(static_cast<float>(advance) * scale + 0.5f);

    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    stbtt_GetCodepointBitmapBox(&face.info, static_cast<int>(codepoint), scale, scale, &x0, &y0,
                                &x1, &y1);
    glyph.width = x1 - x0;
    glyph.height = y1 - y0;
    glyph.offset_x = x0;
    glyph.offset_y = y0;
    if (glyph.width > 0 && glyph.height > 0)
    {
        glyph.coverage.assign(static_cast<std::size_t>(glyph.width) * glyph.height, 0);
        stbtt_MakeCodepointBitmap(&face.info, glyph.coverage.data(), glyph.width, glyph.height,
                                  glyph.width, scale, scale, static_cast<int>(codepoint));
    }

    std::size_t target = slot;
    for (std::size_t i = 0; i < 8; ++i)
    {
        if (!g_cache[(slot + i) % kCacheSlots].used)
        {
            target = (slot + i) % kCacheSlots;
            break;
        }
    }
    g_cache[target] = std::move(glyph);
    return &g_cache[target];
}

/* Ascent in pixels, so callers can treat y as the top of the line box. */
int ascent_for(int pixel_size, slopfin::text::Weight weight) noexcept
{
    Face &face = face_for(weight);
    if (!face.ready)
        return pixel_size;
    int ascent = 0;
    int descent = 0;
    int gap = 0;
    stbtt_GetFontVMetrics(&face.info, &ascent, &descent, &gap);
    const float scale = stbtt_ScaleForPixelHeight(&face.info, static_cast<float>(pixel_size));
    return static_cast<int>(static_cast<float>(ascent) * scale + 0.5f);
}
} // namespace

namespace slopfin::text
{

bool initialize() noexcept
{
    /* On the console the package mounts at /app0; the Linux preview reads the
       same files out of the working tree, or wherever SLOPFIN_ASSETS points. */
#ifdef SLOPFIN_HOST
    const char *root = std::getenv("SLOPFIN_ASSETS");
    const std::string assets = std::string{root != nullptr ? root : "assets"} + "/";
#else
    const std::string assets = "/app0/assets/";
#endif
    g_ready = load_face(g_faces[0], (assets + "font-regular.ttf").c_str());
    (void)load_face(g_faces[1], (assets + "font-medium.ttf").c_str());
    (void)load_face(g_faces[2], (assets + "font-bold.ttf").c_str());
    (void)load_face(g_faces[3], (assets + "font-readable-regular.ttf").c_str());
    (void)load_face(g_faces[4], (assets + "font-readable-bold.ttf").c_str());
    return g_ready;
}

int line_height(int pixel_size) noexcept
{
    return (pixel_size * 3) / 2;
}

/* Measured at the real size, reported in logical units, because every caller
   uses it to lay out logical coordinates. */
int measure(std::string_view utf8, int pixel_size, Weight weight) noexcept
{
    if (!g_ready)
        return 0;
    const int size = std::max(1, gfx::to_physical_size(pixel_size));
    int width = 0;
    std::size_t cursor = 0;
    while (cursor < utf8.size())
    {
        const std::uint32_t codepoint = next_codepoint(utf8, cursor);
        if (codepoint == 0)
            break;
        const Glyph *glyph = acquire(codepoint, size, weight);
        if (glyph != nullptr)
            width += glyph->advance;
    }
    return gfx::to_logical_size(width);
}

int centered_y(int y, int height, std::string_view utf8, int pixel_size, Weight weight) noexcept
{
    if (!g_ready)
        return y + (height - pixel_size) / 2;
    const int size = std::max(1, gfx::to_physical_size(pixel_size));
    const int ascent = ascent_for(size, weight);
    int top = size, bottom = 0;
    std::size_t cursor = 0;
    while (cursor < utf8.size())
    {
        const auto codepoint = next_codepoint(utf8, cursor);
        const Glyph *glyph = acquire(codepoint, size, weight);
        if (glyph && glyph->height > 0)
        {
            top = std::min(top, ascent + glyph->offset_y);
            bottom = std::max(bottom, ascent + glyph->offset_y + glyph->height);
        }
    }
    if (bottom <= top)
        return y + (height - pixel_size) / 2;
    const int logical_top = gfx::to_logical_size(top);
    const int logical_height = gfx::to_logical_size(bottom - top);
    return y + (height - logical_height) / 2 - logical_top;
}

void draw(int x, int y, std::string_view utf8, int pixel_size, Weight weight,
          gfx::Color color) noexcept
{
    if (!g_ready)
        return;
    /*
     * Glyphs are rasterised at the surface's real pixel size and positioned in
     * real pixels. Rendering at the logical size and letting the surface scale
     * the result is exactly the softness this avoids.
     */
    const int size = std::max(1, gfx::to_physical_size(pixel_size));
    const int baseline = gfx::to_physical_y(y) + ascent_for(size, weight);
    std::size_t cursor = 0;
    int pen = gfx::to_physical_x(x);
    while (cursor < utf8.size())
    {
        const std::uint32_t codepoint = next_codepoint(utf8, cursor);
        if (codepoint == 0)
            break;
        const Glyph *glyph = acquire(codepoint, size, weight);
        if (glyph == nullptr)
            continue;
        if (glyph->width > 0 && glyph->height > 0)
        {
            gfx::blend_mask_physical(pen + glyph->offset_x, baseline + glyph->offset_y,
                                     glyph->width, glyph->height, glyph->coverage.data(), color);
        }
        pen += glyph->advance;
    }
}

void draw_ellipsized(int x, int y, int max_width, std::string_view utf8, int pixel_size,
                     Weight weight, gfx::Color color) noexcept
{
    if (measure(utf8, pixel_size, weight) <= max_width)
    {
        draw(x, y, utf8, pixel_size, weight, color);
        return;
    }
    const int ellipsis = measure("...", pixel_size, weight);
    std::size_t cursor = 0;
    std::size_t last_fit = 0;
    int width = 0;
    while (cursor < utf8.size())
    {
        const std::size_t start = cursor;
        const std::uint32_t codepoint = next_codepoint(utf8, cursor);
        if (codepoint == 0)
            break;
        const Glyph *glyph = acquire(codepoint, pixel_size, weight);
        if (glyph == nullptr)
            continue;
        if (width + glyph->advance + ellipsis > max_width)
        {
            last_fit = start;
            break;
        }
        width += glyph->advance;
        last_fit = cursor;
    }
    draw(x, y, utf8.substr(0, last_fit), pixel_size, weight, color);
    draw(x + width, y, "...", pixel_size, weight, color);
}

namespace
{
/* Where a wrapped line really starts: past any spaces the break left behind. */
std::size_t skip_spaces(std::string_view utf8, std::size_t at) noexcept
{
    while (at < utf8.size() && (utf8[at] == ' ' || utf8[at] == '\t'))
        ++at;
    return at;
}
} // namespace

int draw_wrapped(int x, int y, int max_width, int max_lines, int spacing, std::string_view utf8,
                 int pixel_size, Weight weight, gfx::Color color) noexcept
{
    int drawn = 0;
    std::size_t line_start = 0;
    std::size_t cursor = 0;
    std::size_t last_space = std::string_view::npos;
    int width = 0;

    while (cursor < utf8.size() && drawn < max_lines)
    {
        const std::size_t start = cursor;
        const std::uint32_t codepoint = next_codepoint(utf8, cursor);
        if (codepoint == 0)
            break;
        if (codepoint == '\n')
        {
            draw(x, y + drawn * spacing, utf8.substr(line_start, start - line_start), pixel_size,
                 weight, color);
            ++drawn;
            line_start = skip_spaces(utf8, cursor);
            cursor = line_start;
            last_space = std::string_view::npos;
            width = 0;
            continue;
        }
        if (codepoint == ' ')
            last_space = start;
        const Glyph *glyph = acquire(codepoint, pixel_size, weight);
        if (glyph == nullptr)
            continue;
        width += glyph->advance;
        if (width <= max_width)
            continue;

        const std::size_t break_at =
            (last_space != std::string_view::npos && last_space > line_start) ? last_space : start;
        const bool final_line = drawn + 1 >= max_lines;
        std::string_view line = utf8.substr(line_start, break_at - line_start);
        if (final_line)
            draw_ellipsized(x, y + drawn * spacing, max_width, utf8.substr(line_start), pixel_size,
                            weight, color);
        else
            draw(x, y + drawn * spacing, line, pixel_size, weight, color);
        ++drawn;
        /*
         * A line that breaks on the space itself would otherwise begin with
         * that space, indenting it out of line with the ones around it.
         */
        line_start = skip_spaces(utf8, (break_at == start) ? start : break_at + 1);
        cursor = line_start;
        last_space = std::string_view::npos;
        width = 0;
    }

    if (drawn < max_lines && line_start < utf8.size())
    {
        draw(x, y + drawn * spacing, utf8.substr(line_start), pixel_size, weight, color);
        ++drawn;
    }
    return drawn;
}

} // namespace slopfin::text
