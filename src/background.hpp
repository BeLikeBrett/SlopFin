/*
 * SlopFin - serialized background jobs, isolated by service.
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

enum class Lane
{
    general,
    requests,
    updates,
    count
};

/* Separate lanes keep a slow request server from delaying account or update work. */
bool run(std::function<void()> job, Lane lane = Lane::general) noexcept;

/* Jobs queued or running; for a "working" indicator. */
int pending() noexcept;

} // namespace slopfin::background

#endif
