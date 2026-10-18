/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/update.cpp"
#include <cassert>
namespace slopfin::trace
{
void mark(std::string_view) noexcept
{
}
} // namespace slopfin::trace
int main()
{
    using namespace slopfin::update;
    assert(application_file("eboot.bin") == "/app0/eboot.bin");
    recovered_storage = true;
    assert(application_file("eboot.bin") == "/data/homebrew/PPSA99001/eboot.bin");
    recovered_storage = false;
    assert(storage_error(ENOENT).find("unavailable") != std::string::npos);
    assert(storage_error(EACCES).find("permissions") != std::string::npos);
    assert(storage_error(ENOSPC).find("free space") != std::string::npos);
    assert(storage_error(ENOTDIR).find("error " + std::to_string(ENOTDIR)) != std::string::npos);
    state.phase = Phase::installing;
    state.version = "01.000.006";
    state.received = state.total = 1024;
    failure("ELF Loader is unavailable");
    assert(state.phase == Phase::ready);
    assert(state.received == state.total && state.version == "01.000.006");
    assert(state.message == "ELF Loader is unavailable");
    state.phase = Phase::downloading;
    failure("Download interrupted");
    assert(state.phase == Phase::error);
}
