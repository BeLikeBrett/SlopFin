/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <string_view>
#include <vector>
namespace slopfin::http
{
inline std::vector<std::string_view> pem_certificates(std::string_view bundle)
{
    constexpr std::string_view begin = "-----BEGIN CERTIFICATE-----";
    constexpr std::string_view end = "-----END CERTIFICATE-----";
    std::vector<std::string_view> certificates;
    for (std::size_t from = 0;;)
    {
        const auto start = bundle.find(begin, from);
        if (start == std::string_view::npos)
            break;
        const auto finish = bundle.find(end, start + begin.size());
        if (finish == std::string_view::npos || certificates.size() >= 256)
            return {};
        from = finish + end.size();
        certificates.push_back(bundle.substr(start, from - start));
    }
    return certificates;
}
} // namespace slopfin::http
