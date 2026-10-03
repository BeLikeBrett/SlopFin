/*
 * SlopFin - the signed-in user: the profile badge, its menu, and the Profile
 * screen with its picture picker and password change.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Modelled on the Jellyfin web client's user menu (Profile, Dashboard, Sign
 * Out), laid out for a controller. Everything that talks to the server runs on
 * the background thread.
 */

#ifndef SLOPFIN_ACCOUNT_HPP
#define SLOPFIN_ACCOUNT_HPP

#include <string>

namespace slopfin::account
{

/* Loads the signed-in user if that has not been done since sign-in. */
void ensure_user() noexcept;
/* Forgets the user, after signing out. */
void forget() noexcept;
[[nodiscard]] bool is_admin() noexcept;

/* ---- the badge at the top right: picture, name, and the Square button ---- */

[[nodiscard]] int badge_width() noexcept;
/* Right-aligned at `right`, `y` its top edge. */
void draw_badge(int right, int y) noexcept;

/* ---- its menu ---- */

enum class Choice : unsigned char
{
    none, /* still open */
    closed,
    profile,
    dashboard,
    sign_out,
};

[[nodiscard]] bool menu_open() noexcept;
/* True while it is still on screen, which includes the closing fade. */
[[nodiscard]] bool menu_visible() noexcept;
void open_menu() noexcept;
Choice handle_menu_input() noexcept;
/* Hangs under the badge whose right edge is `right` and whose foot is `top`. */
void draw_menu(int right, int top) noexcept;

/* ---- the Profile screen ---- */

void open_profile() noexcept;
/* none while the screen stays, closed to leave it, sign_out to sign out. */
Choice handle_profile_input() noexcept;
void draw_profile(const std::string &server_name) noexcept;

} // namespace slopfin::account

#endif
