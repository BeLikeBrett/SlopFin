/*
 * SlopFin - startup trace.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * There is no console output and no debugger attached to a crashing launch,
 * so progress is appended to a file that survives the crash and can be read
 * back over FTP.
 */

#ifndef SLOPFIN_TRACE_HPP
#define SLOPFIN_TRACE_HPP

#include <string_view>

namespace slopfin::trace
{

/* Truncates the log and records that the process reached main. */
void begin() noexcept;

/* Appends one line and flushes immediately. */
void mark(std::string_view stage) noexcept;

} // namespace slopfin::trace

#endif
