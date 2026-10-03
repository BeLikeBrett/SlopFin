/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/* SlopFin - verified HTTPS and DNS through the platform HTTP stack.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_WEB_TRANSPORT_HPP
#define SLOPFIN_WEB_TRANSPORT_HPP
#include <atomic>
#include <string>
#include <vector>
namespace slopfin::http
{
class WebRequest
{
  public:
    ~WebRequest();
    bool open(const std::string &url, const std::string &method,
              const std::vector<std::string> &headers, std::string_view body,
              std::string_view content_type, bool media = false) noexcept;
    long read(void *buffer, std::size_t capacity) noexcept;
    void interrupt() noexcept;
    const std::string &location() const noexcept
    {
        return location_;
    }
    int status() const noexcept
    {
        return status_;
    }

  private:
    int status_ = 0;
    std::string location_;
    std::atomic<bool> interrupted_{false};
#ifdef SLOPFIN_HOST
    void *easy_ = nullptr;
    void *multi_ = nullptr;
    void *headers_ = nullptr;
    std::vector<unsigned char> pending_;
    bool done_ = false, failed_ = false, paused_ = false;
    bool pump() noexcept;
    static std::size_t write(char *, std::size_t, std::size_t, void *);
#else
    std::atomic<int> request_{-1};
    int connection_ = -1;
#endif
};
} // namespace slopfin::http
#endif
