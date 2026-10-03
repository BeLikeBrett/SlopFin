/*
 * SlopFin - one background thread for the screens that talk to the server
 * outside the main data worker: the profile and the dashboard.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Jobs run one at a time, in order, on a thread given the spare cores. A job
 * must not touch the render thread's state directly; it hands results back
 * under its own module's mutex.
 */

#ifndef SLOPFIN_BACKGROUND_HPP
#define SLOPFIN_BACKGROUND_HPP

#include <functional>

namespace slopfin::background
{

void run(std::function<void()> job) noexcept;

/* Jobs queued or running; for a "working" indicator. */
int pending() noexcept;

} // namespace slopfin::background

#endif
