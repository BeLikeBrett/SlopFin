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
#include <cmath>
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
constexpr std::size_t kFaceCount = 13;
static_assert(kFaceCount <= 16, "GPU glyph identities reserve four bits per face");
constexpr int kCacheSlots = 1024;

struct Face
{
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ready = false;
};

struct EdgeMask
{
    int width_quarters = -1;
    std::vector<unsigned char> coverage;
};

/* One rasterised glyph at one pixel size. */
struct Glyph
{
    std::uint64_t key = 0; /* codepoint | physical size << 21 | face << 37 */
    bool used = false;
    int width = 0;
    int height = 0;
    int offset_x = 0;
    int offset_y = 0;
    int advance = 0;
    std::vector<unsigned char> coverage;
    int distance_width = 0;
    int distance_height = 0;
    int distance_x = 0;
    int distance_y = 0;
    float distance_scale = 1.0f;
    std::vector<unsigned char> distance;
    std::array<EdgeMask, 2> edges;
};

std::array<Face, kFaceCount> g_faces{};
std::array<Glyph, kCacheSlots> g_cache{};
struct LineEdge
{
    std::string text;
    int size = 0;
    slopfin::text::Weight weight = slopfin::text::Weight::regular;
    int quarters = 0;
    bool shadow = false;
    int x = 0, y = 0, width = 0, height = 0;
    std::uint64_t used = 0, id = 0;
    std::vector<unsigned char> coverage;
};
std::array<LineEdge, 12> g_line_edges{};
std::uint64_t g_edge_clock = 0, g_edge_id = 0;
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

std::uint64_t cache_key(std::uint32_t codepoint, int pixel_size,
                        slopfin::text::Weight weight) noexcept
{
    return (codepoint & 0x1fffffu) | (static_cast<std::uint64_t>(pixel_size & 0xffff) << 21) |
           (static_cast<std::uint64_t>(weight) << 37);
}

Glyph *acquire(std::uint32_t codepoint, int pixel_size, slopfin::text::Weight weight) noexcept
{
    const std::uint64_t key = cache_key(codepoint, pixel_size, weight);
    /* Mix the face and size into the slot too. Otherwise every face of the
       same letter competes for the same eight slots. */
    std::uint64_t hash = key;
    hash = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ULL;
    hash = (hash ^ (hash >> 27)) * 0x94d049bb133111ebULL;
    const std::size_t slot = (hash ^ (hash >> 31)) % kCacheSlots;

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

const EdgeMask *acquire_edge(Glyph &glyph, std::uint32_t codepoint, int size,
                             slopfin::text::Weight weight, int quarters, bool shadow) noexcept
{
    if (glyph.width <= 0 || glyph.height <= 0)
        return nullptr;
    if (glyph.distance.empty())
    {
        Face &face = face_for(weight);
        /* Enough room for the heaviest offered outline at this font size.
           Keep this geometry across thickness/opacity changes in Settings. */
        const int padding = std::max(8, size / 6 + 3);
        glyph.distance_scale = 128.0f / static_cast<float>(padding);
        auto *distance = stbtt_GetCodepointSDF(
            &face.info, stbtt_ScaleForPixelHeight(&face.info, static_cast<float>(size)),
            static_cast<int>(codepoint), padding, 128, glyph.distance_scale, &glyph.distance_width,
            &glyph.distance_height, &glyph.distance_x, &glyph.distance_y);
        if (distance == nullptr)
            return nullptr;
        glyph.distance.assign(distance, distance + glyph.distance_width * glyph.distance_height);
        stbtt_FreeSDF(distance, face.info.userdata);
    }
    EdgeMask &edge = glyph.edges[shadow ? 1 : 0];
    if (edge.width_quarters != quarters)
    {
        const float radius = quarters * 0.25f;
        edge.coverage.resize(glyph.distance.size());
        for (std::size_t i = 0; i < edge.coverage.size(); ++i)
        {
            const float distance =
                (static_cast<int>(glyph.distance[i]) - 128) / glyph.distance_scale;
            float alpha;
            if (shadow)
            {
                /* A feathered contour, rather than an offset hard-edged copy. */
                const float feather = std::max(1.0f, radius);
                alpha = std::clamp((distance + feather) / (2.0f * feather), 0.0f, 1.0f);
                alpha = alpha * alpha * (3.0f - 2.0f * alpha);
            }
            else
                alpha = std::clamp(distance + radius + 0.5f, 0.0f, 1.0f);
            edge.coverage[i] = static_cast<unsigned char>(alpha * 255.0f + 0.5f);
        }
        edge.width_quarters = quarters;
    }
    return &edge;
}

const LineEdge *acquire_line_edge(std::string_view utf8, int size, slopfin::text::Weight weight,
                                  int quarters, bool shadow) noexcept
{
    for (auto &line : g_line_edges)
        if (line.used && line.size == size && line.weight == weight && line.quarters == quarters &&
            line.shadow == shadow && line.text == utf8)
        {
            line.used = ++g_edge_clock;
            return &line;
        }
    int left = 0, top = 0, right = 0, bottom = 0, pen = 0;
    std::size_t cursor = 0;
    while (cursor < utf8.size())
    {
        const auto codepoint = next_codepoint(utf8, cursor);
        Glyph *glyph = acquire(codepoint, size, weight);
        if (!glyph)
            continue;
        if (acquire_edge(*glyph, codepoint, size, weight, quarters, shadow))
        {
            left = std::min(left, pen + glyph->distance_x);
            top = std::min(top, glyph->distance_y);
            right = std::max(right, pen + glyph->distance_x + glyph->distance_width);
            bottom = std::max(bottom, glyph->distance_y + glyph->distance_height);
        }
        pen += glyph->advance;
        if (pen > 32768)
            return nullptr;
    }
    const int width = right - left, height = bottom - top;
    if (width <= 0 || height <= 0 || static_cast<std::size_t>(width) * height > 4 * 1024 * 1024)
        return nullptr;
    auto &line = *std::min_element(g_line_edges.begin(), g_line_edges.end(),
                                   [](const auto &a, const auto &b) { return a.used < b.used; });
    line.text = utf8;
    line.size = size;
    line.weight = weight;
    line.quarters = quarters;
    line.shadow = shadow;
    line.x = left;
    line.y = top;
    line.width = width;
    line.height = height;
    line.used = ++g_edge_clock;
    line.id = (std::uint64_t{0x56} << 56) | ++g_edge_id;
    line.coverage.assign(static_cast<std::size_t>(width) * height, 0);
    pen = 0;
    cursor = 0;
    while (cursor < utf8.size())
    {
        const auto codepoint = next_codepoint(utf8, cursor);
        Glyph *glyph = acquire(codepoint, size, weight);
        if (!glyph)
            continue;
        if (const auto *edge = acquire_edge(*glyph, codepoint, size, weight, quarters, shadow))
            for (int y = 0; y < glyph->distance_height; ++y)
            {
                auto *destination = line.coverage.data() + (y + glyph->distance_y - top) * width +
                                    pen + glyph->distance_x - left;
                const auto *source = edge->coverage.data() + y * glyph->distance_width;
                for (int x = 0; x < glyph->distance_width; ++x)
                    destination[x] = std::max(destination[x], source[x]);
            }
        pen += glyph->advance;
    }
    return &line;
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
    constexpr const char *extra[] = {"font-roboto-regular.ttf", "font-roboto-bold.ttf",
                                     "font-lato-regular.ttf",   "font-lato-bold.ttf",
                                     "font-serif-regular.ttf",  "font-serif-bold.ttf",
                                     "font-mono-regular.ttf",   "font-mono-bold.ttf"};
    for (std::size_t i = 0; i < std::size(extra); ++i)
        (void)load_face(g_faces[i + 5], (assets + extra[i]).c_str());
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
            /* A glyph's pixels follow from its character, size and weight. */
            const std::uint64_t id = (std::uint64_t{0x54} << 56) |
                                     (static_cast<std::uint64_t>(weight) << 40) |
                                     (static_cast<std::uint64_t>(size) << 24) | codepoint;
            gfx::blend_mask_physical(pen + glyph->offset_x, baseline + glyph->offset_y,
                                     glyph->width, glyph->height, glyph->coverage.data(), color,
                                     id);
        }
        pen += glyph->advance;
    }
}

void prepare_edges(int pixel_size, Weight weight) noexcept
{
    if (!g_ready)
        return;
    const int size = std::max(1, gfx::to_physical_size(pixel_size));
    for (std::uint32_t codepoint = 33; codepoint < 127; ++codepoint)
        if (Glyph *glyph = acquire(codepoint, size, weight))
            (void)acquire_edge(*glyph, codepoint, size, weight, 4, false);
}

void draw_edge(int x, int y, std::string_view utf8, int pixel_size, Weight weight, gfx::Color color,
               float width, bool shadow) noexcept
{
    if (!g_ready || width <= 0.0f || (color >> 24) == 0)
        return;
    const int size = std::max(1, gfx::to_physical_size(pixel_size));
    const float scale = static_cast<float>(gfx::physical_height()) / gfx::kHeight;
    const int quarters = std::clamp(static_cast<int>(width * scale * 4.0f + 0.5f), 1, 255);
    const int offset = shadow ? std::max(1, (quarters + 4) / 4) : 0;
    const int baseline = gfx::to_physical_y(y) + ascent_for(size, weight) + offset;
    if (const auto *line = acquire_line_edge(utf8, size, weight, quarters, shadow))
    {
        /* Union letter coverage first. Blending each letter separately makes
           overlapping translucent outlines develop dark seams. */
        gfx::blend_mask_physical(gfx::to_physical_x(x) + offset + line->x, baseline + line->y,
                                 line->width, line->height, line->coverage.data(), color, line->id);
    }
}

std::vector<std::string> wrap_lines(std::string_view utf8, int max_width, int pixel_size,
                                    Weight weight)
{
    std::vector<std::string> lines;
    max_width = std::max(1, max_width);
    for (std::size_t paragraph = 0; paragraph <= utf8.size();)
    {
        const std::size_t newline = utf8.find('\n', paragraph);
        const std::size_t stop = newline == std::string_view::npos ? utf8.size() : newline;
        std::size_t start = paragraph;
        if (start == stop)
            lines.emplace_back();
        while (start < stop)
        {
            std::size_t cursor = start, previous = start, space = std::string_view::npos;
            while (cursor < stop)
            {
                previous = cursor;
                if (utf8[cursor] == ' ' || utf8[cursor] == '\t')
                    space = cursor;
                (void)next_codepoint(utf8, cursor);
                cursor = std::min(cursor, stop);
                if (measure(utf8.substr(start, cursor - start), pixel_size, weight) > max_width)
                {
                    if (space != std::string_view::npos && space > start)
                        cursor = space;
                    else if (previous > start)
                        cursor = previous;
                    break;
                }
            }
            std::size_t end = cursor;
            while (end > start &&
                   (utf8[end - 1] == ' ' || utf8[end - 1] == '\t' || utf8[end - 1] == '\r'))
                --end;
            lines.emplace_back(utf8.substr(start, end - start));
            start = cursor;
            while (start < stop && (utf8[start] == ' ' || utf8[start] == '\t'))
                ++start;
        }
        if (newline == std::string_view::npos)
            break;
        paragraph = newline + 1;
    }
    return lines;
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
