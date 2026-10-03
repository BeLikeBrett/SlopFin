/*
 * SlopFin - the server's dashboard, for administrators.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Jellyfin web dashboard's pages, reshaped for a controller: a list of
 * pages on the left and the chosen page's rows on the right. A row with things
 * to do opens a short list of them; a setting changes in place, and text is
 * typed on the console's own keyboard.
 */

#ifndef SLOPFIN_DASHBOARD_HPP
#define SLOPFIN_DASHBOARD_HPP

namespace slopfin::dashboard
{

void open() noexcept;
/* False when the viewer has left the dashboard. */
bool handle_input() noexcept;
void draw() noexcept;

} // namespace slopfin::dashboard

#endif
