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
    /* Before anything that can fault, so the first crash is reported too. */
    slopfin::crash::install();

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
