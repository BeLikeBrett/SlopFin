/*
 * SlopFin - Linux host backend, shared state.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The host build compiles the console's own renderer, input and screens. What
 * it replaces is the layer underneath: the PS5 C ABI those files already call.
 * Implementing that ABI rather than branching inside the renderer is what
 * keeps one copy of the drawing code, tile swizzle and pixel convention
 * included, so a change seen here is the change the console will show.
 */

#ifndef SLOPFIN_HOST_PLATFORM_HPP
#define SLOPFIN_HOST_PLATFORM_HPP

#include <cstdint>
#include <string>

namespace slopfin::host
{

/* How the window is opened. Headless skips SDL entirely, which is what the
   capture tooling uses; nothing about the drawn pixels differs. */
struct Options
{
    bool headless = false;
    int scale_percent = 100; /* window size relative to the 1920x1080 surface */
    bool vsync = true;
    /* Use a virtual 60 Hz frame clock for deterministic capture timing; screenshot I/O must not
     * stretch animations. */
    bool fixed_step = false;
};

bool open_display(const Options &options) noexcept;
void close_display() noexcept;

/* Pumps window and keyboard events into the pad state. False asks to quit. */
bool pump_events() noexcept;

/* One synthesized DualSense sample, in the console's own 120-byte layout. */
struct PadState
{
    std::uint32_t buttons = 0;
    std::uint8_t left_x = 128;
    std::uint8_t left_y = 128;
    std::uint8_t right_x = 128;
    std::uint8_t right_y = 128;
    std::uint8_t trigger_l = 0;
    std::uint8_t trigger_r = 0;
};

PadState &pad_state() noexcept;

/*
 * The last frame handed to the display, un-tiled back into scan-line order.
 * This is what the console would be sending over HDMI, not what the app meant
 * to draw, which is the distinction that cost this project a long
 * investigation once already.
 */
const std::uint32_t *scanout_pixels(int &width, int &height) noexcept;

/* Writes that frame as a PNG. Returns false when there is nothing to write. */
bool write_png(const std::string &path) noexcept;

/* Counts presents, so a capture can be asked for after a settled frame. */
std::uint64_t frames_presented() noexcept;

/* True once per F12, so a capture can be taken by hand while browsing. */
bool capture_requested() noexcept;

/* Creates a directory and every parent it needs. */
bool ensure_directory(const std::string &path) noexcept;

} // namespace slopfin::host

#endif
