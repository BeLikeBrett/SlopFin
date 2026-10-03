/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "search_sections.hpp"
#include <cassert>
#include <cstdio>
int main()
{
    using namespace slopfin;
    std::vector<jellyfin::Item> items;
    const auto add = [&](const char *id, const char *type)
    {
        jellyfin::Item item;
        item.id = id;
        item.type = type;
        items.push_back(item);
    };
    for (int i = 0; i < 60; ++i)
    {
        jellyfin::Item item;
        item.id = "episode-" + std::to_string(i);
        item.type = "Episode";
        items.push_back(item);
    }
    add("movie-1", "Movie");
    add("movie-2", "Movie");
    add("series-1", "Series");
    add("movie-1", "Movie");
    auto sections = search_sections::build(items);
    assert(sections.size() == 3 && sections[0].kind == 0 && sections[1].kind == 1 &&
           sections[2].kind == 2);
    assert(sections[0].indices.size() == 2 && sections[1].indices.size() == 1 &&
           sections[2].indices.size() == 60);
    assert(sections[1].top == sections[0].extent() &&
           sections[2].top == sections[0].extent() + sections[1].extent());
    assert(search_sections::move(sections, 60, 1, 0) == 61);
    assert(search_sections::move(sections, 61, 1, 0) == 61); // no wrap into another kind
    assert(search_sections::move(sections, 61, 0, 1) == 62); // shorter rail clamps column
    assert(search_sections::move(sections, 62, 0, 1) == 0);
    assert(search_sections::move(sections, 0, 1, 0) == 1);
    assert(search_sections::move(sections, 59, 0, -1) == 62);
    assert(search_sections::includes("Series,Episode", "Episode"));
    assert(!search_sections::includes("Series,Episode", "Movie"));
    assert(search_sections::includes("", "Movie"));
    assert(search_sections::build({}).empty());
    std::puts("Search grouping, title priority, duplicate filtering and rail navigation passed");
}
