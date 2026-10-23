/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/seerr.cpp"
#include "../host/host_platform.hpp"
#include "fake_voice_search.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>

namespace slopfin::ime
{
bool busy() noexcept
{
    return false;
}
void request(const std::string &, const std::string &, const std::string &, Mode) noexcept
{
}
bool take_result(std::string &) noexcept
{
    return false;
}
bool take_cancelled() noexcept
{
    return false;
}
} // namespace slopfin::ime

int main()
{
    using namespace slopfin;
    using namespace slopfin::seerr;
    host::Options options;
    options.headless = true;
    assert(host::open_display(options) && gfx::initialize() && text::initialize());
    config::load();
    config::current().seerr_host = "https://requests.example.invalid";
    std::filesystem::create_directories("build/tests");
    g_view.entered = true;
    g_view.mode = 0;
    g_view.media_filter = 1;
    g_view.search_open = true;
    g_view.field_focused = true;
    g_view.search_loading = false;
    g_view.refresh_at = std::chrono::steady_clock::now() + std::chrono::hours(1);
    g_view.query.clear();
    g_view.results.clear();
    for (bool populated : {false, true})
    {
        if (populated)
        {
            Title film;
            film.tmdb_id = 1;
            film.name = "TRON: Legacy";
            film.media_type = "movie";
            g_view.results = {film};
            g_view.query = "TRON";
            g_view.field_focused = false;
        }
        for (int frame = 0; frame < 24; ++frame)
        {
            ui::begin_frame(1.0f / 60.0f);
            gfx::clear(gfx::palette::background);
            draw(true);
        }
        // Search advertises R3 Speak but never the catalogue's filter chips.
        const auto *pixels = gfx::back_buffer();
        int speak_ink = 0;
        for (int y = gfx::to_physical_y(ui::kSearchFieldY - 10);
             y < gfx::to_physical_y(ui::kSearchFieldY + ui::kSearchFieldH + 10); ++y)
        {
            for (int x = gfx::to_physical_x(ui::kContentX + kSearchFieldW + 20);
                 x < gfx::to_physical_x(ui::kContentX + kSearchFieldW + 200); ++x)
                speak_ink += pixels[y * gfx::physical_width() + x] != gfx::palette::background;
            for (int x = gfx::to_physical_x(ui::kContentX + kSearchFieldW + 210);
                 x < gfx::to_physical_x(ui::kContentRight); ++x)
                assert(pixels[y * gfx::physical_width() + x] == gfx::palette::background);
        }
        assert(speak_ink > 0);
        if (populated)
            for (int y = gfx::to_physical_y(ui::kSearchFieldY + ui::kSearchFieldH + 34);
                 y < gfx::to_physical_y(catalog_grid_top() - 20); ++y)
                for (int x = gfx::to_physical_x(ui::kContentX);
                     x < gfx::to_physical_x(ui::kContentRight); ++x)
                    assert(pixels[y * gfx::physical_width() + x] == gfx::palette::background);
        gfx::present();
        assert(host::write_png(populated ? "build/tests/discover-search-results.png"
                                         : "build/tests/discover-search-empty.png"));
    }
    const int idle_top = catalog_grid_top();
    const std::vector<std::pair<std::string, voice_search::Snapshot>> phases = {
        {"preparing", {voice_search::Phase::preparing, "Hidden voice engine setup copy"}},
        {"downloading", {voice_search::Phase::downloading, "Model download", 0.43f}},
        {"listening", {voice_search::Phase::listening, "Controller microphone", 0, 0.72f}},
        {"muted", {voice_search::Phase::listening, "Unmute your microphone to speak", 0, 0, true}},
        {"processing", {voice_search::Phase::processing, "Recognising on your PS5"}},
        {"error", {voice_search::Phase::error, "Could not start the microphone"}},
    };
    for (const auto &[name, state] : phases)
    {
        g_view.voice_owned = true;
        voice_search::fake_snapshot = state;
        const bool status = ui::voice_search_status_visible(state.phase);
        assert(catalog_grid_top() == idle_top + (status ? ui::kVoiceSearchStatusHeight : 0));
        for (int frame = 0; frame < 24; ++frame)
        {
            ui::begin_frame(1.0f / 60.0f);
            gfx::clear(gfx::palette::background);
            draw(true);
        }
        const auto *pixels = gfx::back_buffer();
        const int gap_start = status ? ui::kSearchFieldY + ui::kSearchFieldH + 54 + 110
                                     : ui::kSearchFieldY + ui::kSearchFieldH + 34;
        for (int y = gfx::to_physical_y(gap_start); y < gfx::to_physical_y(catalog_grid_top() - 20);
             ++y)
            for (int x = gfx::to_physical_x(ui::kContentX);
                 x < gfx::to_physical_x(ui::kContentRight); ++x)
                assert(pixels[y * gfx::physical_width() + x] == gfx::palette::background);
        gfx::present();
        assert(host::write_png("build/tests/discover-voice-" + name + ".png"));
    }
    leave();
    gfx::shutdown();
    host::close_display();
    std::cout << "Discover Search: fixed scope and clean empty/results layouts passed\n";
}
