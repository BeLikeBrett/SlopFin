/*
 * SlopFin - validated Jellyfin intro and chapter markers.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SLOPFIN_INTRO_METADATA_HPP
#define SLOPFIN_INTRO_METADATA_HPP

#include "json.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace slopfin::intro
{
constexpr double kTicksPerSecond = 10000000.0;
struct Markers
{
    double intro_start = -1.0;
    double intro_end = -1.0;
    double outro_start = -1.0;
};

inline bool timestamp(double value) noexcept
{
    return std::isfinite(value) && value >= 0.0;
}

/* A zero runtime means unknown. Never round the runtime down to minutes. */
inline bool bounds(double start, double end, double runtime = 0.0) noexcept
{
    return timestamp(start) && timestamp(end) && end > start && std::isfinite(runtime) &&
           (runtime <= 0.0 || end <= runtime);
}

inline std::string normalized(std::string_view name)
{
    std::string result;
    bool space = false;
    for (unsigned char c : name)
    {
        if (std::isspace(c))
            space = !result.empty();
        else
        {
            if (space)
                result += ' ';
            result += static_cast<char>(std::tolower(c));
            space = false;
        }
    }
    return result;
}

inline bool intro_name(const std::string &name)
{
    /* Some chapter tools emit a separate end marker, not an intro to skip. */
    if (name.ends_with(" end"))
        return false;
    for (std::string_view label : {"intro", "introduction", "opening", "opening credits",
                                   "opening credit", "opening theme", "title sequence"})
        if (name == label || name.starts_with(std::string{label} + ": ") ||
            name.starts_with(std::string{label} + " - "))
            return true;
    if (name == "op")
        return true;
    if (!name.starts_with("op"))
        return false;
    std::string_view suffix{name};
    suffix.remove_prefix(2);
    if (suffix.starts_with(' '))
        suffix.remove_prefix(1);
    return !suffix.empty() && std::all_of(suffix.begin(), suffix.end(),
                                          [](unsigned char c) { return c >= '0' && c <= '9'; });
}

inline bool credits_name(const std::string &name)
{
    return name == "credits" || name == "ending" || name.find("end credit") != std::string::npos ||
           name.find("closing credit") != std::string::npos ||
           name.find("outro") != std::string::npos;
}

inline Markers chapters(const json::Value *list, double runtime)
{
    Markers result;
    if (list == nullptr || list->type != json::Type::array || !std::isfinite(runtime))
        return result;
    struct Chapter
    {
        double start;
        std::string name;
    };
    std::vector<Chapter> ordered;
    for (std::size_t i = 0; i < list->size(); ++i)
        if (const auto *entry = list->at(i); entry != nullptr)
        {
            const double start = entry->num("StartPositionTicks", -1.0) / kTicksPerSecond;
            if (timestamp(start))
                ordered.push_back({start, normalized(entry->str("Name"))});
        }
    std::sort(ordered.begin(), ordered.end(),
              [](const Chapter &a, const Chapter &b) { return a.start < b.start; });
    for (const auto &chapter : ordered)
    {
        if (runtime > 0.0 && chapter.start >= runtime)
            continue;
        if (credits_name(chapter.name) && result.outro_start < 0.0)
            result.outro_start = chapter.start;
        if (!intro_name(chapter.name) || result.intro_start >= 0.0)
            continue;
        /* Keep generic/content chapters: they bound the intro. Skip duplicates,
           not the first real scene, when looking for the end timestamp. */
        const auto next = std::upper_bound(ordered.begin(), ordered.end(), chapter.start,
                                           [](double start, const Chapter &entry)
                                           { return start < entry.start; });
        if (next != ordered.end() && bounds(chapter.start, next->start, runtime) &&
            next->start - chapter.start > 1.0 && next->start - chapter.start <= 300.0)
        {
            result.intro_start = chapter.start;
            result.intro_end = next->start;
        }
    }
    return result;
}

inline Markers segments(const json::Value &root, double runtime)
{
    Markers result;
    const json::Value *list = root.find("Items");
    if (list == nullptr)
        list = &root;
    if (list->type != json::Type::array)
        return result;
    for (std::size_t i = 0; i < list->size(); ++i)
        if (const auto *entry = list->at(i); entry != nullptr)
        {
            const double start = entry->num("StartTicks", -1.0) / kTicksPerSecond;
            const double end = entry->num("EndTicks", -1.0) / kTicksPerSecond;
            if (!bounds(start, end, runtime))
                continue;
            if (entry->str("Type") == "Intro" &&
                (result.intro_start < 0.0 || start < result.intro_start))
            {
                result.intro_start = start;
                result.intro_end = end;
            }
            else if (entry->str("Type") == "Outro" &&
                     (result.outro_start < 0.0 || start < result.outro_start))
                result.outro_start = start;
        }
    return result;
}

inline Markers with_fallback(Markers primary, const Markers &fallback) noexcept
{
    if (!bounds(primary.intro_start, primary.intro_end))
    {
        primary.intro_start = fallback.intro_start;
        primary.intro_end = fallback.intro_end;
    }
    if (primary.outro_start < 0.0)
        primary.outro_start = fallback.outro_start;
    return primary;
}
} // namespace slopfin::intro
#endif
