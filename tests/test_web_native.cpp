/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise the real PS5 request adapter against ABI stubs, without a console. */
#include "web_transport.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>
namespace
{
int created = 0, deleted = 0, closed = 0, aborted = 0, pools = 0;
bool fail_send = false;
std::string method, uploaded, address;
} // namespace
namespace slopfin::trace
{
void mark(std::string_view) noexcept
{
}
} // namespace slopfin::trace
extern "C"
{
    int sceNetPoolCreate(const char *, int, int)
    {
        ++pools;
        return 1;
    }
    int sceSslInit(std::size_t)
    {
        return 2;
    }
    int sceHttpInit(int pool, int ssl, std::size_t)
    {
        assert(pool == 1 && ssl == 2);
        return 3;
    }
    int sceHttpCreateTemplate(int ctx, const char *, int version, int)
    {
        assert(ctx == 3 && version == 2);
        return 4;
    }
    int sceHttpCreateConnectionWithURL(int tmpl, const char *url, int)
    {
        assert(tmpl == 4);
        address = url;
        return 5;
    }
    int sceHttpCreateRequestWithURL2(int conn, const char *verb, const char *url, std::uint64_t)
    {
        assert(conn == 5 && address == url);
        method = verb;
        ++created;
        return 6;
    }
    int sceHttpAddRequestHeader(int, const char *name, const char *value, std::uint32_t)
    {
        assert(*name && *value);
        return 0;
    }
    int sceHttpSetAutoRedirect(int, int enabled)
    {
        assert(enabled == 0);
        return 0;
    }
    int sceHttpSetResolveTimeOut(int, std::uint32_t)
    {
        return 0;
    }
    int sceHttpSetConnectTimeOut(int, std::uint32_t)
    {
        return 0;
    }
    int sceHttpSetRecvTimeOut(int, std::uint32_t)
    {
        return 0;
    }
    int sceHttpSetSendTimeOut(int, std::uint32_t)
    {
        return 0;
    }
    int sceHttpSendRequest(int, const void *body, std::size_t size)
    {
        uploaded.assign(size ? static_cast<const char *>(body) : "", size);
        return fail_send ? -1 : 0;
    }
    int sceHttpGetStatusCode(int, int *status)
    {
        *status = 200;
        return 0;
    }
    int sceHttpReadData(int, void *body, std::size_t size)
    {
        if (!size)
            return 0;
        *static_cast<char *>(body) = 'x';
        return 1;
    }
    int sceHttpAbortRequest(int)
    {
        ++aborted;
        return 0;
    }
    int sceHttpDeleteRequest(int)
    {
        ++deleted;
        return 0;
    }
    int sceHttpDeleteConnection(int)
    {
        ++closed;
        return 0;
    }
}
int main()
{
    using slopfin::http::WebRequest;
    {
        WebRequest request;
        assert(request.open("https://media.example.com/System/Info/Public", "GET", {}, {}, {}));
        assert(request.status() == 200 && method == "GET");
        char byte = 0;
        assert(request.read(&byte, 1) == 1 && byte == 'x');
        request.interrupt();
        assert(request.read(&byte, 1) < 0 && aborted == 1);
    }
    {
        WebRequest request;
        assert(request.open("https://media.example.com/Users/AuthenticateByName", "POST",
                            {"X-Emby-Authorization: test"}, "{}", "application/json"));
        assert(method == "POST" && uploaded == "{}");
    }
    fail_send = true;
    {
        WebRequest request;
        assert(!request.open("https://example.test/", "GET", {}, {}, {}));
    }
    assert(pools == 1 && created == 3 && deleted == 3 && closed == 3);
    std::puts("Native HTTPS request, upload, cancellation and cleanup tests passed");
}
