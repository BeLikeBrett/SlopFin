/*
 * SlopFin - splicing refreshed rows back into Home.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Continue Watching and Next Up are re-fetched while the app is open, so they
 * have to be put back into a Home that is already on screen without disturbing
 * the rows around them. The awkward cases are the empty ones: a row that was
 * there and should now be gone, and a row that was absent and has to appear.
 *
 * Header-only and free of the app's types so it can be tested on its own.
 */

#ifndef SLOPFIN_HOME_ROWS_HPP
#define SLOPFIN_HOME_ROWS_HPP

#include <string>
#include <utility>
#include <vector>

namespace slopfin::home
{

/*
 * `refreshed` is what the server just answered, in the order it should appear,
 * and holds only rows that came back with items. `owned` names every row the
 * refresh is authoritative about, including any that came back empty.
 *
 * The result is `refreshed` followed by every row of `existing` the refresh
 * does not own, in its original order. A row in `owned` but missing from
 * `refreshed` is dropped, which is how the last resumed item leaving Continue
 * Watching makes the row disappear rather than linger with stale contents.
 */
template <typename Row, typename TitleOf>
std::vector<Row> merge_progress(std::vector<Row> refreshed, std::vector<Row> existing,
                                const std::vector<std::string> &owned, TitleOf title_of)
{
    std::vector<Row> out;
    out.reserve(refreshed.size() + existing.size());
    for (Row &row : refreshed)
        out.push_back(std::move(row));
    for (Row &row : existing)
    {
        bool claimed = false;
        for (const std::string &name : owned)
            if (title_of(row) == name)
            {
                claimed = true;
                break;
            }
        if (!claimed)
            out.push_back(std::move(row));
    }
    return out;
}

} // namespace slopfin::home

#endif
