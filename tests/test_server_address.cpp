/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "server_address.hpp"
#include "config.hpp"
#include <cassert>
#include <cstdio>
int main()
{
    using slopfin::server_address::parse;
    using slopfin::server_address::url;
    assert(slopfin::config::Settings{}.host.empty());
    for (const auto input : {"192.168.1.20", " 192.168.1.20 \n"})
    {
        auto a = parse(input);
        assert(a.valid() && a.host == "192.168.1.20" && a.port == 8096);
    }
    for (const auto input :
         {"media.example.com", "https://media.example.com", " HTTPS://MEDIA.EXAMPLE.COM/ ",
          "https://media.example.com/web/index.html#!/home"})
    {
        auto a = parse(input);
        assert(a.valid() && a.host == "https://media.example.com" && a.port == 443);
        assert(a.display() == "https://media.example.com");
    }
    auto base = parse("https://media.example.com:8443/jellyfin/web/index.html#!/home");
    assert(base.valid() && base.port == 8443 && base.host == "https://media.example.com/jellyfin");
    assert(url(base.host, base.port, "/System/Info/Public") ==
           "https://media.example.com:8443/jellyfin/System/Info/Public");
    auto http = parse("http://media.example.com");
    assert(http.port == 80 && http.host == "http://media.example.com");
    assert(parse("media.example.com:8096").host == "http://media.example.com");
    assert(parse("https://192.168.1.20:8920").port == 8920);
    assert(parse("172.16.0.2:1234").port == 1234);
    assert(parse("10.0.0.2:65535").valid());
    assert(parse("192.168.001.020").host == "192.168.1.20");
    for (const auto input : {"",
                             "  ",
                             "192.168.1.20:",
                             "192.168.1.20:0",
                             "192.168.1.20:65536",
                             "192.168.1.20:8096abc",
                             "192.168.1.20:-1",
                             "192.168.1.20:999999999999",
                             "192.168.0.256",
                             "192.168.0",
                             "192.168.0.1.2",
                             "192.168..1",
                             "[::1]:8096",
                             "http://user:password@192.168.1.20",
                             "media..example.com",
                             "media.example.com.",
                             "-media.example.com",
                             "https://media.example.com/with space",
                             "ftp://media.example.com",
                             "0.0.0.0",
                             "255.255.255.255"})
    {
        auto a = parse(input);
        assert(!a.valid() && !a.error.empty());
    }
    std::puts("Server address tests passed");
}
