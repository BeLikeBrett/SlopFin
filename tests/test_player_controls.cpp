/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/app.cpp"
#include "../host/host_platform.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>

int main()
{
    using namespace slopfin;
    host::Options options;
    options.headless = true;
    assert(host::open_display(options) && gfx::initialize() && text::initialize() &&
           pad::initialize());
    config::load();
    auto press = [](pad::Button button, auto handler)
    {
        pad::inject(button);
        pad::poll();
        handler();
        pad::poll();
    };
    g_view.settings_page = 8;
    g_view.audio_row = 0;
    assert(config::current().audio_passthrough);
    press(pad::Button::cross, handle_settings_input);
    config::load();
    assert(!config::current().audio_passthrough);
    press(pad::Button::down, handle_settings_input);
    assert(g_view.audio_row == 1 && !player::software_audio_available());
    press(pad::Button::cross, handle_settings_input);
    assert(!config::current().software_audio);
    press(pad::Button::down, handle_settings_input);
    press(pad::Button::right, handle_settings_input);
    config::load();
    assert(config::current().dv_hdr10_base);
    g_view.audio_row = 0;
    for (int row = 0; row < 3; ++row)
    {
        g_view.audio_row = row;
        for (int frame = 0; frame < 90; ++frame)
        {
            ui::begin_frame(1.0f / 60.0f);
            gfx::clear(gfx::palette::background);
            draw_settings("Fixture");
        }
        gfx::present();
        assert(host::write_png("build/tests/audio-settings-" + std::to_string(row) + ".png"));
    }
    g_view.settings_page = 0;
    g_view.settings_index = 7;
    gfx::clear(gfx::palette::background);
    draw_settings("Fixture");
    gfx::present();
    assert(host::write_png("build/tests/settings-menu.png"));

    // Square has one consistent role during intros, with and without the timeline.
    for (bool controls : {false, true})
    {
        player::restart(jellyfin::PlaybackRequest{}, false);
        player::set_paused(false);
        g_view.osd_visible = controls;
        g_view.panel_open = false;
        g_view.intro_visible = true;
        g_view.intro_dismissed = false;
        g_view.playing_item.intro_end_seconds = 60.0;
        g_view.intro_in.jump(1.0f);
        gfx::clear(gfx::palette::background);
        draw_skip_intro();
        gfx::present();
        assert(host::write_png(controls ? "build/tests/intro-with-timeline.png"
                                        : "build/tests/intro-without-timeline.png"));
        press(pad::Button::cross, handle_player_input);
        assert(!g_view.intro_dismissed && player::current_request().start_seconds == 0.0);
        press(pad::Button::square, handle_player_input);
        assert(g_view.intro_dismissed && !g_view.intro_visible);
        assert(player::current_request().start_seconds == 60.0);
    }
    player::restart(jellyfin::PlaybackRequest{}, false);
    g_view.panel_open = true;
    g_view.panel_kind = 2;
    g_view.intro_visible = true;
    g_view.intro_dismissed = false;
    press(pad::Button::square, handle_player_input);
    assert(!g_view.intro_dismissed && player::current_request().start_seconds == 0.0);
    gfx::shutdown();
    host::close_display();
    std::cout << "player controls: settings persistence, disabled decoder, intro input passed\n";
}
