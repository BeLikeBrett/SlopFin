/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/* SlopFin - server entry and canonical endpoint construction.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_SERVER_ADDRESS_HPP
#define SLOPFIN_SERVER_ADDRESS_HPP
#include <string>
#include <string_view>
#include <algorithm>
#include <cctype>
namespace slopfin::server_address
{
// Old saved bare IPs retain their fast LAN transport. New URLs carry scheme
// and optional base path; port remains separate for config compatibility.
inline bool web_transport(std::string_view host)
{
    return host.find("://") != std::string_view::npos;
}
inline std::string origin(std::string_view host, int port)
{
    if (!web_transport(host))
        return "http://" + std::string(host) + ":" + std::to_string(port);
    const auto start = host.find("://") + 3;
    const auto slash = host.find('/', start);
    std::string result(host.substr(0, slash));
    const bool standard = (host.rfind("https://", 0) == 0 && port == 443) ||
                          (host.rfind("http://", 0) == 0 && port == 80);
    if (!standard)
        result += ":" + std::to_string(port);
    if (slash != std::string_view::npos)
        result += host.substr(slash);
    return result;
}
inline std::string url(std::string_view host, int port, std::string_view path)
{
    return origin(host, port) + std::string(path);
}
struct Address
{
    std::string host;
    int port = 8096;
    std::string error;
    bool valid() const noexcept
    {
        return error.empty() && !host.empty();
    }
    std::string display() const
    {
        return web_transport(host) ? origin(host, port) : host + ":" + std::to_string(port);
    }
};
inline Address parse(std::string_view input)
{
    Address result;
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front())))
        input.remove_prefix(1);
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())))
        input.remove_suffix(1);
    if (input.empty())
    {
        result.error = "Enter your Jellyfin server address first.";
        return result;
    }
    for (char c : input)
        if (std::isspace(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) < 32)
        {
            result.error = "Remove spaces from the server address and try again.";
            return result;
        }
    std::string lowered(input);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::string scheme;
    if (lowered.rfind("https://", 0) == 0)
    {
        scheme = "https://";
        input.remove_prefix(8);
    }
    else if (lowered.rfind("http://", 0) == 0)
    {
        scheme = "http://";
        input.remove_prefix(7);
    }
    else if (input.find("://") != std::string_view::npos)
    {
        result.error =
            "Use a server address beginning with http:// or https://, or leave the prefix out.";
        return result;
    }
    std::string path;
    const auto slash = input.find('/');
    if (slash != std::string_view::npos)
    {
        path = input.substr(slash);
        input = input.substr(0, slash);
        const auto web = path.find("/web");
        if (web != std::string::npos && (web + 4 == path.size() || path[web + 4] == '/'))
            path.resize(web);
        const auto suffix = path.find_first_of("?#");
        if (suffix != std::string::npos)
            path.resize(suffix);
        while (!path.empty() && path.back() == '/')
            path.pop_back();
    }
    const auto colon = input.find(':');
    auto host = input.substr(0, colon);
    if (host.empty() || host.size() > 253)
    {
        result.error = "Check your server address and try again.";
        return result;
    }
    bool numeric = true;
    for (char c : host)
    {
        if (c != '.' && (c < '0' || c > '9'))
            numeric = false;
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-')
        {
            result.error = "Enter a server name or IP address, without a username or spaces.";
            return result;
        }
    }
    if (numeric)
    {
        int parts[4]{};
        std::size_t offset = 0;
        for (int part = 0; part < 4; ++part)
        {
            const auto end = host.find('.', offset);
            auto number = host.substr(offset, end == std::string_view::npos ? host.size() - offset
                                                                            : end - offset);
            if (number.empty() || number.size() > 3 ||
                (part < 3 && end == std::string_view::npos) ||
                (part == 3 && end != std::string_view::npos))
            {
                result.error =
                    "Check the IP address. It should contain four numbers, like 192.168.1.20.";
                return result;
            }
            for (char c : number)
                parts[part] = parts[part] * 10 + c - '0';
            if (parts[part] > 255)
            {
                result.error = "Check the IP address. Each number must be between 0 and 255.";
                return result;
            }
            offset = end + 1;
        }
        if (parts[0] == 0 || parts[0] >= 224)
        {
            result.error = "Enter your server's IP address, not a broadcast address.";
            return result;
        }
        result.host = std::to_string(parts[0]) + "." + std::to_string(parts[1]) + "." +
                      std::to_string(parts[2]) + "." + std::to_string(parts[3]);
    }
    else
    {
        std::size_t offset = 0;
        while (offset < host.size())
        {
            const auto end = host.find('.', offset);
            auto label = host.substr(offset, end == std::string_view::npos ? host.size() - offset
                                                                           : end - offset);
            if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-')
            {
                result.error = "Check the server name and try again.";
                return result;
            }
            if (end == std::string_view::npos)
                break;
            offset = end + 1;
            if (offset == host.size())
            {
                result.error = "Remove the dot at the end of the server name.";
                return result;
            }
        }
        result.host = std::string(host);
        std::transform(result.host.begin(), result.host.end(), result.host.begin(),
                       [](unsigned char c) { return std::tolower(c); });
    }
    // Bare IPs use Jellyfin's LAN port. Names use HTTPS, unless a local
    // Jellyfin port or single-label LAN hostname was explicitly supplied.
    if (scheme.empty() && !numeric)
        scheme = (host.find('.') == std::string_view::npos ||
                  (colon != std::string_view::npos && input.substr(colon + 1) == "8096"))
                     ? "http://"
                     : "https://";
    result.port = scheme == "https://" ? 443 : scheme == "http://" ? (numeric ? 8096 : 80) : 8096;
    if (colon != std::string_view::npos)
    {
        auto port = input.substr(colon + 1);
        int value = 0;
        if (port.empty())
        {
            result.error = "Add a port after the colon, or remove the colon.";
            return result;
        }
        for (char c : port)
        {
            if (c < '0' || c > '9' || value > 6553)
            {
                result.error = "The port must be a number from 1 to 65535.";
                return result;
            }
            value = value * 10 + c - '0';
        }
        if (value < 1 || value > 65535)
        {
            result.error = "The port must be a number from 1 to 65535.";
            return result;
        }
        result.port = value;
    }
    if (!path.empty() && scheme.empty())
        scheme = "http://";
    if (!scheme.empty())
        result.host = scheme + result.host + path;
    return result;
}
} // namespace slopfin::server_address
#endif
