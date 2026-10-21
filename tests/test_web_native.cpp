/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise the real PS5 request adapter against ABI stubs, without a console. */
#include "web_transport.hpp"
#include "tls_certificates.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
struct HttpsCertificate
{
    const char *data;
    std::size_t size;
};
namespace
{
int created = 0, deleted = 0, closed = 0, aborted = 0, pools = 0, response_code = 200;
bool fail_send = false, fail_limit = false, fail_certificates = false;
int certificate_loads = 0, connections = 0;
unsigned verify_errors = 0;
std::size_t ca_offset = 0;
const std::string ca_bundle = "# public roots\n-----BEGIN CERTIFICATE-----\nroot one\n"
                              "-----END CERTIFICATE-----\n-----BEGIN CERTIFICATE-----\nroot two\n"
                              "-----END CERTIFICATE-----\n";
std::size_t header_limit = 0;
const std::string response_headers =
    "HTTP/1.1 302 Found\r\nContent-Security-Policy: " + std::string(6000, 'x') +
    "\r\nLocation: https://release-assets.githubusercontent.com/file\r\n";
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
    /* A packaged /app0 holding only the CA bundle; every other file is absent. */
    int open(const char *path, int flags, ...)
    {
        if (std::string_view(path) != "/app0")
        {
            errno = ENOENT;
            return -1;
        }
        assert((flags & (O_DIRECTORY | O_NOFOLLOW)) == (O_DIRECTORY | O_NOFOLLOW));
        return 121;
    }
    int _openat(int parent, const char *path, int flags, ...)
    {
        assert(flags & O_NOFOLLOW);
        if (parent == 121 && std::string_view(path) == "assets" && (flags & O_DIRECTORY))
            return 122;
        if (parent == 122 && std::string_view(path) == "ca-certificates.pem")
        {
            ca_offset = 0;
            return 123;
        }
        errno = ENOENT;
        return -1;
    }
    int fstat(int fd, struct stat *st)
    {
        assert(fd >= 121 && fd <= 123);
        *st = {};
        st->st_mode = fd == 123 ? S_IFREG : S_IFDIR;
        st->st_size = fd == 123 ? static_cast<off_t>(ca_bundle.size()) : 0;
        st->st_ino = static_cast<ino_t>(fd);
        return 0;
    }
    off_t lseek(int fd, off_t offset, int whence)
    {
        assert(fd == 123 && whence == SEEK_SET && offset == 0);
        ca_offset = 0;
        return 0;
    }
    long read(int fd, void *buffer, std::size_t size)
    {
        assert(fd == 123);
        const auto count = std::min(size, ca_bundle.size() - ca_offset);
        std::memcpy(buffer, ca_bundle.data() + ca_offset, count);
        ca_offset += count;
        return static_cast<long>(count);
    }
    int close(int fd)
    {
        assert(fd >= 121 && fd <= 123);
        return 0;
    }
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
    int sceHttpsLoadCert(int ctx, int count, const HttpsCertificate **list, const void *cert,
                         const void *key)
    {
        assert(ctx == 3 && count == 2 && !cert && !key);
        const auto expected = slopfin::http::pem_certificates(ca_bundle);
        for (int i = 0; i < count; ++i)
            assert(std::string_view(list[i]->data, list[i]->size) == expected[i]);
        ++certificate_loads;
        return fail_certificates ? -1 : 0;
    }
    int sceHttpsGetSslError(int, int *error, unsigned *detail)
    {
        *error = -1;
        *detail = verify_errors;
        return 0;
    }
    int sceHttpCreateTemplate(int ctx, const char *, int version, int)
    {
        assert(ctx == 3 && version == 2);
        return 4;
    }
    int sceHttpCreateConnectionWithURL(int tmpl, const char *url, int)
    {
        assert(tmpl == 4);
        ++connections;
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
    int sceHttpSetResponseHeaderMaxSize(int, std::size_t size)
    {
        header_limit = size;
        return fail_limit ? -1 : 0;
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
        return fail_send ? -1 : header_limit < response_headers.size() ? -2143088525 : 0;
    }
    int sceHttpGetAllResponseHeaders(int, char **headers, std::size_t *size)
    {
        *headers = const_cast<char *>(response_headers.data());
        *size = response_headers.size();
        return 0;
    }
    int sceHttpGetStatusCode(int, int *status)
    {
        *status = response_code;
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
int main(int argc, char **)
{
    using slopfin::http::WebRequest;
    assert(slopfin::http::pem_certificates("-----BEGIN CERTIFICATE-----").empty());
    assert(slopfin::http::pem_certificates("no certificates").empty());
    if (argc > 1)
    {
        fail_certificates = true;
        WebRequest request;
        assert(!request.open("https://example.test/", "GET", {}, {}, {}));
        assert(certificate_loads == 1 && connections == 0);
        std::puts("Native HTTPS refuses requests when its public CAs cannot load");
        return 0;
    }
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
    response_code = 302;
    {
        WebRequest request;
        assert(request.open("https://github.com/BeLikeBrett/SlopFin/releases/download/file", "GET",
                            {}, {}, {}));
        assert(request.status() == 302);
        assert(request.location() == "https://release-assets.githubusercontent.com/file");
    }
    fail_limit = true;
    {
        WebRequest request;
        assert(!request.open("https://example.test/", "GET", {}, {}, {}));
    }
    fail_limit = false;
    fail_send = true;
    verify_errors = 0x04;
    {
        WebRequest request;
        assert(!request.open("https://example.test/", "GET", {}, {}, {}));
        assert(request.certificate_errors() == 0x04);
    }
    assert(pools == 1 && created == 5 && deleted == 5 && closed == 5);
    assert(certificate_loads == 1);
    std::puts("Native HTTPS request, upload, cancellation and cleanup tests passed");
}
