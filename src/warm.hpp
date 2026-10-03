/*
 * SlopFin - warming every poster in the library.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Once signed in, every film, show and season poster is fetched into memory
 * before anyone scrolls to it, so a library opens and scrolls with its art
 * already there. Nothing is written to storage; it lasts as long as the
 * process does.
 *
 * Episode stills are deliberately not included. There are thousands of them,
 * and at the size they are drawn they would take most of the console's
 * flexible memory. A season's stills are warmed when its page opens instead.
 */

#ifndef SLOPFIN_WARM_HPP
#define SLOPFIN_WARM_HPP

namespace slopfin::warm
{

/*
 * Starts walking the library on a thread of its own, once per run. The
 * heights are the ones each kind of poster is drawn at, so what is warmed is
 * exactly what gets drawn. Returns immediately.
 */
void begin(int poster_height, int season_height) noexcept;

} // namespace slopfin::warm

#endif
