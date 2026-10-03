/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "http.hpp"
#include "server_address.hpp"
#include "json.hpp"
#include <cassert>
#include <cstdio>
namespace slopfin::trace
{
void mark(std::string_view) noexcept
{
}
} // namespace slopfin::trace
int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    auto address = slopfin::server_address::parse(argv[1]);
    assert(address.valid());
    auto response = slopfin::http::get(address.host, address.port, "/System/Info/Public", {});
    if (argc == 3)
    {
        assert(!response.ok());
        std::puts("Untrusted TLS certificate rejected");
        return 0;
    }
    assert(response.ok());
    auto root = slopfin::json::parse(response.body);
    assert(root && root->str("ServerName") == "Test server");
    assert(!root->str("Id").empty());
    slopfin::http::Stream stream;
    assert(stream.open(address.host, address.port, "/System/Info/Public", {}));
    char bytes[7];
    std::string body;
    for (long count; (count = stream.read(bytes, sizeof(bytes))) > 0;)
        body.append(bytes, count);
    assert(body == response.body);
    stream.close();
    assert(stream.open(address.host, address.port, "/System/Info/Public", {}));
    stream.interrupt();
    assert(stream.read(bytes, sizeof(bytes)) < 0);
    auto redirect =
        slopfin::http::get(address.host, address.port, "/redirect", {"X-Emby-Token: test-token"});
    assert(redirect.status == 302);
    auto posted =
        slopfin::http::post(address.host, address.port, "/echo", {}, "payload", "text/plain");
    assert(posted.ok() && posted.body == "payload");
    std::puts("HTTPS API, incremental reads, cancellation and certificate verification passed");
}
