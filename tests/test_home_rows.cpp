/*
 * SlopFin - host tests for refreshing Continue Watching and Next Up in place.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The complaint: start a film, back out, and it was not in Continue Watching
 * until the app was restarted. The fix re-fetches those two rows while Home is
 * on screen and splices them back in. What can go wrong is all in the splice
 * -- a row that should vanish lingering, the library rows reordering, or a
 * refresh that returned nothing wiping Home -- so that is what is checked.
 */

#include "home_rows.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;

void check(bool condition, const std::string &what)
{
    if (condition)
        return;
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
}

struct Row
{
    std::string title;
    std::vector<std::string> items;
};

const std::vector<std::string> kOwned{"Continue Watching", "Next Up"};

std::vector<Row> merge(std::vector<Row> refreshed, std::vector<Row> existing)
{
    return slopfin::home::merge_progress(std::move(refreshed), std::move(existing), kOwned,
                                         [](const Row &row) { return row.title; });
}

std::vector<std::string> titles(const std::vector<Row> &rows)
{
    std::vector<std::string> out;
    for (const Row &row : rows)
        out.push_back(row.title);
    return out;
}

std::vector<Row> home()
{
    return {{"Continue Watching", {"Avatar"}},
            {"Next Up", {"Fray S4E3"}},
            {"Latest in Movies", {"Dune", "Sinners"}},
            {"Latest in Shows", {"Bloodhounds"}}};
}
} // namespace

int main()
{
    /* The scenario itself: a film just watched moves to the front. */
    {
        const std::vector<Row> rows = merge(
            {{"Continue Watching", {"Oppenheimer", "Avatar"}}, {"Next Up", {"Fray S4E3"}}}, home());
        check(rows.size() == 4, "row count is unchanged");
        check(rows[0].title == "Continue Watching" && rows[0].items.front() == "Oppenheimer",
              "the film just watched leads Continue Watching");
        check(titles(rows) == std::vector<std::string>{"Continue Watching", "Next Up",
                                                       "Latest in Movies", "Latest in Shows"},
              "every row keeps its place");
        check(rows[2].items == std::vector<std::string>{"Dune", "Sinners"},
              "library rows are carried over untouched");
    }

    /* Finishing the last resumable thing: the row goes, it does not linger. */
    {
        const std::vector<Row> rows = merge({{"Next Up", {"Fray S4E4"}}}, home());
        check(titles(rows) ==
                  std::vector<std::string>{"Next Up", "Latest in Movies", "Latest in Shows"},
              "an emptied Continue Watching disappears");
        check(rows[0].items.front() == "Fray S4E4", "Next Up advances to the next episode");
    }

    /* Starting something on a Home that had no progress rows at all. */
    {
        const std::vector<Row> rows =
            merge({{"Continue Watching", {"Oppenheimer"}}},
                  {{"Latest in Movies", {"Dune"}}, {"Latest in Shows", {"Bloodhounds"}}});
        check(titles(rows) == std::vector<std::string>{"Continue Watching", "Latest in Movies",
                                                       "Latest in Shows"},
              "a new Continue Watching appears ahead of the library rows");
    }

    /* The server answering with nothing must never take the library rows. */
    {
        const std::vector<Row> rows = merge({}, home());
        check(titles(rows) == std::vector<std::string>{"Latest in Movies", "Latest in Shows"},
              "an empty answer only removes the two progress rows");
    }

    /* Running the same refresh twice changes nothing the second time. */
    {
        std::vector<Row> once = merge({{"Continue Watching", {"Oppenheimer"}}}, home());
        std::vector<Row> twice = merge({{"Continue Watching", {"Oppenheimer"}}}, once);
        check(titles(once) == titles(twice), "a repeated refresh is stable");
    }

    if (g_failures == 0)
        std::printf("home rows: all checks passed\n");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
