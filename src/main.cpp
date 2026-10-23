/*
 * SlopFin - application entry point.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "app.hpp"
#include "app_files.hpp"
#include "crash.hpp"
#include "gfx.hpp"
#include "icons.hpp"
#include "pad.hpp"
#include "ime.hpp"
#include "voice_search.hpp"
#include "player.hpp"
#include "reporter.hpp"
#include "telemetry.hpp"
#include "text.hpp"
#include "trace.hpp"
#include "ui_common.hpp"
#include "update.hpp"

#include <cstdint>

extern "C" int sceKernelUsleep(std::uint32_t microseconds);

namespace
{
/* Returning from main tears down the launch context, which the shell reports
   as a crash. Every failure path parks here instead. */
[[noreturn]] void park() noexcept
{
    for (;;)
        (void)sceKernelUsleep(1000000);
}

void draw_centered(int y, const char *line, int size, slopfin::text::Weight weight,
                   slopfin::gfx::Color colour) noexcept
{
    using namespace slopfin;
    text::draw((gfx::kWidth - text::measure(line, size, weight)) / 2, y, line, size, weight,
               colour);
}

/* Shown, before anything else, while the app's saved data cannot be opened:
   the same calm page as the first launch, saying what to do. */
void draw_storage_help() noexcept
{
    using namespace slopfin;
    using text::Weight;
    gfx::clear(ui::kPageBottom);
    gfx::vertical_gradient(0, 0, gfx::kWidth, ui::kPageRamp, ui::kHomeTheme.page_top,
                           ui::kPageBottom);
    constexpr int kMark = 120;
    icons::draw_gradient(icons::Icon::jellyfin, (gfx::kWidth - kMark) / 2, 250, kMark,
                         gfx::rgb(0xaa, 0x5c, 0xc3), gfx::rgb(0x00, 0xa4, 0xdc));
    draw_centered(420, "SlopFin can't open its saved data", 48, Weight::bold, gfx::palette::text);
    draw_centered(500, "Your sign-in and settings are safe and have not been changed.", 26,
                  Weight::regular, gfx::palette::text_dim);
    draw_centered(546, "Start your ELF loader, then try again.", 26, Weight::regular,
                  gfx::palette::text_dim);
    const char *const label = "Try again";
    const int chip = ui::button_chip_width(label);
    (void)ui::draw_button_chip((gfx::kWidth - chip) / 2, 640, icons::Icon::ps_cross, label, true);
}
} // namespace

int main()
{
    (void)slopfin::app_files::capture();
    slopfin::trace::begin();
    slopfin::trace::mark(
        "app files: kept " + std::to_string(slopfin::app_files::application.kept_count()) +
        " of 7, " +
        (slopfin::app_files::installed_package() ? "installed package" : "folder build"));

    if (!slopfin::gfx::initialize())
    {
        slopfin::trace::mark("gfx: FAILED");
        park();
    }
    slopfin::trace::mark("gfx: ready");

    if (!slopfin::text::initialize())
        slopfin::trace::mark("text: FAILED");
    else
        slopfin::trace::mark("text: ready");

    if (!slopfin::pad::initialize())
        slopfin::trace::mark("pad: FAILED");
    else
        slopfin::trace::mark("pad: ready");

    /* Load packaged fonts while /app0 is mounted, then recover shared data before
       config::load can mistake inaccessible settings for a new account. */
    while (!slopfin::update::prepare_storage())
    {
        do
        {
            slopfin::pad::poll();
            draw_storage_help();
            slopfin::gfx::present();
        } while (!slopfin::pad::pressed(slopfin::pad::Button::cross));
    }
    slopfin::trace::begin();
    slopfin::trace::mark("storage: shared settings readable and directory writable");
    slopfin::crash::install();
    slopfin::app::start();

    /*
     * The flip is timed separately from the drawing: it contains the wait for
     * vertical blank, so mixing the two hides where a stall actually is.
     */
    for (;;)
    {
        const std::uint64_t pad_started = slopfin::app::now_us();
        slopfin::pad::poll();
        slopfin::app::note_pad(slopfin::app::now_us() - pad_started);
        slopfin::app::frame();
        if (slopfin::app::exit_requested() || slopfin::crash::close_requested())
        {
            slopfin::voice_search::shutdown();
            slopfin::ime::shutdown();
            slopfin::player::stop();
            (void)slopfin::reporter::flush(2000);
            slopfin::gfx::shutdown();
            slopfin::crash::finish();
            slopfin::trace::mark("app: display released, stopping process");
            /* Self-exit is forbidden by the sandbox, and the launcher RPC can
               block. The privileged helper closes only this verified title,
               after receiving our acknowledgement that output is released. */
            if (!slopfin::update::finish_close())
                slopfin::trace::mark("app: close helper unavailable; close from the PS5 menu");
            park();
        }
        const std::uint64_t before = slopfin::app::now_us();
        slopfin::gfx::present();
        slopfin::app::note_present(slopfin::app::now_us() - before);
    }
}
