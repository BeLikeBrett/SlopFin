/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/* SlopFin - distinct title and episode rails, with navigation across groups.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_SEARCH_SECTIONS_HPP
#define SLOPFIN_SEARCH_SECTIONS_HPP
#include "jellyfin.hpp"
#include <algorithm>
#include <array>
#include <string_view>
#include <vector>
namespace slopfin::search_sections
{
struct Section
{
    const char *label;
    int kind; // 0 movies, 1 series, 2 episodes
    std::vector<int> indices;
    int top = 0;
    int width() const
    {
        return kind == 2 ? 280 : 180;
    }
    int height() const
    {
        return kind == 2 ? 158 : 270;
    }
    int extent() const
    {
        return height() + 144;
    }
};
inline std::vector<Section> build(const std::vector<jellyfin::Item> &items)
{
    std::array<Section, 3> groups{{{"Movies", 0, {}}, {"Series", 1, {}}, {"Episodes", 2, {}}}};
    for (int index = 0; index < static_cast<int>(items.size()); ++index)
    {
        const auto &item = items[static_cast<std::size_t>(index)];
        const int group = item.type == "Movie"     ? 0
                          : item.type == "Series"  ? 1
                          : item.type == "Episode" ? 2
                                                   : -1;
        if (group < 0)
            continue;
        auto &indices = groups[static_cast<std::size_t>(group)].indices;
        const bool duplicate =
            std::any_of(indices.begin(), indices.end(), [&](int previous)
                        { return items[static_cast<std::size_t>(previous)].id == item.id; });
        if (!duplicate)
            indices.push_back(index);
    }
    std::vector<Section> result;
    int top = 0;
    for (auto &group : groups)
        if (!group.indices.empty())
        {
            group.top = top;
            top += group.extent();
            result.push_back(std::move(group));
        }
    return result;
}
inline std::pair<int, int> locate(const std::vector<Section> &sections, int item)
{
    for (int row = 0; row < static_cast<int>(sections.size()); ++row)
    {
        const auto &indices = sections[static_cast<std::size_t>(row)].indices;
        auto found = std::find(indices.begin(), indices.end(), item);
        if (found != indices.end())
            return {row, static_cast<int>(found - indices.begin())};
    }
    return {0, 0};
}
inline int move(const std::vector<Section> &sections, int item, int dx, int dy)
{
    if (sections.empty())
        return -1;
    auto [row, column] = locate(sections, item);
    const int next_row = std::clamp(row + dy, 0, static_cast<int>(sections.size()) - 1);
    const auto &indices = sections[static_cast<std::size_t>(next_row)].indices;
    const int next_column = std::clamp(column + dx, 0, static_cast<int>(indices.size()) - 1);
    return indices[static_cast<std::size_t>(next_column)];
}
inline bool includes(std::string_view types, std::string_view type)
{
    if (types.empty())
        return true;
    while (!types.empty())
    {
        auto comma = types.find(',');
        if (types.substr(0, comma) == type)
            return true;
        if (comma == std::string_view::npos)
            break;
        types.remove_prefix(comma + 1);
    }
    return false;
}
} // namespace slopfin::search_sections
#endif
