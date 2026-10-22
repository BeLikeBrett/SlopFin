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
    // A long focus move must reach its destination within 150 ms.
    ui::begin_frame(1.0f / 60.0f);
    ui::focus_pill(100, 100, 300, 60, ui::FocusGroup::settings, 777);
    for (int frame = 0; frame < 9; ++frame)
    {
        gfx::clear(gfx::palette::background);
        ui::begin_frame(1.0f / 60.0f);
        ui::focus_pill(100, 700, 300, 60, ui::FocusGroup::settings, 777);
    }
    const auto *pixels = gfx::back_buffer();
    const auto pixel = [&](int x, int y)
    { return pixels[gfx::to_physical_y(y) * gfx::physical_width() + gfx::to_physical_x(x)]; };
    assert(pixel(200, 750) != gfx::palette::background);
    assert(pixel(200, 150) == gfx::palette::background);
    g_view.settings_page = 8;
    g_view.audio_row = 0;
    assert(config::current().audio_passthrough);
    press(pad::Button::cross, handle_settings_input);
    config::load();
    assert(!config::current().audio_passthrough);
    press(pad::Button::down, handle_settings_input);
    assert(g_view.audio_row == 1 && !player::software_audio_available());
    // Without FFmpeg in the build the row says Not included and cannot change.
    const bool software_before = config::current().software_audio;
    press(pad::Button::cross, handle_settings_input);
    assert(config::current().software_audio == software_before);
    press(pad::Button::down, handle_settings_input);
    const bool dv_before = config::current().dv_hdr10_base;
    assert(dv_before); /* on by default since 1.17 */
    press(pad::Button::right, handle_settings_input);
    config::load();
    assert(config::current().dv_hdr10_base == !dv_before);
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
    const std::vector<std::pair<std::string, int>> expected_settings = {
        {"Account", 5},     {"Subtitles", 1}, {"Playback", 2}, {"Audio/Video", 8},
        {"Navigation", 10}, {"Discover", 9},  {"Updates", 7},  {"Diagnostics", 6}};
    assert(kSettingsMenuCount == static_cast<int>(expected_settings.size()));
    for (int i = 0; i < kSettingsMenuCount; ++i)
    {
        assert(kSettingsMenu[i].name == expected_settings[static_cast<std::size_t>(i)].first);
        g_view.settings_index = i;
        press(pad::Button::cross, handle_settings_input);
        assert(g_view.settings_page == expected_settings[static_cast<std::size_t>(i)].second);
        press(pad::Button::circle, handle_settings_input);
    }
    g_view.settings_index = 0;
    for (int frame = 0; frame < 120; ++frame)
        scroll_settings_menu(1.0f, 1.0f / 60.0f);
    assert(g_view.settings_index == kSettingsMenuCount - 1);
    scroll_settings_menu(0, 1.0f / 60.0f);
    scroll_settings_menu(-1, 1.0f / 60.0f);
    assert(g_view.settings_index == kSettingsMenuCount - 2);
    for (int frame = 0; frame < 120; ++frame)
        scroll_settings_menu(-1.0f, 1.0f / 60.0f);
    assert(g_view.settings_index == 0);
    scroll_settings_menu(0, 1.0f / 60.0f);
    g_view.settings_index = kSettingsMenuCount - 1;
    gfx::clear(gfx::palette::background);
    draw_settings("Fixture");
    gfx::present();
    assert(host::write_png("build/tests/settings-menu.png"));

    g_shared.libraries = {{"movies", "Movies", "movies"}, {"tv", "Shows", "tvshows"}};
    // Discover is a tab of its own now: the Home tab's sidebar is its
    // overview, the libraries and Settings, and Discover lists its categories.
    auto entries = build_sidebar(g_shared.libraries);
    assert(entries.size() == 4 && category_key(entries.front()) == "home");
    assert(category_key(entries.back()) == "settings");
    assert(discover_sidebar().size() == static_cast<std::size_t>(seerr::kSectionCount));
    assert(discover_sidebar().back().gap_before &&
           discover_sidebar().back().section == seerr::kRequestsSection);
    assert(discover_tab_shown());
    assert(category_key(navigation_entries().front()) == "discover");
    g_view.settings_page = 10;
    g_view.navigation_index = 0;
    press(pad::Button::cross, handle_settings_input);
    assert(g_view.settings_page == 3);
    press(pad::Button::circle, handle_settings_input);
    assert(g_view.settings_page == 10);
    press(pad::Button::down, handle_settings_input);
    press(pad::Button::r1, handle_settings_input);
    assert(config::current().navigation_order.front() == "home" && g_view.navigation_index == 2);
    press(pad::Button::cross, handle_settings_input);
    entries = build_sidebar(g_shared.libraries);
    assert(entries.size() == 4 && category_key(entries.front()) == "home");
    config::load();
    assert(category_hidden("discover") && !discover_tab_shown());
    config::current().navigation_hidden = {"home", "discover", "library:movies", "library:tv",
                                           "settings"};
    entries = build_sidebar(g_shared.libraries);
    assert(entries.size() == 1 && category_key(entries.front()) == "settings");
    g_shared.libraries[0].name = "Renamed";
    g_shared.libraries.push_back({"new", "New library", "movies"});
    entries = build_sidebar(g_shared.libraries);
    assert(entries.size() == 2 && category_key(entries.front()) == "library:new");
    assert(navigation_entries().size() == 5);
    press(pad::Button::square, handle_settings_input);
    assert(config::current().navigation_hidden.empty() &&
           config::current().navigation_order.empty());
    config::load();
    assert(config::current().navigation_hidden.empty() &&
           config::current().navigation_order.empty());
    for (const int index : {0, 1, 4})
    {
        g_view.navigation_index = index;
        for (int frame = 0; frame < 90; ++frame)
        {
            gfx::clear(gfx::palette::background);
            ui::begin_frame(1.0f / 60.0f);
            draw_settings("Fixture");
        }
        gfx::present();
        assert(host::write_png("build/tests/navigation-" + std::to_string(index) + ".png"));
    }
    g_view.settings_page = 0;

    // Returning to a library keeps its cards and viewport without another request.
    g_shared.screen = Screen::detail;
    g_shared.grid_library_id = "fixture-library";
    jellyfin::Item card;
    card.id = "fixture-card";
    g_shared.grid_items = {card};
    g_shared.grid_total = 120;
    g_back = {Back{}};
    g_back.back().screen = Screen::grid;
    g_back.back().id = "fixture-library";
    g_back.back().title = "Fixture";
    g_back.back().index = 12;
    g_back.back().scroll = 200.0f;
    assert(go_back());
    assert(g_shared.screen == Screen::grid && !g_shared.grid_loading);
    assert(g_shared.grid_items.size() == 1 && g_shared.grid_items[0].id == "fixture-card");
    assert(g_shared.grid_total == 120 && g_view.grid_index == 12);
    assert(g_view.grid_scroll.value() == 200.0f && g_view.restoring_page);
    g_view.restoring_page = false;
    g_detail_cache.clear();
    g_detail_cache_order.clear();
    for (int i = 0; i < 50; ++i)
    {
        card.id = "fixture-" + std::to_string(i);
        remember_detail(card);
    }
    assert(g_detail_cache.size() == 48);
    assert(!cached_detail("fixture-0") && cached_detail("fixture-49"));
    assert(cached_detail("fixture-30"));

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
