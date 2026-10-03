/*
 * SlopFin - artwork fetching, decoding and caching.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Posters are fetched and decoded on a worker thread. The UI asks for one by
 * item id every frame and gets either a bitmap or nothing, never a stall.
 */

#ifndef SLOPFIN_IMAGES_HPP
#define SLOPFIN_IMAGES_HPP

#include "gfx.hpp"

#include <string>

namespace slopfin::images
{

enum class Kind : std::uint8_t
{
    primary = 0,
    backdrop,
    logo,
    /* A Jellyfin user's picture: the id is the user's, and the tag is part of
       the key, because replacing a picture keeps the id.  */
    user,
    /* A JPEG or PNG on the console: the id is its path. Decoded and shrunk to
       the height asked for, so a grid of 4K screenshots stays small. The tag
       names the size, since one picture is wanted as a thumbnail and larger. */
    file,
};

bool start() noexcept;

/*
 * The cores background work belongs on: everything the process may use except
 * the ones the render thread, the row workers and the video decoder occupy.
 * A thread inherits its creator's affinity, and anything created after gfx
 * starts inherits the render core, so background threads should be given
 * this. 0 where it cannot be known.
 */
std::uint64_t spare_cores() noexcept;

/*
 * Returns the artwork if it is decoded, otherwise nullptr and queues a fetch.
 * Safe to call every frame for every visible card.
 */
const gfx::Bitmap *acquire(const std::string &item_id, const std::string &tag, Kind kind,
                           int target_height) noexcept;

bool failed(const std::string &item_id, const std::string &tag, Kind kind) noexcept;

/*
 * Asks for artwork that is not being drawn yet, so it is already decoded by the
 * time it scrolls into view. A card only reaches `acquire` once it is on
 * screen, which guarantees a placeholder is shown first however fast the
 * server is; the rows just outside the view are warmed through here instead.
 */
void prefetch(const std::string &item_id, const std::string &tag, Kind kind,
              int target_height) noexcept;

/*
 * Asks for a poster nobody is looking at yet, so it is already in memory when
 * they do. Served only when nothing on screen is waiting, by at most two of
 * the three workers, and never during playback. `soon` puts it ahead of the
 * rest of the warming -- for a page that has just opened, rather than for the
 * whole library.
 */
void warm(const std::string &item_id, const std::string &tag, int target_height,
          bool soon = false) noexcept;

/* Bytes of artwork held, for the session record a crash report leaves. */
[[nodiscard]] std::size_t cached_bytes() noexcept;

/*
 * Playback raises how much free memory the cache must leave and pauses
 * warming. Beginning releases what it has to at once, before the stream
 * allocates; ending lets warming carry on.
 */
void set_playback(bool playing) noexcept;

/*
 * Releases artwork oldest-first while the cache is over its ceiling or the
 * console is under its free-memory floor. When there is room, keeps
 * everything.
 */
void trim() noexcept;

} // namespace slopfin::images

#endif
