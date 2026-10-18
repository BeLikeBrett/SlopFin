/*
 * SlopFin - application entry point.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "app.hpp"
#include "crash.hpp"
#include "gfx.hpp"
#include "pad.hpp"
#include "ime.hpp"
#include "player.hpp"
#include "telemetry.hpp"
#include "text.hpp"
#include "trace.hpp"
#include "update.hpp"

#include <cstdint>

extern "C" int sceKernelUsleep(std::uint32_t microseconds);
extern "C" int sceLncUtilKillLocalProcess(const char *reason);

namespace
{
/* Returning from main tears down the launch context, which the shell reports
   as a crash. Every failure path parks here instead. */
[[noreturn]] void park() noexcept
{
    for (;;)
        (void)sceKernelUsleep(1000000);
}
} // namespace

int main()
{
    slopfin::trace::begin();

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
            using namespace slopfin;
            pad::poll();
            gfx::clear(gfx::palette::background);
            text::draw(160, 330, "Can't open saved app data", 42, text::Weight::bold,
                       gfx::palette::text);
            text::draw(160, 410, "Your saved sign-in and settings have not been changed.", 26,
                       text::Weight::regular, gfx::palette::text_dim);
            text::draw(160, 465, "Start ELF Loader, then press X to retry.", 26,
                       text::Weight::regular, gfx::palette::text);
            gfx::present();
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
        if (slopfin::app::exit_requested())
        {
            slopfin::ime::shutdown();
            slopfin::player::stop();
            slopfin::gfx::shutdown();
            slopfin::crash::finish();
            slopfin::trace::mark("app: display released, stopping process");
            (void)sceLncUtilKillLocalProcess("SlopFin reload");
            park(); // Termination may return before ShellCore removes the process.
        }
        const std::uint64_t before = slopfin::app::now_us();
        slopfin::gfx::present();
        slopfin::app::note_present(slopfin::app::now_us() - before);
    }
}
