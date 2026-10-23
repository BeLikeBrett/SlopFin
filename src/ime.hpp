/*
 * SlopFin - system on-screen keyboard.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Wraps the console's IME dialog. Derived from ProsperoTV's iptv_ime.c,
 * copyright BlackBearReloaded, GPL-3.0-or-later.
 *
 * The dialog must be started and polled from the render thread, so a request
 * is queued and serviced on the next frame.
 */

#ifndef SLOPFIN_IME_HPP
#define SLOPFIN_IME_HPP

#include <cstdint>
#include <string>

namespace slopfin::ime
{

enum class Mode : std::uint8_t
{
    text = 0,
    password,
    search,
};

bool initialize() noexcept;
void shutdown() noexcept;

/* Search uses one fixed placement below the field on Home and Discover.
   These are IME dialog coordinates; PS5 scales its panel for the output. */
inline constexpr float kSearchKeyboardX = 1080.0f;
inline constexpr float kSearchKeyboardY = 244.0f;

/* Queues a keyboard. Ignored while one is already open. Search placement is
   reapplied for every opening rather than supplied separately by each page. */
void request(const std::string &initial, const std::string &title, const std::string &placeholder,
             Mode mode) noexcept;

/* Drives the dialog; call once per frame. */
void poll() noexcept;

[[nodiscard]] bool busy() noexcept;
[[nodiscard]] bool available() noexcept;
bool take_cancelled() noexcept;

/*
 * True exactly once after the user confirms, with the entered text. Returns
 * false when the keyboard was cancelled.
 */
bool take_result(std::string &out) noexcept;

} // namespace slopfin::ime

#endif
