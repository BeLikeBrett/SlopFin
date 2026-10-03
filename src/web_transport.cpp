/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/* SlopFin - verified HTTPS and DNS through the platform HTTP stack.
 * PS5 API declarations follow ProsperoTV's iptv_http.cpp, copyright
 * BlackBearReloaded, GPL-3.0-or-later. */
#include "web_transport.hpp"
#include "trace.hpp"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <mutex>
#ifdef SLOPFIN_HOST
#include <curl/curl.h>
namespace slopfin::http
{
std::size_t WebRequest::write(char *bytes, std::size_t size, std::size_t count, void *context)
{
    auto &self = *static_cast<WebRequest *>(context);
    const auto length = size * count;
    if (self.interrupted_)
        return 0;
    if (self.pending_.size() + length > 256u * 1024u)
    {
        self.paused_ = true;
        return CURL_WRITEFUNC_PAUSE;
    }
    self.pending_.insert(self.pending_.end(), bytes, bytes + length);
    return length;
}
bool WebRequest::pump() noexcept
{
    if (interrupted_)
        return false;
    int running = 0;
    if (curl_multi_perform(static_cast<CURLM *>(multi_), &running) != CURLM_OK)
        return false;
    int left = 0;
    while (auto *message = curl_multi_info_read(static_cast<CURLM *>(multi_), &left))
        if (message->msg == CURLMSG_DONE)
        {
            done_ = true;
            failed_ = message->data.result != CURLE_OK;
        }
    long code = 0;
    curl_easy_getinfo(static_cast<CURL *>(easy_), CURLINFO_RESPONSE_CODE, &code);
    status_ = static_cast<int>(code);
    char *redirect = nullptr;
    curl_easy_getinfo(static_cast<CURL *>(easy_), CURLINFO_REDIRECT_URL, &redirect);
    if (redirect != nullptr)
        location_ = redirect;
    if (!done_ && pending_.empty())
        curl_multi_poll(static_cast<CURLM *>(multi_), nullptr, 0, 100, nullptr);
    return !failed_;
}
bool WebRequest::open(const std::string &url, const std::string &method,
                      const std::vector<std::string> &headers, std::string_view body,
                      std::string_view content_type, bool media) noexcept
{
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    easy_ = curl_easy_init();
    multi_ = curl_multi_init();
    if (!easy_ || !multi_)
        return false;
    auto *easy = static_cast<CURL *>(easy_);
    auto *list = static_cast<curl_slist *>(headers_);
    for (auto &header : headers)
        list = curl_slist_append(list, header.c_str());
    if (!content_type.empty())
        list = curl_slist_append(list, ("Content-Type: " + std::string(content_type)).c_str());
    headers_ = list;
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method.c_str());
    if (method != "GET")
    {
        // open completes the request upload before returning; caller owns body.
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body.empty() ? "" : body.data());
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    }
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "SlopFin/0.1 (PlayStation 5)");
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, media ? 30L : 10L);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &WebRequest::write);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, this);
    // Host previews can trust an explicit CA bundle while still verifying TLS.
    if (const char *bundle = std::getenv("SSL_CERT_FILE"); bundle != nullptr && *bundle != '\0')
        curl_easy_setopt(easy, CURLOPT_CAINFO, bundle);
    // TLS verification stays enabled. Redirects are not followed with tokens.
    if (curl_multi_add_handle(static_cast<CURLM *>(multi_), easy) != CURLM_OK)
        return false;
    while ((!status_ || (status_ >= 300 && status_ < 400 && location_.empty())) && !done_)
        if (!pump())
            return false;
    return status_ != 0 && !failed_;
}
long WebRequest::read(void *buffer, std::size_t capacity) noexcept
{
    if (interrupted_)
        return -1;
    if (!capacity)
        return 0;
    if (paused_ && pending_.empty())
    {
        paused_ = false;
        curl_easy_pause(static_cast<CURL *>(easy_), CURLPAUSE_CONT);
    }
    while (pending_.empty() && !done_)
        if (!pump())
            return -1;
    if (failed_)
        return -1;
    const auto count = std::min(capacity, pending_.size());
    std::memcpy(buffer, pending_.data(), count);
    pending_.erase(pending_.begin(), pending_.begin() + count);
    return static_cast<long>(count);
}
void WebRequest::interrupt() noexcept
{
    interrupted_ = true;
}
WebRequest::~WebRequest()
{
    if (multi_ && easy_)
        curl_multi_remove_handle(static_cast<CURLM *>(multi_), static_cast<CURL *>(easy_));
    if (easy_)
        curl_easy_cleanup(static_cast<CURL *>(easy_));
    if (multi_)
        curl_multi_cleanup(static_cast<CURLM *>(multi_));
    if (headers_)
        curl_slist_free_all(static_cast<curl_slist *>(headers_));
}
} // namespace slopfin::http
#else
extern "C"
{
    int sceNetPoolCreate(const char *, int, int);
    int sceSslInit(std::size_t);
    int sceHttpInit(int, int, std::size_t);
    int sceHttpCreateTemplate(int, const char *, int, int);
    int sceHttpCreateConnectionWithURL(int, const char *, int);
    int sceHttpCreateRequestWithURL2(int, const char *, const char *, std::uint64_t);
    int sceHttpAddRequestHeader(int, const char *, const char *, std::uint32_t);
    int sceHttpSetAutoRedirect(int, int);
    int sceHttpSetResolveTimeOut(int, std::uint32_t);
    int sceHttpSetConnectTimeOut(int, std::uint32_t);
    int sceHttpSetRecvTimeOut(int, std::uint32_t);
    int sceHttpSetSendTimeOut(int, std::uint32_t);
    int sceHttpSendRequest(int, const void *, std::size_t);
    int sceHttpGetStatusCode(int, int *);
    int sceHttpGetAllResponseHeaders(int, char **, std::size_t *);
    int sceHttpReadData(int, void *, std::size_t);
    int sceHttpAbortRequest(int);
    int sceHttpDeleteRequest(int);
    int sceHttpDeleteConnection(int);
}
namespace
{
std::mutex initialization;
int network_template = -1;
bool initialized = false;
bool initialize()
{
    std::lock_guard<std::mutex> guard(initialization);
    if (initialized)
        return network_template >= 0;
    initialized = true;
    int pool = sceNetPoolCreate("slopfin_https", 64 * 1024, 0);
    if (pool < 0)
    {
        slopfin::trace::mark("https: net pool failed " + std::to_string(pool));
        return false;
    }
    int ssl = sceSslInit(512 * 1024);
    if (ssl < 0)
    {
        slopfin::trace::mark("https: SSL initialization failed " + std::to_string(ssl));
        return false;
    }
    int http = sceHttpInit(pool, ssl, 512 * 1024);
    if (http < 0)
    {
        slopfin::trace::mark("https: HTTP initialization failed " + std::to_string(http));
        return false;
    }
    network_template = sceHttpCreateTemplate(http, "SlopFin/0.1 (PlayStation 5)", 2, 0);
    slopfin::trace::mark("https: platform transport initialized " +
                         std::to_string(network_template));
    return network_template >= 0;
}
} // namespace
namespace slopfin::http
{
bool WebRequest::open(const std::string &url, const std::string &method,
                      const std::vector<std::string> &headers, std::string_view body,
                      std::string_view content_type, bool media) noexcept
{
    if (!initialize())
        return false;
    connection_ = sceHttpCreateConnectionWithURL(network_template, url.c_str(), 0);
    if (connection_ < 0)
    {
        trace::mark("https: connection failed " + std::to_string(connection_));
        return false;
    }
    int request =
        sceHttpCreateRequestWithURL2(connection_, method.c_str(), url.c_str(), body.size());
    request_ = request;
    if (request < 0)
    {
        trace::mark("https: request failed " + std::to_string(request));
        return false;
    }
    if (sceHttpSetAutoRedirect(request, 0) < 0 || sceHttpSetResolveTimeOut(request, 10000000) < 0 ||
        sceHttpSetConnectTimeOut(request, 10000000) < 0 ||
        sceHttpSetSendTimeOut(request, 10000000) < 0 ||
        sceHttpSetRecvTimeOut(request, media ? 30000000 : 10000000) < 0)
        return false;
    for (const auto &header : headers)
    {
        const auto colon = header.find(':');
        if (colon == std::string::npos)
            return false;
        auto value = header.substr(colon + 1);
        while (!value.empty() && value.front() == ' ')
            value.erase(0, 1);
        if (sceHttpAddRequestHeader(request, header.substr(0, colon).c_str(), value.c_str(), 0) < 0)
            return false;
    }
    if (!content_type.empty() &&
        sceHttpAddRequestHeader(request, "Content-Type", std::string(content_type).c_str(), 0) < 0)
        return false;
    int sent = sceHttpSendRequest(request, body.empty() ? nullptr : body.data(), body.size());
    if (sent < 0)
    {
        trace::mark("https: send failed " + std::to_string(sent));
        return false;
    }
    if (sceHttpGetStatusCode(request, &status_) < 0)
        return false;
    if (status_ >= 300 && status_ < 400)
    {
        char *raw = nullptr;
        std::size_t length = 0;
        if (sceHttpGetAllResponseHeaders(request, &raw, &length) < 0 || raw == nullptr ||
            length > 65536)
            return false;
        std::string_view all(raw, length);
        for (std::size_t start = 0; start < all.size();)
        {
            const auto end = all.find("\r\n", start);
            const auto line =
                all.substr(start, end == std::string_view::npos ? all.size() - start : end - start);
            if (line.size() >= 9)
            {
                std::string name(line.substr(0, 9));
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (name == "location:")
                {
                    auto value = line.substr(9);
                    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                        value.remove_prefix(1);
                    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
                        value.remove_suffix(1);
                    location_ = value;
                }
            }
            if (end == std::string_view::npos)
                break;
            start = end + 2;
        }
    }
    trace::mark("https: response " + std::to_string(status_));
    return true;
}
long WebRequest::read(void *buffer, std::size_t capacity) noexcept
{
    if (interrupted_)
        return -1;
    return sceHttpReadData(request_, buffer, capacity);
}
void WebRequest::interrupt() noexcept
{
    interrupted_ = true;
    int request = request_;
    if (request >= 0)
        (void)sceHttpAbortRequest(request);
}
WebRequest::~WebRequest()
{
    int request = request_.exchange(-1);
    if (request >= 0)
        (void)sceHttpDeleteRequest(request);
    if (connection_ >= 0)
        (void)sceHttpDeleteConnection(connection_);
}
} // namespace slopfin::http
#endif
