/*
 * SlopFin - stored sign-in regression tests.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A launch that cannot reach the server must keep the saved sign-in; only a
 * server that rejects the token signs the viewer out.
 */
#include "config.hpp"
#include "jellyfin.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace
{
/* A one-request-at-a-time server answering every request with `status`. */
struct Server
{
    int fd = -1;
    int port = 0;
    std::atomic<int> status{200};
    std::atomic<bool> stop{false};
    std::thread thread;

    Server()
    {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        const int on = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
        socklen_t length = sizeof(address);
        assert(getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) == 0);
        port = ntohs(address.sin_port);
        assert(listen(fd, 8) == 0);
        thread = std::thread(
            [this]
            {
                while (!stop)
                {
                    const int client = accept(fd, nullptr, nullptr);
                    if (client < 0)
                        continue;
                    char request[4096];
                    (void)recv(client, request, sizeof(request), 0);
                    const std::string body = status == 200 ? R"({"Id":"user","Name":"Alex"})" : "";
                    const std::string reply = "HTTP/1.1 " + std::to_string(status.load()) +
                                              " X\r\nContent-Type: application/json\r\n"
                                              "Content-Length: " +
                                              std::to_string(body.size()) +
                                              "\r\nConnection: close\r\n\r\n" + body;
                    (void)send(client, reply.data(), reply.size(), 0);
                    close(client);
                }
            });
    }
    ~Server()
    {
        stop = true;
        shutdown(fd, SHUT_RDWR);
        close(fd);
        thread.join();
    }
};

void sign_in(int port)
{
    auto &settings = slopfin::config::current();
    settings.host = "127.0.0.1";
    settings.port = port;
    settings.token = "stored-token";
    settings.user_id = "user";
    assert(slopfin::config::save());
}
} // namespace

int main()
{
    using slopfin::jellyfin::SessionCheck;
    char pattern[] = "/tmp/slopfin-session-test-XXXXXX";
    const char *directory = mkdtemp(pattern);
    assert(directory != nullptr);
    setenv("SLOPFIN_DATA", directory, 1);
    slopfin::config::load();

    Server server;

    /* Accepted. */
    sign_in(server.port);
    assert(slopfin::jellyfin::verify_session() == SessionCheck::valid);

    /* A server that is down, or answering with an error of its own, keeps the
       stored sign-in for the next attempt. */
    server.status = 503;
    assert(slopfin::jellyfin::verify_session() == SessionCheck::unreachable);
    assert(slopfin::config::current().signed_in());
    {
        Server closed;
        const int unused = closed.port;
        sign_in(unused);
    }
    assert(slopfin::jellyfin::verify_session() == SessionCheck::unreachable);
    slopfin::config::load();
    assert(slopfin::config::current().signed_in() &&
           slopfin::config::current().token == "stored-token");

    /* Rejected: forgotten, in memory and on disk. */
    sign_in(server.port);
    server.status = 401;
    assert(slopfin::jellyfin::verify_session() == SessionCheck::rejected);
    assert(!slopfin::config::current().signed_in());
    slopfin::config::load();
    assert(!slopfin::config::current().signed_in() &&
           slopfin::config::current().host == "127.0.0.1");

    std::filesystem::remove_all(directory);
    std::cout << "session: stored sign-in kept while unreachable, cleared when rejected\n";
    return 0;
}
