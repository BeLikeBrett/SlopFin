/*
 * SlopFin - minimal HTTP/1.1 client over BSD sockets.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "http.hpp"
#include "trace.hpp"
#include "server_address.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C"
{
    int socket(int domain, int type, int protocol);
    int connect(int fd, const void *address, unsigned int length);
    long send(int fd, const void *buffer, unsigned long length, int flags);
    long recv(int fd, void *buffer, unsigned long length, int flags);
    int close(int fd);
    int shutdown(int fd, int how);
    int setsockopt(int fd, int level, int option, const void *value, unsigned int length);
    int inet_pton(int family, const char *source, void *destination);
}

namespace
{
constexpr int kAfInet = 2;
constexpr int kSockStream = 1;
/*
 * The console runs a FreeBSD kernel and the Linux preview does not, and the
 * two disagree about both the socket address layout and the option numbers.
 * This is the one place in the app where that shows, because it is the one
 * place that talks to a kernel directly rather than through the C library.
 */
#ifdef SLOPFIN_HOST
constexpr int kSolSocket = 1;
constexpr int kSoRcvTimeo = 20;
constexpr int kSoSndTimeo = 21;
constexpr int kSoRcvBuf = 8;
#else
constexpr int kSolSocket = 0xffff;
constexpr int kSoRcvTimeo = 0x1006;
constexpr int kSoSndTimeo = 0x1005;
constexpr int kSoRcvBuf = 0x1002;
#endif
constexpr std::size_t kMaxBody = 24u * 1024u * 1024u;
constexpr int kTimeoutSeconds = 10;

#ifdef SLOPFIN_HOST
/* Linux sockaddr_in: family, port, addr, zero padding. No length byte. */
struct SockAddrIn
{
    std::uint16_t family;
    std::uint16_t port;
    std::uint32_t address;
    std::uint8_t zero[8];
};
#else
/* FreeBSD sockaddr_in: len, family, port, addr, zero padding. */
struct SockAddrIn
{
    std::uint8_t length;
    std::uint8_t family;
    std::uint16_t port;
    std::uint32_t address;
    std::uint8_t zero[8];
};
#endif

struct TimeVal
{
    long seconds;
    long microseconds;
};

std::uint16_t host_to_network16(std::uint16_t value) noexcept
{
    return static_cast<std::uint16_t>((value << 8) | (value >> 8));
}

int open_connection(std::string_view host, int port, bool media = false) noexcept
{
    std::string host_text(host);
    SockAddrIn address{};
#ifndef SLOPFIN_HOST
    address.length = sizeof(SockAddrIn);
#endif
    address.family = kAfInet;
    address.port = host_to_network16(static_cast<std::uint16_t>(port));
    if (inet_pton(kAfInet, host_text.c_str(), &address.address) != 1)
        return -1; /* Numeric addresses only; the server is configured by IP. */

    const int fd = socket(kAfInet, kSockStream, 0);
    if (fd < 0)
        return -1;

    if (media)
    {
        // Negotiate a useful TCP window before connecting. Video arrives in
        // bursts, and the transport producer briefly waits on audio output.
        // A tiny default receive window can turn those waits into long gaps.
        for (int bytes : {2 * 1024 * 1024, 1024 * 1024, 512 * 1024, 256 * 1024})
            if (setsockopt(fd, kSolSocket, kSoRcvBuf, &bytes, sizeof(bytes)) == 0)
            {
                slopfin::trace::mark("http: media receive buffer requested " +
                                     std::to_string(bytes));
                break;
            }
    }

    const TimeVal timeout{kTimeoutSeconds, 0};
    (void)setsockopt(fd, kSolSocket, kSoRcvTimeo, &timeout, sizeof(timeout));
    (void)setsockopt(fd, kSolSocket, kSoSndTimeo, &timeout, sizeof(timeout));

    if (connect(fd, &address, sizeof(address)) < 0)
    {
        (void)close(fd);
        return -1;
    }
    return fd;
}

bool send_all(int fd, std::string_view data) noexcept
{
    std::size_t sent = 0;
    while (sent < data.size())
    {
        const long written = send(fd, data.data() + sent, data.size() - sent, 0);
        if (written <= 0)
            return false;
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

/* Reads until the server closes or the declared body length is satisfied. */
bool read_response(int fd, int &status, std::string &body) noexcept
{
    std::string buffer;
    char chunk[16384];
    std::size_t header_end = std::string::npos;

    while (header_end == std::string::npos)
    {
        const long received = recv(fd, chunk, sizeof(chunk), 0);
        if (received <= 0)
            return false;
        buffer.append(chunk, static_cast<std::size_t>(received));
        header_end = buffer.find("\r\n\r\n");
        if (buffer.size() > 256u * 1024u && header_end == std::string::npos)
            return false;
    }

    const std::string headers = buffer.substr(0, header_end);
    if (std::sscanf(headers.c_str(), "HTTP/%*d.%*d %d", &status) != 1)
        return false;

    std::size_t content_length = std::string::npos;
    bool chunked = false;
    std::size_t line_start = headers.find("\r\n");
    while (line_start != std::string::npos)
    {
        const std::size_t line_end = headers.find("\r\n", line_start + 2);
        const std::string line = headers.substr(
            line_start + 2,
            (line_end == std::string::npos ? headers.size() : line_end) - line_start - 2);
        std::string lowered = line;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (lowered.rfind("content-length:", 0) == 0)
            content_length = static_cast<std::size_t>(std::strtoul(line.c_str() + 15, nullptr, 10));
        else if (lowered.rfind("transfer-encoding:", 0) == 0 &&
                 lowered.find("chunked") != std::string::npos)
            chunked = true;
        line_start = line_end;
    }

    body = buffer.substr(header_end + 4);
    while (true)
    {
        if (!chunked && content_length != std::string::npos && body.size() >= content_length)
            break;
        if (body.size() > kMaxBody)
            return false;
        const long received = recv(fd, chunk, sizeof(chunk), 0);
        if (received <= 0)
            break;
        body.append(chunk, static_cast<std::size_t>(received));
    }

    if (chunked)
    {
        /* Decode chunked transfer in place. */
        std::string decoded;
        std::size_t cursor = 0;
        while (cursor < body.size())
        {
            const std::size_t line_break = body.find("\r\n", cursor);
            if (line_break == std::string::npos)
                break;
            const auto size = static_cast<std::size_t>(
                std::strtoul(body.substr(cursor, line_break - cursor).c_str(), nullptr, 16));
            cursor = line_break + 2;
            if (size == 0)
                break;
            if (cursor + size > body.size())
                break;
            decoded.append(body, cursor, size);
            cursor += size + 2;
        }
        body = std::move(decoded);
    }
    else if (content_length != std::string::npos && body.size() > content_length)
    {
        body.resize(content_length);
    }
    return true;
}

std::string build_request(std::string_view method, std::string_view host, int port,
                          std::string_view path, const std::vector<std::string> &headers,
                          std::string_view body, std::string_view content_type) noexcept
{
    std::string request;
    request.reserve(512 + body.size());
    request.append(method).append(" ").append(path).append(" HTTP/1.1\r\n");
    request.append("Host: ").append(host).append(":").append(std::to_string(port)).append("\r\n");
    request.append("User-Agent: SlopFin/0.1 (PlayStation 5)\r\n");
    request.append("Accept: */*\r\n");
    request.append("Connection: close\r\n");
    for (const std::string &header : headers)
        request.append(header).append("\r\n");
    if (!body.empty())
    {
        request.append("Content-Type: ").append(content_type).append("\r\n");
        request.append("Content-Length: ").append(std::to_string(body.size())).append("\r\n");
    }
    request.append("\r\n");
    request.append(body);
    return request;
}
} // namespace

namespace slopfin::http
{

Response get(std::string_view host, int port, std::string_view path,
             const std::vector<std::string> &headers) noexcept
{
    return send("GET", host, port, path, headers, {}, {});
}

Response post(std::string_view host, int port, std::string_view path,
              const std::vector<std::string> &headers, std::string_view body,
              std::string_view content_type) noexcept
{
    return send("POST", host, port, path, headers, body, content_type);
}

bool send_bytes(std::string_view host, int port, std::string_view bytes) noexcept
{
    const int fd = open_connection(host, port);
    if (fd < 0)
        return false;
    const bool sent = send_all(fd, bytes);
    (void)close(fd);
    return sent;
}

Response send(std::string_view method, std::string_view host, int port, std::string_view path,
              const std::vector<std::string> &headers, std::string_view body,
              std::string_view content_type) noexcept
{
    Response response;
    if (server_address::web_transport(host))
    {
        WebRequest request;
        if (!request.open(server_address::url(host, port, path), std::string(method), headers, body,
                          content_type))
            return response;
        response.status = request.status();
        char chunk[16384];
        long count = 0;
        while ((count = request.read(chunk, sizeof(chunk))) > 0)
        {
            if (response.body.size() + static_cast<std::size_t>(count) > kMaxBody)
            {
                response.status = 0;
                response.body.clear();
                return response;
            }
            response.body.append(chunk, static_cast<std::size_t>(count));
        }
        if (count < 0)
        {
            response.status = 0;
            response.body.clear();
        }
        return response;
    }
    const int fd = open_connection(host, port);
    if (fd < 0)
        return response;
    std::string request = build_request(method, host, port, path, headers, body, content_type);
    /* A body-less POST or DELETE still says how long its body is; some
       endpoints wait for one otherwise. */
    if (body.empty() && method != "GET")
        request.insert(request.size() - 2, "Content-Length: 0\r\n");
    if (send_all(fd, request))
        (void)read_response(fd, response.status, response.body);
    (void)close(fd);
    return response;
}

std::vector<unsigned char> get_binary(std::string_view host, int port, std::string_view path,
                                      const std::vector<std::string> &headers) noexcept
{
    const Response response = get(host, port, path, headers);
    if (!response.ok())
        return {};
    return {response.body.begin(), response.body.end()};
}

Stream::~Stream()
{
    close();
}

bool Stream::open(std::string_view host, int port, std::string_view path,
                  const std::vector<std::string> &headers) noexcept
{
    close();
    if (server_address::web_transport(host))
    {
        web_ = std::make_unique<WebRequest>();
        if (!web_->open(server_address::url(host, port, path), "GET", headers, {}, {}, true))
        {
            close();
            return false;
        }
        status_ = web_->status();
        if (status_ < 200 || status_ >= 300)
        {
            close();
            return false;
        }
        return true;
    }
    descriptor_ = open_connection(host, port, true);
    if (descriptor_ < 0)
        return false;

    const std::string request = build_request("GET", host, port, path, headers, {}, {});
    if (!send_all(descriptor_, request))
    {
        close();
        return false;
    }

    // A cold HDR tone-map transcode can take more than ten seconds to produce
    // its first response. Give media startup its own budget, then restore the
    // normal read timeout once headers arrive. API/artwork requests stay fast.
    const TimeVal startup_timeout{30, 0};
    (void)setsockopt(descriptor_, kSolSocket, kSoRcvTimeo, &startup_timeout,
                     sizeof(startup_timeout));
    /* Read only as far as the end of the headers; the rest is body. */
    std::string buffer;
    char chunk[8192];
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos)
    {
        const long received = recv(descriptor_, chunk, sizeof(chunk), 0);
        if (received <= 0)
        {
            close();
            return false;
        }
        buffer.append(chunk, static_cast<std::size_t>(received));
        header_end = buffer.find("\r\n\r\n");
        if (header_end == std::string::npos && buffer.size() > 128u * 1024u)
        {
            close();
            return false;
        }
    }
    if (std::sscanf(buffer.c_str(), "HTTP/%*d.%*d %d", &status_) != 1 || status_ < 200 ||
        status_ >= 300)
    {
        close();
        return false;
    }

    const TimeVal playback_timeout{kTimeoutSeconds, 0};
    (void)setsockopt(descriptor_, kSolSocket, kSoRcvTimeo, &playback_timeout,
                     sizeof(playback_timeout));

    /*
     * A live transcode has no content length, so the server frames the body in
     * chunks. Those length prefixes are not part of the payload; handing them
     * to a demultiplexer injects rubbish into the stream every few kilobytes.
     */
    const std::string header_block = buffer.substr(0, header_end);
    std::string lowered = header_block;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    chunked_ = lowered.find("transfer-encoding:") != std::string::npos &&
               lowered.find("chunked") != std::string::npos;

    pending_ = buffer.substr(header_end + 4);
    chunk_remaining_ = 0;
    finished_ = false;
    return true;
}

bool Stream::fill() noexcept
{
    if (descriptor_ < 0)
        return false;
    char chunk[16384];
    const long received = recv(descriptor_, chunk, sizeof(chunk), 0);
    if (received <= 0)
        return false;
    pending_.append(chunk, static_cast<std::size_t>(received));
    return true;
}

long Stream::read(void *buffer, std::size_t capacity) noexcept
{
    if (web_)
        return web_->read(buffer, capacity);
    if (capacity == 0 || finished_)
        return finished_ ? 0 : -1;

    if (!chunked_)
    {
        if (pending_.empty() && !fill())
            return 0;
        const std::size_t take = pending_.size() < capacity ? pending_.size() : capacity;
        std::memcpy(buffer, pending_.data(), take);
        pending_.erase(0, take);
        return static_cast<long>(take);
    }

    /* Consume chunk framing until some payload is available. */
    while (chunk_remaining_ == 0)
    {
        std::size_t line_end = pending_.find("\r\n");
        while (line_end == std::string::npos)
        {
            if (!fill())
                return 0;
            line_end = pending_.find("\r\n");
        }
        /* A chunk's trailing blank line appears as an empty size line. */
        if (line_end == 0)
        {
            pending_.erase(0, 2);
            continue;
        }
        const auto size = static_cast<std::size_t>(
            std::strtoul(pending_.substr(0, line_end).c_str(), nullptr, 16));
        pending_.erase(0, line_end + 2);
        if (size == 0)
        {
            finished_ = true;
            return 0;
        }
        chunk_remaining_ = size;
    }

    if (pending_.empty() && !fill())
        return 0;

    std::size_t take = pending_.size() < capacity ? pending_.size() : capacity;
    if (take > chunk_remaining_)
        take = chunk_remaining_;
    std::memcpy(buffer, pending_.data(), take);
    pending_.erase(0, take);
    chunk_remaining_ -= take;
    return static_cast<long>(take);
}

void Stream::interrupt() noexcept
{
    if (web_)
        web_->interrupt();
    if (descriptor_ >= 0)
        (void)::shutdown(descriptor_, 2);
}

void Stream::close() noexcept
{
    web_.reset();
    if (descriptor_ >= 0)
        (void)::close(descriptor_);
    descriptor_ = -1;
    pending_.clear();
    chunk_remaining_ = 0;
    chunked_ = false;
    finished_ = false;
}

std::string url_encode(std::string_view value) noexcept
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (const char raw : value)
    {
        const auto ch = static_cast<unsigned char>(raw);
        const bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
                                ch == '~';
        if (unreserved)
        {
            out.push_back(raw);
            continue;
        }
        out.push_back('%');
        out.push_back(kHex[ch >> 4]);
        out.push_back(kHex[ch & 0x0fu]);
    }
    return out;
}

} // namespace slopfin::http
