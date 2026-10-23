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
    /* Audio/Video has one choice, Surround sound; Cross, Left and Right flip it. */
    g_view.settings_page = 8;
    assert(config::current().audio_passthrough);
    press(pad::Button::cross, handle_settings_input);
    config::load();
    assert(!config::current().audio_passthrough);
    press(pad::Button::down, handle_settings_input);
    assert(!config::current().audio_passthrough); /* there is no other row */
    for (int choice = 0; choice < 2; ++choice)
    {
        for (int frame = 0; frame < 90; ++frame)
        {
            ui::begin_frame(1.0f / 60.0f);
            gfx::clear(gfx::palette::background);
            draw_settings("Fixture");
        }
        gfx::present();
        assert(host::write_png("build/tests/audio-settings-" + std::to_string(choice) + ".png"));
        press(pad::Button::right, handle_settings_input);
    }
    config::load();
    assert(!config::current().audio_passthrough); /* flipped twice */
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
    // Square must restart the MKV reader when re-enabling a picture track;
    // asking the server to turn that track into SRT cannot work.
    jellyfin::PlaybackRequest picture_request;
    picture_request.item_id = "picture-fixture";
    assert(player::start(picture_request, "Picture fixture", 300));
    g_view.panel_open = false;
    g_view.intro_visible = false;
    g_view.osd_visible = false;
    jellyfin::Track bitmap_track;
    bitmap_track.index = 2;
    bitmap_track.codec = "pgssub";
    g_view.playing_source.subtitle_tracks = {bitmap_track};
    press(pad::Button::square, handle_player_input);
    assert(player::current_request().subtitle_index == 2 && player::status().subtitle_index == 2);
    press(pad::Button::square, handle_player_input);
    assert(player::status().subtitle_index == -1);
    press(pad::Button::square, handle_player_input);
    assert(player::current_request().subtitle_index == 2 && player::status().subtitle_index == 2);
    g_view.panel_kind = 0;
    bitmap_track.language = "eng";
    bitmap_track.label = "PGSUB";
    g_view.playing_source.subtitle_tracks = {bitmap_track, bitmap_track};
    g_view.playing_source.subtitle_tracks[1].index = 3;
    assert(panel_row_label(1) == "English · PGS · Track 1");
    assert(panel_row_label(2) == "English · PGS · Track 2");
    g_view.playing_source.subtitle_tracks = {bitmap_track};
    // Picture appearance controls use the same saved choices on Settings and in playback.
    g_view.settings_page = 1;
    const auto &appearance_rows = subtitle_rows();
    for (const auto field :
         {&subtitle_style::Choices::picture_size, &subtitle_style::Choices::picture_position})
    {
        for (int i = 0; i < static_cast<int>(appearance_rows.size()); ++i)
            if (appearance_rows[static_cast<std::size_t>(i)].field == field)
                g_view.subtitle_row = i;
        const int before = config::current().subtitle_look.*field;
        press(pad::Button::right, handle_subtitle_settings_input);
        config::load();
        assert(config::current().subtitle_look.*field == before + 1);
        for (int frame = 0; frame < 12; ++frame)
        {
            gfx::clear(gfx::palette::background);
            ui::begin_frame(1.0f / 60.0f);
            draw_subtitle_settings();
        }
        gfx::present();
        assert(host::write_png(field == &subtitle_style::Choices::picture_size
                                   ? "build/tests/subtitles-picture-size-settings.png"
                                   : "build/tests/subtitles-picture-position-settings.png"));
    }
    // Each reset affects only its own appearance section, and keeps track preference.
    int text_reset = -1, picture_reset = -1;
    for (int i = 0; i < static_cast<int>(appearance_rows.size()); ++i)
    {
        if (std::string_view(appearance_rows[i].label) == "Reset text appearance")
            text_reset = i;
        if (std::string_view(appearance_rows[i].label) == "Reset picture appearance")
            picture_reset = i;
    }
    assert(text_reset > 0 && picture_reset > text_reset);
    auto &look = config::current().subtitle_look;
    look.size = 4;
    look.colour = 3;
    look.picture_size = 4;
    look.picture_position = 3;
    look.picture = 1;
    look.edge_colour = 12;
    look.background_colour = 10;
    look.edge_width = 2;
    look.edge_opacity = 0;
    g_view.subtitle_row = text_reset;
    press(pad::Button::cross, handle_subtitle_settings_input);
    config::load();
    assert(look.size == 1 && look.colour == 0 && look.picture_size == 4 &&
           look.picture_position == 3 && look.picture == 1);
    assert(look.edge_width == 1 && look.edge_opacity == 1 && look.edge_colour == 0 &&
           look.background_colour == 0);
    look.size = 3;
    look.colour = 2;
    g_view.subtitle_row = picture_reset;
    press(pad::Button::cross, handle_subtitle_settings_input);
    config::load();
    assert(look.size == 3 && look.colour == 2 && look.picture_size == 1 &&
           look.picture_position == 0 && look.picture == 1);
    const auto row_for = [&](int subtitle_style::Choices::*field)
    {
        for (int i = 0; i < static_cast<int>(appearance_rows.size()); ++i)
            if (appearance_rows[static_cast<std::size_t>(i)].field == field)
                return i;
        return -1;
    };
    look.edge = 0;
    look.background = 1;
    for (const auto field :
         {&subtitle_style::Choices::colour, &subtitle_style::Choices::edge_colour,
          &subtitle_style::Choices::background_colour})
    {
        g_view.subtitle_row = row_for(field);
        assert(g_view.subtitle_row > 0);
        const auto before = look;
        press(pad::Button::right, handle_subtitle_settings_input);
        config::load();
        assert(look.*field == before.*field + 1);
        assert(look.colour == before.colour + (field == &subtitle_style::Choices::colour));
        assert(look.edge_colour ==
               before.edge_colour + (field == &subtitle_style::Choices::edge_colour));
        assert(look.background_colour ==
               before.background_colour + (field == &subtitle_style::Choices::background_colour));
    }
    look.edge = 2;
    g_view.subtitle_row = row_for(&subtitle_style::Choices::edge);
    press(pad::Button::down, handle_subtitle_settings_input);
    assert(g_view.subtitle_row == row_for(&subtitle_style::Choices::background));
    look.background = 0;
    press(pad::Button::down, handle_subtitle_settings_input);
    assert(g_view.subtitle_row == row_for(&subtitle_style::Choices::spacing));
    // The right stick visits selectable rows, including the independent picture reset.
    g_view.subtitle_row = 1;
    pad::inject_scroll(1.0f, 240);
    for (int frame = 0; frame < 240; ++frame)
    {
        pad::poll();
        handle_subtitle_settings_input();
        assert(
            subtitle_row_enabled(appearance_rows[static_cast<std::size_t>(g_view.subtitle_row)]));
    }
    assert(g_view.subtitle_row == static_cast<int>(appearance_rows.size()) - 1);
    pad::inject_scroll(0.0f, 1);
    pad::poll();
    handle_subtitle_settings_input();
    for (int edge = 0; edge < 4; ++edge)
    {
        look.edge = edge;
        look.size = edge == 3 ? 4 : 1;
        look.colour = 0;
        g_view.subtitle_row = row_for(&subtitle_style::Choices::edge);
        g_view.subtitle_scroll.jump(0.0f);
        for (int frame = 0; frame < 24; ++frame)
        {
            gfx::clear(gfx::palette::background);
            ui::begin_frame(1.0f / 60.0f);
            draw_subtitle_settings();
        }
        gfx::present();
        assert(
            host::write_png("build/tests/subtitle-settings-edge-" + std::to_string(edge) + ".png"));
    }
    // Colour preview exercises the actual caption renderer, not a separate mock.
    for (int i = 0; i < 3; ++i)
    {
        look.font = i + 3;
        look.bold = i == 1;
        look.size = 1;
        look.edge = 0;
        look.edge_width = 1;
        look.colour = i == 0 ? 2 : i == 1 ? 10 : 14;
        look.edge_colour = i == 0 ? 2 : i == 1 ? 10 : 0;
        look.background = 1;
        look.background_colour = i == 0 ? 13 : i == 1 ? 10 : 11;
        g_view.subtitle_row = row_for(&subtitle_style::Choices::background_colour);
        for (int frame = 0; frame < 45; ++frame)
        {
            gfx::clear(gfx::palette::background);
            ui::begin_frame(1.0f / 60.0f);
            g_view.subtitle_scroll.step(motion::kScroll, 1.0f / 60.0f);
            draw_subtitle_settings();
        }
        gfx::present();
        assert(host::write_png("build/tests/subtitle-colours-" + std::to_string(i) + ".png"));
    }
    player::Status picture_status = player::status();
    picture_status.subtitle_index = 2;
    picture_status.subtitle_bitmap = true;
    note_subtitle_state(picture_status);
    g_view.panel_kind = 0;
    g_view.panel_open = true;
    const int first_picture =
        1 + static_cast<int>(g_view.playing_source.subtitle_tracks.size()) + 4;
    assert(timing_row(first_picture) == TimingRow::picture_size);
    g_view.panel_index = first_picture;
    /* The row shows the current choice; the reset above left it at Original. */
    assert(panel_row_label(first_picture).find("Original") != std::string::npos);
    look.picture_size = 2;
    assert(panel_row_label(first_picture).find("Larger") != std::string::npos);
    assert(panel_rows() == 1 + static_cast<int>(g_view.playing_source.subtitle_tracks.size()) + 7);
    picture_status.subtitle_burned = true;
    picture_status.subtitle_bitmap = false;
    note_subtitle_state(picture_status);
    assert(timing_row(first_picture) == TimingRow::none);
    // A full episode toolbar stays within the safe area, including a long resume time.
    jellyfin::Item detail_item;
    detail_item.id = "scroll-fixture";
    detail_item.name = "Episode fixture";
    detail_item.type = "Episode";
    detail_item.series_id = "fixture-series";
    detail_item.runtime_minutes = 20;
    detail_item.resume_ticks = 600.0 * 10000000.0;
    detail_item.sources.resize(1);
    detail_item.sources[0].id = "fixture-source";
    for (int i = 0; i < 45; ++i)
    {
        jellyfin::Track track;
        track.index = i;
        track.codec = "subrip";
        track.label = "Subtitle " + std::to_string(i + 1);
        detail_item.sources[0].subtitle_tracks.push_back(track);
    }
    const auto detail_buttons = detail_actions(detail_item);
    assert(detail_buttons.size() == 5 && detail_buttons.front().progress == 0.5f);
    assert(detail_buttons.front().remaining == "10m left");
    int toolbar_width = 12;
    for (const auto &button : detail_buttons)
        toolbar_width += detail_action_width(button) + kDetailActionGap;
    assert(toolbar_width < kContentRight - kSafeX);
    for (int focused : {0, 1})
    {
        gfx::clear(gfx::rgb(20, 25, 32));
        int x = 100;
        for (int i = 0; i < static_cast<int>(detail_buttons.size()); ++i)
        {
            draw_detail_action(x, 200, detail_buttons[static_cast<std::size_t>(i)], i == focused);
            x +=
                detail_action_width(detail_buttons[static_cast<std::size_t>(i)]) + kDetailActionGap;
        }
        gfx::present();
        assert(
            host::write_png("build/tests/play-button-focus-" + std::to_string(focused) + ".png"));
    }

    g_view.detail_source = 0;
    g_view.detail_action = static_cast<int>(detail_buttons.size()) - 1;
    g_view.details_open = true;
    g_view.details_scroll = 0.0f;
    g_view.details_offset.jump(0.0f);
    g_view.details_glide.stop();
    gfx::clear(gfx::palette::background);
    draw_details_sheet(detail_item);
    assert(g_view.details_limit > kDetailsViewport);
    auto details_input = [&] { handle_detail_input(detail_item, {}, {}); };
    press(pad::Button::down, details_input);
    assert(g_view.details_scroll == kDetailsLine && g_view.details_offset.value() == 0.0f);
    for (int frame = 0; frame < 6; ++frame)
        g_view.details_offset.step(motion::kScroll, 1.0f / 60.0f);
    assert(g_view.details_offset.value() > 0.0f &&
           g_view.details_offset.value() < g_view.details_scroll);

    // Holding the d-pad ramps to a much faster continuous scroll. A tap above
    // still moved exactly one line. Release and reversal must reset the ramp.
    const float real_details_limit = g_view.details_limit;
    g_view.details_limit = 10000.0f;
    pad::inject_hold(pad::Button::down, 90);
    float first_half = 0.0f;
    for (int frame = 0; frame < 90; ++frame)
    {
        pad::poll();
        scroll_details_sheet(0.0f, 1.0f / 60.0f);
        g_view.details_offset.step(motion::kScroll, 1.0f / 60.0f);
        if (frame == 44)
            first_half = g_view.details_scroll;
    }
    assert(g_view.details_scroll - first_half > first_half && g_view.details_scroll > 1600.0f);
    pad::poll();
    scroll_details_sheet(0.0f, 1.0f / 60.0f);
    assert(g_view.details_hold_time == 0.0f && g_view.details_hold_direction == 0);
    g_view.details_limit = real_details_limit;
    g_view.details_scroll = 0.0f;
    g_view.details_offset.jump(0.0f);

    // Continuous stick movement starts at the rendered position; it coasts, then stops.
    for (int frame = 0; frame < 30; ++frame)
        scroll_details_sheet(1.0f, 1.0f / 60.0f);
    const float released_at = g_view.details_scroll;
    scroll_details_sheet(0.0f, 1.0f / 60.0f);
    assert(g_view.details_scroll > released_at);
    for (int frame = 0; frame < 90; ++frame)
        scroll_details_sheet(0.0f, 1.0f / 60.0f);
    assert(!g_view.details_glide.moving());
    for (int frame = 0; frame < 600; ++frame)
        scroll_details_sheet(1.0f, 1.0f / 60.0f);
    assert(g_view.details_scroll == g_view.details_limit);
    for (int frame = 0; frame < 60; ++frame)
        scroll_details_sheet(-1.0f, 1.0f / 60.0f);
    assert(g_view.details_scroll >= 0.0f && g_view.details_scroll < g_view.details_limit);
    press(pad::Button::down, details_input);
    assert(!g_view.details_glide.moving());
    press(pad::Button::circle, details_input);
    assert(!g_view.details_open && !g_view.details_glide.moving());

    // The media page also accepts analog scrolling; the d-pad takes focus back smoothly.
    detail_item.people.resize(2);
    const std::vector<jellyfin::Item> episode_row(8, detail_item);
    g_view.detail_action = 0;
    g_view.detail_page.jump(0.0f);
    g_view.detail_page_manual = false;
    g_view.detail_page_glide.stop();
    pad::inject_scroll(1.0f, 60);
    for (int frame = 0; frame < 60; ++frame)
    {
        pad::poll();
        handle_detail_input(detail_item, episode_row, {});
    }
    assert(g_view.detail_page_manual && g_view.detail_page.value() > 0.0f);
    const float page_at = g_view.detail_page.value();
    pad::inject_scroll(0.0f, 1);
    pad::poll();
    press(pad::Button::left, [&] { handle_detail_input(detail_item, episode_row, {}); });
    assert(!g_view.detail_page_manual && g_view.detail_page.value() == page_at);
    // Circle hides the OSD first; leaving a direct play must create the title
    // page instead of displaying an empty, non-loading detail model.
    jellyfin::Item direct_play;
    direct_play.id = "return-direct-play";
    direct_play.name = "Direct play fixture";
    direct_play.type = "Movie";
    g_view.playing_item = direct_play;
    g_view.intro_visible = false;
    g_view.finished = false;
    g_view.changing_episode = false;
    g_view.autoplay.chosen_by_hand();
    g_view.osd_visible = true;
    g_view.osd_focus = View::OsdFocus::bar;
    g_view.panel_open = false;
    g_shared.screen = Screen::playing;
    g_shared.detail_item = {};
    g_shared.detail_loading = false;
    g_back.clear();
    press(pad::Button::circle, handle_player_input);
    assert(g_shared.screen == Screen::playing && !g_view.osd_visible);
    press(pad::Button::circle, handle_player_input);
    assert(g_shared.screen == Screen::detail && g_shared.detail_item.id == direct_play.id);
    assert(g_shared.detail_target_id == direct_play.id && g_shared.detail_loading);
    assert(g_back.empty());
    press(pad::Button::circle, [&] { handle_detail_input(g_shared.detail_item, {}, {}); });
    assert(g_shared.screen == Screen::home);

    // An existing series/season page keeps its position when an episode ends.
    g_shared.detail_item.id = "return-series";
    g_shared.detail_item.type = "Series";
    g_shared.detail_target_id = "return-series";
    g_shared.screen = Screen::playing;
    g_view.detail_page.jump(173.0f);
    g_view.detail_episode = 4;
    g_view.finished = true;
    press(pad::Button::circle, handle_player_input);
    assert(g_shared.screen == Screen::detail && g_shared.detail_item.id == "return-series");
    assert(g_view.detail_page.value() == 173.0f && g_view.detail_episode == 4);
    assert(!g_view.finished);

    // If playback metadata itself is unavailable, Circle has a safe Home fallback.
    g_shared.screen = Screen::playing;
    g_shared.detail_item = {};
    g_view.playing_item = {};
    g_view.osd_visible = false;
    press(pad::Button::circle, handle_player_input);
    assert(g_shared.screen == Screen::home);

    // Autoplay is always at the far right, after Diagnostics, and appears only
    // for episodes with a series. Optional track controls keep their order.
    g_view.playing_item = detail_item;
    g_view.playing_source = detail_item.sources.front();
    g_view.playing_source.audio_tracks.resize(1);
    const auto full_player_buttons = player_buttons();
    const std::vector<PlayerAction> expected_player_actions = {
        PlayerAction::subtitles, PlayerAction::audio, PlayerAction::quality, PlayerAction::debug,
        PlayerAction::autoplay};
    assert(full_player_buttons.size() == expected_player_actions.size());
    for (std::size_t i = 0; i < expected_player_actions.size(); ++i)
        assert(full_player_buttons[i].action == expected_player_actions[i]);
    g_view.playing_source = {};
    const auto no_track_buttons = player_buttons();
    assert(no_track_buttons.size() == 3 && no_track_buttons[1].action == PlayerAction::debug &&
           no_track_buttons.back().action == PlayerAction::autoplay);
    g_view.playing_item.type = "Movie";
    assert(player_buttons().size() == 2 && player_buttons().back().action == PlayerAction::debug);

    // Marking watched clears Resume everywhere without changing another item.
    // Start over remains a separate action while a title has resume progress.
    detail_item.played = false;
    detail_item.played_percentage = 50.0;
    jellyfin::Item untouched = detail_item;
    untouched.id = "different-item";
    g_shared.detail_item = detail_item;
    g_shared.detail_episodes = {detail_item, untouched};
    g_shared.detail_next_up = detail_item;
    g_shared.grid_items = {detail_item, untouched};
    g_shared.search_items = {detail_item};
    g_shared.rows = {{"Continue Watching", {detail_item, untouched}, true},
                     {"Latest", {detail_item}, false}};
    g_shared.siblings = {detail_item};
    g_shared.pending_play = detail_item;
    g_shared.progress_focus_changes.clear();
    remember_detail(detail_item);
    remember_detail_rows("fixture-series", {detail_item, untouched}, detail_item);
    g_view.detail_action = 2; /* Mark watched, after Resume and Start over */
    g_view.details_open = false;
    g_view.detail_page_manual = false;
    g_shared.request = Request::none;
    auto before_mark = detail_actions(detail_item);
    assert(before_mark[1].action == DetailAction::restart &&
           detail_action_label(before_mark[1]) == "Start over");
    assert(before_mark[2].label == "Mark watched");
    press(pad::Button::cross, [&] { handle_detail_input(detail_item, {}, {}); });
    assert(g_shared.request == Request::mark_played && g_shared.request_text == detail_item.id);
    // Submission alone is not optimistic success: failed HTTP keeps this state.
    assert(!g_shared.detail_item.played && g_shared.detail_item.resume_ticks > 0.0);
    apply_marked_progress(detail_item.id, jellyfin::UserProgress{true, 0.0, 100.0});
    const auto assert_marked = [](const jellyfin::Item &item)
    { assert(item.played && item.resume_ticks == 0.0 && item.played_percentage == 100.0); };
    assert_marked(g_shared.detail_item);
    assert_marked(g_shared.detail_episodes[0]);
    assert_marked(g_shared.detail_next_up);
    assert_marked(g_shared.grid_items[0]);
    assert_marked(g_shared.search_items[0]);
    assert_marked(g_shared.rows[1].items[0]);
    assert_marked(g_shared.siblings[0]);
    assert_marked(g_shared.pending_play);
    assert_marked(*cached_detail(detail_item.id));
    assert_marked(cached_detail_rows("fixture-series")->entries[0]);
    assert_marked(cached_detail_rows("fixture-series")->next_up);
    assert(g_shared.grid_items[1].resume_ticks == detail_item.resume_ticks &&
           !g_shared.grid_items[1].played);
    assert(g_shared.rows[0].items.size() == 1 && g_shared.rows[0].items[0].id == untouched.id);
    auto after_mark = detail_actions(g_shared.detail_item);
    assert(after_mark.size() == before_mark.size() - 1 && after_mark[0].label == "Play" &&
           after_mark[1].action == DetailAction::watched &&
           after_mark[1].label == "Mark unwatched");
    assert(g_shared.progress_focus_changes.size() == 1);
    g_detail_positions[detail_item.id].action = 2;
    g_back = {Back{}};
    g_back.back().screen = Screen::detail;
    g_back.back().id = detail_item.id;
    g_back.back().action = 4; /* Details */
    reconcile_progress_focus(g_shared.progress_focus_changes.front());
    assert(g_view.detail_action == 1 && g_detail_positions[detail_item.id].action == 1 &&
           g_back.back().action == 3);
    assert(after_mark[static_cast<std::size_t>(g_view.detail_action)].action ==
           DetailAction::watched);
    // Undoing watched status is explicitly named, clears progress too, and
    // does not bring back an old Resume or Start over button.
    press(pad::Button::cross, [&] { handle_detail_input(g_shared.detail_item, {}, {}); });
    assert(g_shared.request == Request::mark_unplayed);
    apply_marked_progress(detail_item.id, jellyfin::UserProgress{false, 0.0, 0.0});
    after_mark = detail_actions(g_shared.detail_item);
    assert(after_mark[0].label == "Play" && after_mark[1].label == "Mark watched" &&
           g_shared.detail_item.played_percentage == 0.0 && !g_shared.detail_item.played);
    assert(g_shared.progress_focus_changes.size() == 1); /* no second toolbar shift */
    g_shared.rows[0].items = {detail_item};
    const auto previous_rows_epoch = g_shared.rows_epoch;
    apply_marked_progress(detail_item.id, jellyfin::UserProgress{true, 0.0, 100.0});
    assert(g_shared.rows.size() == 1 && g_shared.rows[0].title == "Latest" &&
           g_shared.rows_epoch > previous_rows_epoch);
    assert(g_shared.progress_focus_changes.size() == 1); /* stale card cannot shift current focus */

    // First-time Play keeps the same complete circular affordance as Resume.
    gfx::clear(gfx::rgb(20, 25, 32));
    draw_detail_action(100, 200, {DetailAction::play, "Play"}, false);
    assert(pixel(113, 228) != gfx::rgb(20, 25, 32));
    gfx::present();
    assert(host::write_png("build/tests/play-first-time.png"));

    // The last displayed fractional position updates every local view at once,
    // including a brand-new Continue Watching entry and the cached title page.
    detail_item.resume_ticks = 0;
    detail_item.played = false;
    g_view.playing_item = detail_item;
    g_view.finished = false;
    g_shared.detail_item = detail_item;
    g_shared.rows = {{"Latest", {detail_item}, false}};
    player::Status stopped;
    stopped.state = player::State::playing;
    stopped.frames = 240;
    stopped.position_seconds = 601.2345678;
    stopped.duration_seconds = 1200.125;
    remember_playback_progress(stopped);
    const auto exact = [&](const jellyfin::Item &item, double seconds)
    { assert(std::abs(item.resume_ticks / 10000000.0 - seconds) < 0.0000001); };
    exact(g_shared.detail_item, stopped.position_seconds);
    exact(*cached_detail(detail_item.id), stopped.position_seconds);
    exact(g_shared.grid_items[0], stopped.position_seconds);
    exact(g_shared.search_items[0], stopped.position_seconds);
    exact(cached_detail_rows("fixture-series")->entries[0], stopped.position_seconds);
    assert(g_shared.rows.front().title == "Continue Watching");
    exact(g_shared.rows.front().items.front(), stopped.position_seconds);
    auto resumed = detail_actions(g_shared.detail_item);
    assert(resumed[0].label.find("Resume") == 0 && resumed[1].action == DetailAction::restart);
    // Seeking backward must save the new position rather than keeping a maximum.
    stopped.position_seconds = 180.125;
    remember_playback_progress(stopped);
    exact(g_shared.detail_item, stopped.position_seconds);
    assert(g_shared.rows.front().items.size() == 1);
    stopped.state = player::State::failed;
    stopped.frames = 0;
    stopped.position_seconds = 0;
    remember_playback_progress(stopped);
    exact(g_shared.detail_item, 180.125); // A failed opening cannot erase Resume.
    stopped.state = player::State::ended;
    stopped.position_seconds = stopped.duration_seconds;
    remember_playback_progress(stopped);
    assert(g_shared.detail_item.resume_ticks == 0 && g_shared.detail_item.played);
    assert(g_shared.rows.size() == 1 && g_shared.rows.front().title == "Latest");

    gfx::shutdown();
    host::close_display();
    std::cout
        << "player controls: settings, intro input, detail toolbar and smooth scrolling passed\n";
}
