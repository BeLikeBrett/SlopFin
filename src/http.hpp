/*
 * SlopFin - minimal HTTP/1.1 client over BSD sockets.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bare LAN IPs use BSD sockets. DNS/HTTP(S) URLs use the platform HTTP
 * stack, including verified TLS, for API, artwork and streamed playback.
 */

#ifndef SLOPFIN_HTTP_HPP
#define SLOPFIN_HTTP_HPP

#include <memory>
#include "web_transport.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace slopfin::http
{

struct Response
{
    int status = 0;
    std::string body;

    [[nodiscard]] bool ok() const noexcept
    {
        return status >= 200 && status < 300;
    }
};

/* host is a bare address or name; port is explicit. path includes the query. */
Response get(std::string_view host, int port, std::string_view path,
             const std::vector<std::string> &headers) noexcept;

Response post(std::string_view host, int port, std::string_view path,
              const std::vector<std::string> &headers, std::string_view body,
              std::string_view content_type) noexcept;

/* Any method, for the calls that are neither a plain GET nor a POST: DELETE,
   or a POST whose body is not JSON.  */
Response send(std::string_view method, std::string_view host, int port, std::string_view path,
              const std::vector<std::string> &headers, std::string_view body,
              std::string_view content_type) noexcept;

/* Opens a TCP connection, sends the bytes, and closes it.  */
bool send_bytes(std::string_view host, int port, std::string_view bytes) noexcept;

/* Binary fetch used for artwork; returns an empty vector on failure. */
std::vector<unsigned char> get_binary(std::string_view host, int port, std::string_view path,
                                      const std::vector<std::string> &headers) noexcept;

/*
 * An open response body read incrementally. Video is far too large to buffer,
 * so playback pulls from one of these while it plays.
 */
class Stream final
{
  public:
    Stream() = default;
    ~Stream();
    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;

    bool open(std::string_view host, int port, std::string_view path,
              const std::vector<std::string> &headers) noexcept;

    /* Bytes read, 0 at end of stream, negative on error. */
    long read(void *buffer, std::size_t capacity) noexcept;

    /* Wake a blocked reader without closing/reusing its descriptor. Join before close. */
    void interrupt() noexcept;

    void close() noexcept;

    [[nodiscard]] int status() const noexcept
    {
        return status_;
    }
    [[nodiscard]] bool good() const noexcept
    {
        return descriptor_ >= 0 || web_ != nullptr;
    }

  private:
    /* Pulls more bytes from the socket into the pending buffer. */
    bool fill() noexcept;

    std::unique_ptr<WebRequest> web_;
    int descriptor_ = -1;
    int status_ = 0;
    bool chunked_ = false;
    std::size_t chunk_remaining_ = 0;
    bool finished_ = false;
    std::string pending_; /* socket bytes not yet handed to the caller */
};

/* Percent-encodes a value for use in a query string. */
std::string url_encode(std::string_view value) noexcept;

} // namespace slopfin::http

#endif
