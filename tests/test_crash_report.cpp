/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * SlopFin - the crash report has to be readable and complete.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The report is written by a dying process, so the only way to know it works
 * is to kill one: this test forks a child, makes it fault for real, and then
 * reads what the handler left behind. (Opus 5, 2026-09-15.)
 */

#include "crash.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace slopfin::trace
{
void mark(std::string_view) noexcept
{
}
} // namespace slopfin::trace

namespace
{
std::string slurp(const char *path)
{
    std::ifstream file(path);
    std::stringstream body;
    body << file.rdbuf();
    return body.str();
}

int g_failures = 0;
void check(bool condition, const std::string &what)
{
    if (condition)
        return;
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
}

/* A child that sets the scene and then dies the way `kind` says. */
void die(int kind)
{
    slopfin::crash::install();
    slopfin::crash::frame(4242);
    slopfin::crash::phase("playing The Hangover");
    slopfin::crash::note("opened the audio panel");
    slopfin::crash::note("chose Dolby Digital");
    slopfin::crash::self_test(kind);
    _exit(0); /* not reached */
}

int run_child(int kind)
{
    const pid_t child = fork();
    if (child == 0)
        die(kind);
    int status = 0;
    (void)waitpid(child, &status, 0);
    return status;
}
} // namespace

int main()
{
    (void)std::remove("slopfin-crash.txt");
    (void)std::remove("slopfin-crashes.log");
    (void)std::remove("slopfin-session.txt");
    (void)std::remove("slopfin-lastrun.txt");
    (void)std::remove("slopfin-report-sent.txt");
    (void)std::remove("slopfin-report-offered.txt");
    (void)std::remove("slopfin-trace.txt");
    {
        std::ofstream trace("slopfin-trace.txt");
        for (int i = 0; i < 400; ++i)
            trace << "line " << i << "\n";
        trace << "player: the last thing that happened\n";
    }

    /* A deliberate application close is not a crash. The platform's normal
       termination signal must remove the live-session marker before exit. */
    {
        const pid_t child = fork();
        if (child == 0)
        {
            slopfin::crash::install();
            slopfin::crash::phase("home");
            raise(SIGTERM);
            _exit(99);
        }
        int status = 0;
        (void)waitpid(child, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "a normal termination exits cleanly");
        check(slurp("slopfin-session.txt").empty(),
              "a normal termination leaves no unfinished session");
    }

    (void)run_child(1); /* a bad memory access */
    const std::string report = slurp("slopfin-crash.txt");
    check(report.find("SIGSEGV") != std::string::npos, "the report names the signal");
    check(report.find("playing The Hangover") != std::string::npos,
          "it says what the app was doing");
    check(report.find("chose Dolby Digital") != std::string::npos, "it keeps the recent notes");
    check(report.find("4242") != std::string::npos, "it says how far the session got");
    check(report.find("player: the last thing that happened") != std::string::npos,
          "it carries the tail of the trace");
    check(report.find("line 0\n") == std::string::npos, "only the tail, not the whole trace");
    check(report.find("--- stack ---") != std::string::npos, "it has a backtrace section");
    /* A fault in the app's own code has a frame chain, and the first return
       address must be inside this binary rather than a stray value. */
    const std::size_t stack = report.find("--- stack ---");
    const std::size_t first = report.find("  0  0x", stack);
    check(first != std::string::npos, "the backtrace names at least one caller");
    if (first != std::string::npos)
    {
        const std::string address = report.substr(first + 7, 12);
        check(std::stoull(address, nullptr, 16) > 0x1000, "and it is a real address");
    }
    check(report.find("build") != std::string::npos, "it stamps the build");
    check(slopfin::crash::report_available(), "a fatal crash remains available for diagnostics");
    check(slopfin::crash::report_waiting(), "a fatal crash is offered at startup");
    slopfin::crash::report_offered();
    check(!slopfin::crash::report_waiting(), "the same fatal crash is offered only once");

    /* A second crash appends rather than replacing the history. */
    (void)run_child(3); /* abort */
    const std::string history = slurp("slopfin-crashes.log");
    check(history.find("SIGSEGV") != std::string::npos &&
              history.find("SIGABRT") != std::string::npos,
          "the log keeps every crash");
    check(slurp("slopfin-crash.txt").find("SIGABRT") != std::string::npos,
          "the newest report is the newest crash");

    /* A run that dies without reporting leaves its session behind, and the
       next start turns that into a report of its own. */
    (void)std::remove("slopfin-crash.txt");
    {
        std::ofstream session("slopfin-session.txt");
        session << "slopfin session, running\ndoing   home\n";
    }
    slopfin::crash::install();
    const std::string last = slurp("slopfin-lastrun.txt");
    check(last.find("without shutting down") != std::string::npos, "an unclean exit is noticed");
    check(last.find("doing   home") != std::string::npos, "and keeps what that run was doing");
    check(slopfin::crash::report_available(), "an unfinished run stays available for diagnostics");
    check(!slopfin::crash::report_waiting(),
          "an unfinished run alone does not claim the app crashed");
    slopfin::crash::finish();
    check(slurp("slopfin-session.txt").empty(), "a clean finish leaves no session behind");

    if (g_failures == 0)
        std::printf("crash report: all checks passed\n");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
