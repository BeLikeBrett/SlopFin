/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <string>
#include <cstddef>
namespace slopfin::update
{
enum class Phase
{
    idle,
    checking,
    current,
    available,
    downloading,
    ready,
    installing,
    error
};
struct Status
{
    Phase phase = Phase::idle;
    std::string current, version, message, notes;
    std::size_t received = 0, total = 0;
    bool preview = false;
};
Status status() noexcept;
/* Before loading settings: recover shared data access without discarding a saved account. */
bool prepare_storage() noexcept;
void check() noexcept;
void download() noexcept;
void install() noexcept;
bool restart_requested() noexcept;
} // namespace slopfin::update
