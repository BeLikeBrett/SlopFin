/*
 * SlopFin - screens, navigation and the render loop's contents.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SLOPFIN_APP_HPP

#include <cstdint>
#include <string>
#define SLOPFIN_APP_HPP

namespace slopfin::app
{

void start() noexcept;

/* Shared clock and the flip timing the main loop measures. */
std::uint64_t now_us() noexcept;
void note_present(std::uint64_t microseconds) noexcept;
void note_pad(std::uint64_t microseconds) noexcept;

/* One frame: consume input, advance animation, draw. */
void frame() noexcept;

/* The render loop must stop presenting before releasing its display. */
[[nodiscard]] bool exit_requested() noexcept;

/* Development aid: open an item's detail page, as if chosen. Render thread only. */
void open_item(const std::string &item_id) noexcept;
#ifdef SLOPFIN_HOST
/* Submit through the real scoped search, for reproducible headless previews. */
void preview_search(const std::string &term) noexcept;
#endif

} // namespace slopfin::app

#endif
