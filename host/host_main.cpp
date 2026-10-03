/*
 * SlopFin - Linux host entry point.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The same loop main.cpp runs on the console, wrapped in what a workstation
 * needs instead of a controller and a television: a window, a keyboard, and a
 * way to drive the interface from a script so a capture is reproducible rather
 * than whatever happened to be on screen when a key was pressed.
 */

#include "host_platform.hpp"

#include "../src/app.hpp"
#include "../src/crash.hpp"
#include "../src/gfx.hpp"
#include "../src/icons.hpp"
#include "../src/pad.hpp"
#include "../src/text.hpp"
#include "../src/trace.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace slopfin;
using slopfin::pad::Button;

struct Step
{
    enum class Kind
    {
        wait,
        press,
        shot,
        search, /* submit a scoped search term */
        open,   /* open an item's detail page by id  */
    } kind = Kind::wait;
    int frames = 1;
    Button button = Button::cross;
    std::string path;
};

struct NamedButton
{
    const char *name;
    Button button;
};

constexpr NamedButton kButtons[] = {
    {"up", Button::up},           {"down", Button::down},
    {"left", Button::left},       {"right", Button::right},
    {"cross", Button::cross},     {"circle", Button::circle},
    {"square", Button::square},   {"triangle", Button::triangle},
    {"l1", Button::l1},           {"r1", Button::r1},
    {"l2", Button::l2},           {"r2", Button::r2},
    {"l3", Button::l3},           {"r3", Button::r3},
    {"options", Button::options}, {"touchpad", Button::touchpad},
};

std::vector<std::string> split(const std::string &text, char separator) noexcept
{
    std::vector<std::string> parts;
    std::string current;
    for (const char c : text)
    {
        if (c == separator)
        {
            if (!current.empty())
                parts.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty())
        parts.push_back(current);
    return parts;
}

std::string trim(std::string text) noexcept
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.erase(text.begin());
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n'))
        text.pop_back();
    return text;
}

/* Script steps run once per frame: waits, controller presses and captures. */
std::vector<Step> parse_script(const std::string &text) noexcept
{
    std::vector<Step> steps;
    for (const std::string &raw : split(text, ';'))
    {
        const std::string piece = trim(raw);
        if (piece.empty())
            continue;
        const std::vector<std::string> words = split(piece, ' ');
        const std::string &head = words[0];

        if (head == "wait")
        {
            Step step;
            step.kind = Step::Kind::wait;
            step.frames = words.size() > 1 ? std::atoi(words[1].c_str()) : 1;
            if (step.frames < 1)
                step.frames = 1;
            steps.push_back(step);
            continue;
        }
        if (head == "shot")
        {
            Step step;
            step.kind = Step::Kind::shot;
            step.path = words.size() > 1 ? words[1] : "shot.png";
            steps.push_back(step);
            continue;
        }
        if (head == "search")
        {
            Step step;
            step.kind = Step::Kind::search;
            step.path = piece.size() > 7 ? piece.substr(7) : "";
            steps.push_back(step);
            continue;
        }
        if (head == "open" && words.size() > 1)
        {
            Step step;
            step.kind = Step::Kind::open;
            step.path = words[1];
            steps.push_back(step);
            continue;
        }
        int repeat = 1;
        if (words.size() > 1 && words[1].size() > 1 && words[1][0] == 'x')
            repeat = std::atoi(words[1].c_str() + 1);
        if (repeat < 1)
            repeat = 1;
        bool found = false;
        for (const NamedButton &entry : kButtons)
        {
            if (head != entry.name)
                continue;
            found = true;
            for (int i = 0; i < repeat; ++i)
            {
                Step press;
                press.kind = Step::Kind::press;
                press.button = entry.button;
                steps.push_back(press);
                /* A frame of air after every press, so the handler that runs
                   on the press and the drawing that answers it are never the
                   same frame. */
                Step gap;
                gap.kind = Step::Kind::wait;
                gap.frames = 2;
                steps.push_back(gap);
            }
            break;
        }
        if (!found)
            std::fprintf(stderr, "slopfin: unknown script step \"%s\"\n", piece.c_str());
    }
    return steps;
}

/* Render an icon contact sheet for visual inspection. */
void draw_icon_sheet() noexcept
{
    gfx::clear(gfx::rgb(0x14, 0x14, 0x1a));
    constexpr int kSize = 96;
    constexpr int kCell = 200;
    constexpr int kColumns = 8;
    static const char *names[] = {
        "play",    "pause",  "skip_back", "skip_forward", "subtitles",  "audio",
        "quality", "info",   "check",     "jellyfin",     "triangle",   "circle",
        "cross",   "square", "search",    "chev_left",    "chev_right",
    };
    const int count = static_cast<int>(icons::Icon::count);
    for (int i = 0; i < count; ++i)
    {
        const int x = 80 + (i % kColumns) * kCell;
        const int y = 120 + (i / kColumns) * kCell;
        gfx::rounded_rect(x - 20, y - 20, kCell - 24, kCell - 24, 16,
                          gfx::rgba(0xff, 0xff, 0xff, 0x0e));
        icons::draw(static_cast<icons::Icon>(i), x, y, kSize, gfx::rgb(0xff, 0xff, 0xff));
        if (i < static_cast<int>(std::size(names)))
            text::draw(x - 10, y + kSize + 12, names[i], 20, text::Weight::regular,
                       gfx::rgb(0xa0, 0xa0, 0xb0));
    }
    text::draw(80, 50, "SlopFin icons", 36, text::Weight::bold, gfx::rgb(0xff, 0xff, 0xff));
}

void usage() noexcept
{
    std::fputs("slopfin (Linux preview)\n"
               "  --headless            draw without a window; use with --script\n"
               "  --scale PERCENT       window size relative to 1920x1080 (default 70)\n"
               "  --no-vsync            do not wait for the host's refresh\n"
               "  --script \"...\"        drive the interface: wait N; down x3; cross; shot a.png\n"
               "  --out DIR             where shot steps write (default captures/)\n"
               "  --exit-after-script   quit once the script finishes\n"
               "  --icon-sheet PATH     draw every icon large and write it, then quit\n"
               "  --real-clock          pace headless frames off the wall, not the frame count\n"
               "  --shot-every N        write a numbered capture every N frames while browsing\n"
               "\n"
               "keys: arrows/WASD move, Return or Z is cross, Backspace or X is circle,\n"
               "      C square, V or Tab triangle, Q/E shoulders, 1/2 triggers,\n"
               "      Esc or P options, T touchpad, I/K or the mouse wheel scroll,\n"
               "      F12 writes a capture.\n",
               stderr);
}
} // namespace

int main(int argc, char **argv)
{
    slopfin::host::Options options;
    options.scale_percent = 70;
    std::string script;
    std::string out_dir = "captures";
    std::string icon_sheet;
    bool exit_after_script = false;
    bool real_clock = false;
    int shot_every = 0;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view argument = argv[i];
        if (argument == "--headless")
            options.headless = true;
        else if (argument == "--no-vsync")
            options.vsync = false;
        else if (argument == "--real-clock")
            real_clock = true;
        else if (argument == "--shot-every" && i + 1 < argc)
            shot_every = std::atoi(argv[++i]);
        else if (argument == "--scale" && i + 1 < argc)
            options.scale_percent = std::atoi(argv[++i]);
        else if (argument == "--script" && i + 1 < argc)
            script = argv[++i];
        else if (argument == "--out" && i + 1 < argc)
            out_dir = argv[++i];
        else if (argument == "--exit-after-script")
            exit_after_script = true;
        else if (argument == "--icon-sheet" && i + 1 < argc)
            icon_sheet = argv[++i];
        else
        {
            usage();
            return argument == "--help" || argument == "-h" ? 0 : 2;
        }
    }
    if (options.scale_percent < 10 || options.scale_percent > 200)
        options.scale_percent = 70;
    /* Headless exists to be captured, and a capture wants a clock that does
       not depend on how long writing the PNG took. */
    options.fixed_step = options.headless && !real_clock;

    slopfin::trace::begin();
    (void)slopfin::host::ensure_directory(out_dir);
    if (!slopfin::host::open_display(options))
        return 1;
    if (!slopfin::gfx::initialize())
    {
        std::fputs("slopfin: gfx::initialize failed\n", stderr);
        return 1;
    }
    if (!slopfin::text::initialize())
        std::fputs("slopfin: fonts missing; run from the app/ directory\n", stderr);
    (void)slopfin::pad::initialize();

    if (!icon_sheet.empty())
    {
        /* Twice: the first present fills one buffer, and the capture reads
           what was last handed to the display. */
        for (int i = 0; i < 2; ++i)
        {
            draw_icon_sheet();
            slopfin::gfx::present();
        }
        const bool written = slopfin::host::write_png(icon_sheet);
        std::fprintf(stderr, "slopfin: %s %s\n", written ? "wrote" : "could not write",
                     icon_sheet.c_str());
        slopfin::gfx::shutdown();
        slopfin::host::close_display();
        return written ? 0 : 1;
    }

    slopfin::crash::install();
    slopfin::app::start();

    const std::vector<Step> steps = parse_script(script);
    std::size_t at = 0;
    int wait_left = 0;
    int manual_shot = 0;
    int timed_shot = 0;

    for (;;)
    {
        if (!slopfin::host::pump_events())
            break;

        if (at < steps.size() && wait_left == 0)
        {
            const Step &step = steps[at];
            switch (step.kind)
            {
            case Step::Kind::wait:
                wait_left = step.frames;
                break;
            case Step::Kind::press:
                slopfin::pad::inject(step.button);
                break;
            case Step::Kind::search:
                slopfin::app::preview_search(step.path);
                break;
            case Step::Kind::open:
                slopfin::app::open_item(step.path);
                break;
            case Step::Kind::shot:
            {
                const std::string path = out_dir + "/" + step.path;
                if (!slopfin::host::write_png(path))
                    std::fprintf(stderr, "slopfin: could not write %s\n", path.c_str());
                else
                    std::fprintf(stderr, "slopfin: wrote %s\n", path.c_str());
                break;
            }
            }
            ++at;
        }
        else if (wait_left > 0)
        {
            --wait_left;
        }
        else if (exit_after_script && !steps.empty())
        {
            break;
        }

        slopfin::pad::poll();
        slopfin::app::frame();
        if (slopfin::app::exit_requested())
            break;
        slopfin::gfx::present();

        /* Capture the presented host frame at the requested interval. */
        if (shot_every > 0 &&
            (slopfin::host::frames_presented() % static_cast<std::uint64_t>(shot_every)) == 0)
        {
            char path[256];
            std::snprintf(path, sizeof(path), "%s/live-%04d.png", out_dir.c_str(), timed_shot++);
            (void)slopfin::host::write_png(path);
        }

        /* F12 is read after the present, so the capture is the frame that was
           just shown rather than the one being built. */
        if (!options.headless)
        {
            if (slopfin::host::capture_requested())
            {
                char path[256];
                std::snprintf(path, sizeof(path), "%s/manual-%03d.png", out_dir.c_str(),
                              manual_shot++);
                if (slopfin::host::write_png(path))
                    std::fprintf(stderr, "slopfin: wrote %s\n", path);
            }
        }
    }

    slopfin::gfx::shutdown();
    slopfin::host::close_display();

    /* Exit without destroying shared state while detached app workers remain active, matching
     * console process teardown. */
    std::fflush(nullptr);
    std::_Exit(0);
}
