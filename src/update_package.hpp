/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <string>
#include <string_view>
#include <cstddef>
#include <cstdint>
namespace slopfin::update
{
struct Release
{
    std::string version, url, digest;
    std::size_t size = 0;
    bool preview = false;
};
std::uint64_t version_number(std::string_view text) noexcept;
bool trusted_download(std::string_view url) noexcept;
Release select_release(std::string_view feed, std::string_view current) noexcept;
bool stage_archive(const std::string &archive, const std::string &stage, const Release &release,
                   std::string &error) noexcept;
} // namespace slopfin::update
