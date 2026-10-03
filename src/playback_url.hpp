/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_PLAYBACK_URL_HPP
#define SLOPFIN_PLAYBACK_URL_HPP

#include <string>
#include <string_view>

namespace slopfin::jellyfin
{
/* Query names are case-insensitive. Decode percent escapes in values only. */
inline std::string query_value(std::string_view url, std::string_view key)
{
    const auto question = url.find('?');
    if (question == std::string_view::npos)
        return {};
    auto query = url.substr(question + 1);
    query = query.substr(0, query.find('#'));
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; };
    const auto hex = [](char c)
    {
        return c >= '0' && c <= '9'   ? c - '0'
               : c >= 'a' && c <= 'f' ? c - 'a' + 10
               : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                      : -1;
    };
    while (!query.empty())
    {
        const auto amp = query.find('&');
        const auto parameter = query.substr(0, amp);
        const auto equals = parameter.find('=');
        const auto name = parameter.substr(0, equals);
        bool matches = name.size() == key.size();
        for (std::size_t i = 0; matches && i < name.size(); ++i)
            matches = lower(name[i]) == lower(key[i]);
        if (matches && equals != std::string_view::npos)
        {
            const auto value = parameter.substr(equals + 1);
            std::string decoded;
            for (std::size_t i = 0; i < value.size(); ++i)
            {
                if (value[i] == '%' && i + 2 < value.size() && hex(value[i + 1]) >= 0 &&
                    hex(value[i + 2]) >= 0)
                {
                    decoded += static_cast<char>(hex(value[i + 1]) * 16 + hex(value[i + 2]));
                    i += 2;
                }
                else
                    decoded += value[i] == '+' ? ' ' : value[i];
            }
            return decoded;
        }
        if (amp == std::string_view::npos)
            break;
        query.remove_prefix(amp + 1);
    }
    return {};
}

/* Replace every case-insensitive occurrence; value must be URL-encoded. */
inline std::string with_query_value(std::string_view url, std::string_view key,
                                    std::string_view value)
{
    const auto hash = url.find('#');
    const auto fragment = hash == std::string_view::npos ? std::string_view{} : url.substr(hash);
    url = url.substr(0, hash);
    const auto question = url.find('?');
    std::string result{url.substr(0, question)};
    result += '?';
    auto query = question == std::string_view::npos ? std::string_view{} : url.substr(question + 1);
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; };
    while (!query.empty())
    {
        const auto amp = query.find('&');
        const auto parameter = query.substr(0, amp);
        const auto name = parameter.substr(0, parameter.find('='));
        bool matches = name.size() == key.size();
        for (std::size_t i = 0; matches && i < name.size(); ++i)
        {
            const char c = name[i];
            matches = lower(c) == lower(key[i]);
        }
        if (!matches && !parameter.empty())
        {
            result.append(parameter);
            result += '&';
        }
        if (amp == std::string_view::npos)
            break;
        query.remove_prefix(amp + 1);
    }
    result.append(key);
    result += '=';
    result.append(value);
    result.append(fragment);
    return result;
}

/* PlaybackInfo can omit a disabled subtitle index from its generated URL.
 * Preserve the explicit selection or the stream endpoint selects a default. */
inline std::string with_subtitle_index(std::string_view url, int index)
{
    return with_query_value(url, "SubtitleStreamIndex", std::to_string(index));
}
} // namespace slopfin::jellyfin

#endif
