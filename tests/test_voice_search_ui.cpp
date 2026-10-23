/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise the actual search UI without a microphone, model or network worker. */
#include "../src/app.cpp"
#include "../host/host_platform.hpp"
#include "fake_voice_search.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>

namespace slopfin::ime
{
bool fake_open = false;
bool fake_cancelled = false;
bool fake_have_result = false;
std::string fake_result;
int requests = 0;
std::string initial_text;
Mode requested_mode = Mode::text;
bool initialize() noexcept
{
    return true;
}
void shutdown() noexcept
{
    fake_open = false;
}
void request(const std::string &initial, const std::string &, const std::string &,
             Mode mode) noexcept
{
    ++requests;
    initial_text = initial;
    requested_mode = mode;
    fake_open = true;
}
void poll() noexcept
{
}
bool busy() noexcept
{
    return fake_open;
}
bool available() noexcept
{
    return true;
}
bool take_cancelled() noexcept
{
    const bool value = fake_cancelled;
    fake_cancelled = false;
    return value;
}
bool take_result(std::string &out) noexcept
{
    if (!fake_have_result)
        return false;
    fake_have_result = false;
    out = fake_result;
    fake_result.clear();
    return true;
}
} // namespace slopfin::ime

int main()
{
    using namespace slopfin;
    host::Options options;
    options.headless = true;
    assert(host::open_display(options) && gfx::initialize() && text::initialize() &&
           pad::initialize() && ime::initialize());
    config::load();
    std::filesystem::create_directories("build/tests");
    const SearchScope movies{"Movies", "movie-library", "Movie"};
    jellyfin::Item film;
    film.id = "film-fixture";
    film.name = "TRON: Legacy";
    film.type = "Movie";
    const std::vector<jellyfin::Item> results{film, film};
    const auto sections = search_sections::build(results);
    const auto handle = [&](const SearchScope &scope)
    { return handle_search_input(scope, results, sections); };
    const auto press = [&](pad::Button button)
    {
        pad::inject(button);
        pad::poll();
        assert(handle(movies));
        pad::poll();
    };
    const auto reset = [&]
    {
        voice_search::fake_snapshot = {};
        voice_search::fail_start = false;
        voice_search::fake_result.clear();
        voice_search::starts = voice_search::finishes = voice_search::cancellations =
            voice_search::deliveries = 0;
        ime::fake_open = false;
        ime::fake_cancelled = ime::fake_have_result = false;
        ime::fake_result.clear();
        ime::requests = 0;
        ime::initial_text.clear();
        g_view.search = {};
        g_view.prompt = View::Prompt::none;
        g_view.ime_input_guard = 0;
        g_view.sidebar_focused = false;
        g_shared.screen = Screen::home;
        g_shared.request = Request::none;
        g_shared.search_items.clear();
        g_shared.search_error.clear();
        g_shared.search_loading = false;
        pad::poll();
    };

    reset();
    assert(!handle(movies));
    assert(voice_search::starts == 0 && ime::requests == 0);
    open_search();
    assert(g_view.search.open && voice_search::starts == 0 && ime::requests == 0);
    assert(handle(movies));
    assert(voice_search::starts == 0 && ime::requests == 0);
    press(pad::Button::r3);
    assert(voice_search::starts == 1 && voice_search::busy() && ime::requests == 0);
    press(pad::Button::cross);
    assert(voice_search::finishes == 1 &&
           voice_search::snapshot().phase == voice_search::Phase::processing);
    const int previous_cancel = voice_search::cancellations;
    press(pad::Button::circle);
    assert(voice_search::cancellations == previous_cancel + 1 && !voice_search::busy() &&
           g_view.search.open);
    press(pad::Button::circle);
    assert(!g_view.search.open);

    // Cross types from an empty search; it must never activate the microphone.
    reset();
    open_search();
    press(pad::Button::cross);
    assert(voice_search::starts == 0 && ime::requests == 1 && ime::busy() &&
           ime::requested_mode == ime::Mode::search && ime::initial_text.empty());

    // Circle dismisses only the keyboard, for empty and existing searches.
    // Intercepted IME samples can reveal the same held Circle again afterward;
    // require release before allowing it to close the Search overlay.
    for (const std::string &query : {std::string{}, std::string{"TRON"}})
    {
        reset();
        open_search();
        g_view.search.committed = g_view.search.draft = query;
        press(pad::Button::triangle);
        assert(ime::busy() && g_view.prompt == View::Prompt::search);
        ime::fake_open = false;
        ime::fake_cancelled = true;
        pad::inject_hold(pad::Button::circle, 10);
        pad::poll();
        handle_keyboard_result();
        assert(g_view.search.open && g_view.search.committed == query &&
               g_view.prompt == View::Prompt::none && g_view.ime_input_guard == 2);
        for (int frame = 0; frame < 6; ++frame)
        {
            advance_keyboard_input_guard();
            assert(handle(movies) && g_view.search.open && g_view.ime_input_guard > 0);
            pad::poll();
        }
        while (pad::held(pad::Button::circle))
            pad::poll();
        advance_keyboard_input_guard();
        advance_keyboard_input_guard();
        assert(g_view.ime_input_guard == 0 && g_view.search.open);
        press(pad::Button::circle);
        assert(!g_view.search.open);
    }

    reset();
    open_search();
    press(pad::Button::cross);
    ime::fake_open = false;
    ime::fake_have_result = true;
    ime::fake_result = "TRON";
    pad::inject_hold(pad::Button::cross, 4);
    pad::poll();
    handle_keyboard_result();
    assert(g_view.search.open && g_view.search.draft == "TRON" && g_view.search.committed.empty() &&
           g_view.search.pending == 1);
    assert(handle(movies) && g_shared.request == Request::none && voice_search::starts == 0);
    while (pad::held(pad::Button::cross))
        pad::poll();
    advance_keyboard_input_guard();
    advance_keyboard_input_guard();
    assert(handle(movies) && g_shared.request == Request::run_search &&
           g_shared.request_text == "TRON" && g_view.search.open);

    // R3 starts and finishes speech without toggling into another recording.
    reset();
    open_search();
    press(pad::Button::r3);
    press(pad::Button::r3);
    assert(voice_search::finishes == 1 && voice_search::starts == 1);

    // Existing results cannot open a title or move focus while voice owns input.
    g_view.search.committed = g_view.search.draft = "TRON";
    g_view.search.result_index = 0;
    g_shared.request = Request::none;
    for (auto button : {pad::Button::right, pad::Button::down, pad::Button::cross})
        press(button);
    assert(g_view.search.result_index == 0 && g_shared.screen == Screen::home &&
           g_shared.request == Request::none && voice_search::starts == 1);
    g_view.search.draft = "A pending typed edit";
    g_view.search.pending = 1;
    for (auto phase : {voice_search::Phase::preparing, voice_search::Phase::downloading,
                       voice_search::Phase::processing})
    {
        voice_search::fake_snapshot.phase = phase;
        for (auto button : {pad::Button::cross, pad::Button::r3, pad::Button::right})
            press(button);
        assert(voice_search::starts == 1 && voice_search::finishes == 1 &&
               g_view.search.pending == 1 && g_shared.request == Request::none &&
               g_view.search.result_index == 0);
    }
    g_view.search.draft = g_view.search.committed;
    g_view.search.pending = 0;

    // Type is always an explicit escape route, cancelling capture and keeping
    // the previous query as the system keyboard's initial value.
    const int cancel_before_typing = voice_search::cancellations;
    press(pad::Button::triangle);
    assert(voice_search::cancellations == cancel_before_typing + 1 && !voice_search::busy());
    assert(g_view.search.open && ime::requests == 1 && ime::busy() &&
           ime::requested_mode == ime::Mode::search && ime::initial_text == "TRON");

    // Recognised speech follows precisely the same scoped request path as
    // typed text, resets result rails, and is delivered only once.
    for (const SearchScope &scope : {movies, SearchScope{"Shows", "show-library", "Series,Episode"},
                                     SearchScope{"Everything", {}, {}}})
    {
        reset();
        open_search();
        g_view.search.committed = g_view.search.draft = "Previous title";
        g_view.search.result_index = 1;
        g_view.search.scroll.jump(175.0f);
        for (auto &rail : g_view.search.rail)
            rail.jump(120.0f);
        g_shared.search_items = results;
        g_shared.search_error = "An old error";
        voice_search::fake_snapshot.phase = voice_search::Phase::complete;
        voice_search::fake_result = "Dragon Ball Super: Beerus";
        assert(handle(scope));
        assert(g_view.search.open && g_view.search.draft == "Dragon Ball Super: Beerus" &&
               g_view.search.committed == g_view.search.draft);
        assert(g_shared.request == Request::run_search &&
               g_shared.request_text == g_view.search.committed &&
               g_shared.request_scope == scope.library_id && g_shared.request_types == scope.types);
        assert(g_view.search.committed_scope == scope.library_id &&
               g_view.search.committed_types == scope.types && g_view.search.result_index == 0 &&
               g_view.search.scroll.value() == 0.0f && g_view.search.pending == 0);
        for (auto &rail : g_view.search.rail)
            assert(rail.value() == 0.0f);
        assert(g_shared.search_items.empty() && g_shared.search_error.empty() &&
               g_shared.search_loading && voice_search::deliveries == 1);
        const auto epoch = g_shared.search_epoch;
        g_shared.request = Request::none;
        assert(handle(scope));
        assert(voice_search::deliveries == 1 && g_shared.request == Request::none &&
               g_shared.search_epoch == epoch);
        // Repeating the same spoken title is an intentional fresh search.
        voice_search::fake_snapshot.phase = voice_search::Phase::complete;
        voice_search::fake_result = g_view.search.committed;
        assert(handle(scope));
        assert(voice_search::deliveries == 2 && g_shared.request == Request::run_search &&
               g_shared.search_epoch > epoch);
    }

    // Closing or switching to typing discards a completed but unconsumed result.
    reset();
    open_search();
    voice_search::fake_snapshot.phase = voice_search::Phase::complete;
    voice_search::fake_result = "Discard me";
    close_search();
    open_search();
    assert(handle(movies));
    assert(g_view.search.committed.empty() && voice_search::deliveries == 0 &&
           g_shared.request == Request::none);
    voice_search::fake_snapshot.phase = voice_search::Phase::complete;
    voice_search::fake_result = "Discard me too";
    ask_keyboard(View::Prompt::search);
    assert(voice_search::fake_result.empty() && voice_search::deliveries == 0);

    reset();
    open_search();
    voice_search::fake_snapshot = {voice_search::Phase::error, "Microphone unavailable"};
    press(pad::Button::r3);
    assert(voice_search::starts == 1 && voice_search::busy() && ime::requests == 0);

    // Capture the actual search field and status layouts in each lifecycle
    // state, with populated results to catch collisions under the voice panel.
    reset();
    open_search();
    gfx::clear(gfx::palette::background);
    draw_search_results({}, {}, false, {});
    draw_search_bar(movies, false, 0);
    gfx::present();
    assert(host::write_png("build/tests/voice-search-empty.png"));
    g_view.search.committed = g_view.search.draft = "TRON";
    const std::vector<std::pair<std::string, voice_search::Snapshot>> previews = {
        {"preparing", {voice_search::Phase::preparing, "Preparing voice engine must stay hidden"}},
        {"empty", {}},
        {"downloading", {voice_search::Phase::downloading, "Downloading voice model · 43%", 0.43f}},
        {"listening", {voice_search::Phase::listening, "Controller microphone", 0.0f, 0.72f}},
        {"muted", {voice_search::Phase::listening, "Unmute your microphone to speak", 0, 0, true}},
        {"processing", {voice_search::Phase::processing, "Recognising on your PS5"}},
        {"error", {voice_search::Phase::error, "Could not start the microphone"}},
    };
    for (const auto &[name, state] : previews)
    {
        voice_search::fake_snapshot = state;
        g_view.search.committed = g_view.search.draft = name == "empty" ? "" : "TRON";
        assert(search_results_top() > kSearchBarY + kSearchBarH + 30);
        for (int frame = 0; frame < 24; ++frame)
        {
            ++g_view.frames;
            ui::begin_frame(1.0f / 60.0f);
            gfx::clear(gfx::palette::background);
            draw_search_results(
                name == "empty" ? std::vector<jellyfin::Item>{} : results,
                name == "empty" ? std::vector<search_sections::Section>{} : sections, false, {});
            draw_search_bar(movies, false, results.size());
        }
        gfx::present();
        assert(host::write_png("build/tests/voice-search-" + name + ".png"));
        if (state.phase == voice_search::Phase::listening ||
            state.phase == voice_search::Phase::downloading)
        {
            int cyan = 0, purple = 0;
            const auto *pixels = gfx::back_buffer();
            for (int y = gfx::to_physical_y(kSearchBarY + kSearchBarH + 54);
                 y < gfx::to_physical_y(search_results_top()); ++y)
                for (int x = gfx::to_physical_x(kSearchBarX);
                     x < gfx::to_physical_x(kSearchBarX + kSearchBarW); ++x)
                {
                    const auto ink = pixels[y * gfx::physical_width() + x];
                    cyan += ink == gfx::rgb(0x7c, 0xd7, 0xe8);
                    purple += ink == gfx::palette::accent;
                }
            assert(purple == 0 && (state.muted || cyan > 0));
        }
    }
    voice_search::shutdown();
    ime::shutdown();
    gfx::shutdown();
    host::close_display();
    std::cout << "Voice search UI: explicit recording, finish/cancel, typing, scoped results and "
                 "lifecycle layouts passed\n";
}
