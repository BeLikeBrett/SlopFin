/*
 * SlopFin - interface icons.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Each icon is a union of simple signed distance fields evaluated in a unit
 * square. Coverage is 0.5 minus the distance in pixels, clamped, which gives a
 * one-pixel anti-aliased edge at any size without supersampling. Masks are
 * built once per (icon, size) and cached.
 */

#include "icons.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace slopfin::icons
{
namespace
{
struct Vec
{
    float x, y;
};

float length(float x, float y) noexcept
{
    return std::sqrt(x * x + y * y);
}

float circle(Vec p, Vec c, float r) noexcept
{
    return length(p.x - c.x, p.y - c.y) - r;
}

/* A line segment with round caps: the stroke primitive for most icons. */
float capsule(Vec p, Vec a, Vec b, float r) noexcept
{
    const float pax = p.x - a.x, pay = p.y - a.y;
    const float bax = b.x - a.x, bay = b.y - a.y;
    const float h = std::clamp((pax * bax + pay * bay) / (bax * bax + bay * bay), 0.0f, 1.0f);
    return length(pax - bax * h, pay - bay * h) - r;
}

float rounded_box(Vec p, Vec c, Vec half, float r) noexcept
{
    const float qx = std::fabs(p.x - c.x) - half.x + r;
    const float qy = std::fabs(p.y - c.y) - half.y + r;
    return length(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
}

/* Exact triangle distance (Inigo Quilez). */
float triangle(Vec p, Vec a, Vec b, Vec c) noexcept
{
    const Vec e0{b.x - a.x, b.y - a.y}, e1{c.x - b.x, c.y - b.y}, e2{a.x - c.x, a.y - c.y};
    const Vec v0{p.x - a.x, p.y - a.y}, v1{p.x - b.x, p.y - b.y}, v2{p.x - c.x, p.y - c.y};
    const auto project = [](Vec v, Vec e)
    {
        const float t = std::clamp((v.x * e.x + v.y * e.y) / (e.x * e.x + e.y * e.y), 0.0f, 1.0f);
        return Vec{v.x - e.x * t, v.y - e.y * t};
    };
    const Vec pq0 = project(v0, e0), pq1 = project(v1, e1), pq2 = project(v2, e2);
    const float s = (e0.x * e2.y - e0.y * e2.x) > 0.0f ? 1.0f : -1.0f;
    const float d0 = pq0.x * pq0.x + pq0.y * pq0.y, d1 = pq1.x * pq1.x + pq1.y * pq1.y,
                d2 = pq2.x * pq2.x + pq2.y * pq2.y;
    const float c0 = s * (v0.x * e0.y - v0.y * e0.x), c1 = s * (v1.x * e1.y - v1.y * e1.x),
                c2 = s * (v2.x * e2.y - v2.y * e2.x);
    const float d = std::min({d0, d1, d2});
    const float inside = std::min({c0, c1, c2});
    /* Negative inside the triangle, positive outside. */
    return -std::sqrt(d) * (inside > 0.0f ? 1.0f : -1.0f);
}

/* An arc of a ring, from angle a0 to a1 (radians, counter-clockwise from +x). */
float arc(Vec p, Vec c, float r, float half_width, float a0, float a1) noexcept
{
    const float dx = p.x - c.x, dy = p.y - c.y;
    float angle = std::atan2(-dy, dx); /* screen y grows downward */
    const float two_pi = 6.2831853f;
    const auto within = [&](float a)
    {
        float from = a0, to = a1;
        while (to < from)
            to += two_pi;
        while (a < from)
            a += two_pi;
        return a <= to;
    };
    if (within(angle))
        return std::fabs(length(dx, dy) - r) - half_width;
    /* Outside the swept range: distance to the nearer rounded end. */
    const Vec e0{c.x + r * std::cos(a0), c.y - r * std::sin(a0)};
    const Vec e1{c.x + r * std::cos(a1), c.y - r * std::sin(a1)};
    return std::min(circle(p, e0, half_width), circle(p, e1, half_width));
}

float field(Icon icon, Vec p) noexcept
{
    switch (icon)
    {
    case Icon::play:
        return triangle(p, {0.30f, 0.18f}, {0.30f, 0.82f}, {0.84f, 0.50f}) - 0.02f;
    case Icon::pause:
        return std::min(rounded_box(p, {0.35f, 0.5f}, {0.085f, 0.30f}, 0.04f),
                        rounded_box(p, {0.65f, 0.5f}, {0.085f, 0.30f}, 0.04f));
    case Icon::skip_back:
    case Icon::skip_forward:
    {
        /*
         * A clockwise circular arrow (forward) with its gap at the upper right
         * and the head at the top, pointing into the gap. Back is its mirror.
         * The caller draws the "10" inside.
         */
        const bool back = icon == Icon::skip_back;
        const Vec q = back ? Vec{1.0f - p.x, p.y} : p;
        constexpr float kDegrees = 3.14159265f / 180.0f;
        const float ring =
            arc(q, {0.5f, 0.54f}, 0.34f, 0.042f, 80.0f * kDegrees, 400.0f * kDegrees);
        const float head = triangle(q, {0.578f, 0.097f}, {0.540f, 0.313f}, {0.697f, 0.229f});
        return std::min(ring, head);
    }
    case Icon::subtitles:
    {
        /* A rounded frame with two lines of "text", like a caption bubble. */
        const float frame = std::fabs(rounded_box(p, {0.5f, 0.5f}, {0.40f, 0.29f}, 0.10f)) - 0.035f;
        const float line1 = std::min(capsule(p, {0.25f, 0.44f}, {0.47f, 0.44f}, 0.035f),
                                     capsule(p, {0.58f, 0.44f}, {0.75f, 0.44f}, 0.035f));
        const float line2 = std::min(capsule(p, {0.25f, 0.60f}, {0.36f, 0.60f}, 0.035f),
                                     capsule(p, {0.47f, 0.60f}, {0.75f, 0.60f}, 0.035f));
        return std::min({frame, line1, line2});
    }
    case Icon::audio:
    {
        /* A waveform: five bars, tallest in the middle. */
        constexpr float xs[5] = {0.20f, 0.35f, 0.50f, 0.65f, 0.80f};
        constexpr float hs[5] = {0.10f, 0.22f, 0.32f, 0.22f, 0.10f};
        float d = 1.0f;
        for (int i = 0; i < 5; ++i)
            d = std::min(d, capsule(p, {xs[i], 0.5f - hs[i]}, {xs[i], 0.5f + hs[i]}, 0.045f));
        return d;
    }
    case Icon::quality:
    {
        /* Two sliders with knobs: the universal "adjust" glyph. */
        const float rails = std::min(capsule(p, {0.16f, 0.35f}, {0.84f, 0.35f}, 0.03f),
                                     capsule(p, {0.16f, 0.65f}, {0.84f, 0.65f}, 0.03f));
        const float knobs =
            std::min(circle(p, {0.64f, 0.35f}, 0.095f), circle(p, {0.36f, 0.65f}, 0.095f));
        return std::min(rails, knobs);
    }
    case Icon::info:
    {
        const float ring = std::fabs(circle(p, {0.5f, 0.5f}, 0.40f)) - 0.035f;
        const float dot = circle(p, {0.5f, 0.32f}, 0.055f);
        const float bar = capsule(p, {0.5f, 0.47f}, {0.5f, 0.70f}, 0.045f);
        return std::min({ring, dot, bar});
    }
    case Icon::check:
        return std::min(capsule(p, {0.20f, 0.52f}, {0.41f, 0.73f}, 0.06f),
                        capsule(p, {0.41f, 0.73f}, {0.82f, 0.28f}, 0.06f));
    case Icon::jellyfin:
    {
        /*
         * The Jellyfin mark: a rounded triangle with a hole, and a second
         * solid one inside it. Taken from the project's own icon, whose paths
         * describe two nested shapes with apexes at 0.5 and bases at 0.87 and
         * 0.67 of the viewBox. A triangle distance minus a radius gives the
         * rounded corners, so the vertices here are the sharp triangle that
         * dilates into the drawn one.
         */
        constexpr float kRound = 0.085f;
        const float outer =
            triangle(p, {0.5f, 0.175f}, {0.175f, 0.775f}, {0.825f, 0.775f}) - kRound;
        const float hole = triangle(p, {0.5f, 0.345f}, {0.318f, 0.688f}, {0.682f, 0.688f}) - 0.055f;
        const float inner =
            triangle(p, {0.5f, 0.475f}, {0.418f, 0.632f}, {0.582f, 0.632f}) - 0.042f;
        return std::min(std::max(outer, -hole), inner);
    }
    case Icon::ps_triangle:
    {
        /* The pad draws these as outlines, not as filled shapes, and the
           outline is what makes them read as buttons. Taking the absolute
           value of a distance field turns any solid into its own outline. */
        const float solid = triangle(p, {0.5f, 0.17f}, {0.14f, 0.80f}, {0.86f, 0.80f}) + 0.055f;
        return std::fabs(solid) - 0.055f;
    }
    case Icon::ps_circle:
        return std::fabs(circle(p, {0.5f, 0.5f}, 0.33f)) - 0.055f;
    case Icon::ps_cross:
        return std::min(capsule(p, {0.22f, 0.22f}, {0.78f, 0.78f}, 0.055f),
                        capsule(p, {0.78f, 0.22f}, {0.22f, 0.78f}, 0.055f));
    case Icon::ps_square:
        return std::fabs(rounded_box(p, {0.5f, 0.5f}, {0.33f, 0.33f}, 0.03f)) - 0.055f;
    case Icon::search:
    {
        const float glass = std::fabs(circle(p, {0.44f, 0.42f}, 0.26f)) - 0.05f;
        const float handle = capsule(p, {0.63f, 0.61f}, {0.83f, 0.81f}, 0.055f);
        return std::min(glass, handle);
    }
    case Icon::r2:
    {
        /* The trigger's outline: a tab with a rounded top, as it is drawn on
           the controller and in every prompt Sony ships. */
        const float body = std::fabs(rounded_box(p, {0.5f, 0.56f}, {0.30f, 0.30f}, 0.13f)) - 0.05f;
        return body;
    }
    case Icon::chevron_left:
    case Icon::chevron_right:
    {
        const Vec q = icon == Icon::chevron_left ? p : Vec{1.0f - p.x, p.y};
        return std::min(capsule(q, {0.62f, 0.20f}, {0.34f, 0.50f}, 0.065f),
                        capsule(q, {0.34f, 0.50f}, {0.62f, 0.80f}, 0.065f));
    }
    case Icon::count:
        break;
    }
    return 1.0f;
}

struct Entry
{
    Icon icon;
    int size;
    std::vector<unsigned char> coverage;
};
std::vector<Entry> g_cache;
} // namespace

const unsigned char *mask(Icon icon, int size) noexcept
{
    if (size <= 0 || size > 512)
        return nullptr;
    for (const Entry &entry : g_cache)
        if (entry.icon == icon && entry.size == size)
            return entry.coverage.data();

    Entry entry{icon, size, std::vector<unsigned char>(static_cast<std::size_t>(size) * size)};
    const float scale = static_cast<float>(size);
    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            const Vec p{(static_cast<float>(x) + 0.5f) / scale,
                        (static_cast<float>(y) + 0.5f) / scale};
            const float coverage = std::clamp(0.5f - field(icon, p) * scale, 0.0f, 1.0f);
            entry.coverage[static_cast<std::size_t>(y) * size + x] =
                static_cast<unsigned char>(coverage * 255.0f + 0.5f);
        }
    }
    g_cache.push_back(std::move(entry));
    return g_cache.back().coverage.data();
}

void draw(Icon icon, int x, int y, int size, gfx::Color color) noexcept
{
    /* Built at the surface's pixel size, so the curves stay clean at 4K. */
    const int pixels = std::max(1, gfx::to_physical_size(size));
    if (const unsigned char *coverage = mask(icon, pixels); coverage != nullptr)
        gfx::blend_mask_physical(gfx::to_physical_x(x), gfx::to_physical_y(y), pixels, pixels,
                                 coverage, color);
}

void draw_gradient(Icon icon, int x, int y, int size, gfx::Color from, gfx::Color to) noexcept
{
    const int pixels = std::max(1, gfx::to_physical_size(size));
    if (const unsigned char *coverage = mask(icon, pixels); coverage != nullptr)
        gfx::blend_mask_gradient_physical(gfx::to_physical_x(x), gfx::to_physical_y(y), pixels,
                                          pixels, coverage, from, to);
}

} // namespace slopfin::icons
