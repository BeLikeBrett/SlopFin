/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define main slopfin_application_main
#include "../src/main.cpp"
#undef main
#include <cassert>
#include <csignal>
#include <sys/wait.h>
#include <string>
#include <poll.h>

namespace
{
int events = -1;
void record(char event)
{
    assert(write(events, &event, 1) == 1);
}
} // namespace
extern "C" int sceKernelUsleep(std::uint32_t us)
{
    return usleep(us);
}
extern "C" int sceLncUtilKillLocalProcess(const char *)
{
    record('K');
    for (;;)
        usleep(10000);
}
namespace slopfin::voice_search
{
void shutdown() noexcept
{
    record('V');
}
} // namespace slopfin::voice_search
namespace slopfin::trace
{
void begin() noexcept
{
}
void mark(std::string_view) noexcept
{
}
} // namespace slopfin::trace
namespace slopfin::crash
{
bool close_requested() noexcept
{
    return false;
}
void install() noexcept
{
}
void finish() noexcept
{
    record('C');
}
} // namespace slopfin::crash
namespace slopfin::update
{
bool prepare_storage() noexcept
{
    return true;
}
bool finish_close() noexcept
{
    record('H');
    return true;
}
} // namespace slopfin::update
namespace slopfin::app
{
void start() noexcept
{
}
void frame() noexcept
{
}
bool exit_requested() noexcept
{
    return true;
}
std::uint64_t now_us() noexcept
{
    return 0;
}
void note_pad(std::uint64_t) noexcept
{
}
void note_present(std::uint64_t) noexcept
{
}
} // namespace slopfin::app
namespace slopfin::gfx
{
bool initialize() noexcept
{
    return true;
}
bool menu_accelerated() noexcept
{
    return true;
}
void shutdown() noexcept
{
    record('G');
}
void present() noexcept
{
    record('P');
}
} // namespace slopfin::gfx
namespace slopfin::text
{
bool initialize() noexcept
{
    return true;
}
} // namespace slopfin::text
namespace slopfin::pad
{
bool initialize() noexcept
{
    return true;
}
void poll() noexcept
{
}
} // namespace slopfin::pad
namespace slopfin::ime
{
void shutdown() noexcept
{
    record('I');
}
} // namespace slopfin::ime
namespace slopfin::player
{
void stop(bool) noexcept
{
    record('A');
}
} // namespace slopfin::player
namespace slopfin::reporter
{
bool flush(unsigned timeout_ms) noexcept
{
    assert(timeout_ms == 2000);
    record('R');
    return true;
}
} // namespace slopfin::reporter

int main()
{
    int pipefd[2];
    assert(pipe(pipefd) == 0);
    events = pipefd[1];
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0)
    {
        close(pipefd[0]);
        slopfin_application_main();
        _exit(99);
    }
    close(pipefd[1]);
    char sequence[7]{};
    for (char &event : sequence)
    {
        pollfd ready{pipefd[0], POLLIN, 0};
        assert(poll(&ready, 1, 1000) == 1 && read(pipefd[0], &event, 1) == 1);
    }
    int status = 0;
    const bool parked = waitpid(child, &status, WNOHANG) == 0;
    if (parked)
    {
        kill(child, SIGKILL); // The privileged helper owns final termination.
        waitpid(child, &status, 0);
    }
    close(pipefd[0]);
    assert(parked && std::string(sequence, 7) == "VIARGCH");
    // No presentation, forbidden self-exit, or blocking launcher RPC after
    // output release; the helper is notified last and owns process removal.
}
