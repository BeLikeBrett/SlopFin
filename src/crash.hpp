/*
 * SlopFin - diagnostics for a crash, a hang, or a session that vanished.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Three things have to be true of a report for it to be worth anything the
 * morning after: it must be written by a process that is already dying, it
 * must say what the app was *doing*, and it must survive the console killing
 * the process outright.
 *
 *   /data/slopfin-session.txt   what this run is doing now, rewritten when
 *                               the state changes, never on a timer
 *   /data/slopfin-crash.txt     the last fatal fault, written from the signal
 *                               handler with no allocation
 *   /data/slopfin-crashes.log   every report this console has ever produced
 *   /data/slopfin-lastrun.txt   the session file of a run that never said
 *                               goodbye, kept at the next start
 *
 * See docs/development/DIAGNOSTICS.md.
 */

#ifndef SLOPFIN_CRASH_HPP
#define SLOPFIN_CRASH_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace slopfin::crash
{

/* Installs the handlers and picks up an earlier run's unfinished session. */
void install() noexcept;

/*
 * Starts the thread that keeps the session file current, on the cores given
 * (never the render core). One wake a second, and a write only when something
 * changed. Called once the app knows which cores are spare.
 */
void watch(std::uint64_t cores) noexcept;

/*
 * What the app is doing, in one short phrase ("home", "detail Interstellar",
 * "playing 4K HEVC"). Cheap: it copies into a fixed buffer and writes the
 * session file only when the phrase actually changes.
 */
void phase(std::string_view what) noexcept;

/*
 * The line under it, refreshed as often as it is worth refreshing: where
 * playback is, how much memory is left. Stored only; a thread of its own puts
 * it on disk, because the render thread must not write files on a timer.
 */
void detail(std::string_view what) noexcept;

/* A moment worth remembering, kept in a ring the report prints. */
void note(std::string_view what) noexcept;

/* The frame counter, so a report says how far the session got. */
void frame(int frames) noexcept;

/* The session ended the way it was meant to. */
void finish() noexcept;

/* Development aid: prove the whole path end to end by faulting on purpose. */
void self_test(int kind) noexcept;

/* ---- sending a report to the server  ---- */

/* True when this console holds any crash/unfinished-run evidence. */
[[nodiscard]] bool report_available() noexcept;
/* True only when a real fatal crash report should interrupt startup. */
[[nodiscard]] bool report_waiting() noexcept;
/* Marks the current fatal report as offered so "Not now" does not nag every launch. */
void report_offered() noexcept;
/* A one-line description of it, for the button that offers to send it. */
[[nodiscard]] std::string report_summary() noexcept;
/* Everything worth sending, as one document. */
[[nodiscard]] std::string report_body() noexcept;
/* What kind it is, for the file name the server gives it: "crash", "hang". */
[[nodiscard]] std::string report_kind() noexcept;
/* Remembers that this one has been sent, so the offer stops coming back. */
void report_sent() noexcept;
/* Throws away the reports on this console. */
void report_forget() noexcept;

} // namespace slopfin::crash

#endif
