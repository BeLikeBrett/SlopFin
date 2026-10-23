/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/reporter.cpp"
#include <cassert>
#include <iostream>
#include <thread>

namespace
{
pthread_t worker;
slopfin::config::Settings settings;
std::mutex mock_mutex;
std::condition_variable mock_changed;
bool block_requests = false, fail_stop = false;
int stop_status = 503;
struct Recorded
{
    std::string endpoint, body, authorization;
};
std::vector<Recorded> recorded;
bool wait_requests(std::size_t count)
{
    std::unique_lock<std::mutex> lock(mock_mutex);
    return mock_changed.wait_for(lock, std::chrono::seconds(2),
                                 [&] { return recorded.size() >= count; });
}
void unblock()
{
    std::lock_guard<std::mutex> lock(mock_mutex);
    block_requests = false;
    mock_changed.notify_all();
}
std::vector<Recorded> requests()
{
    std::lock_guard<std::mutex> lock(mock_mutex);
    return recorded;
}
void reset_mock(bool blocked = false)
{
    std::lock_guard<std::mutex> lock(mock_mutex);
    recorded.clear();
    fail_stop = false;
    block_requests = blocked;
}
} // namespace
extern "C" int scePthreadCreate(void **out, const void *attr, void *(*entry)(void *),
                                void *argument, const char *)
{
    const int result =
        pthread_create(&worker, static_cast<const pthread_attr_t *>(attr), entry, argument);
    *out = &worker;
    return result;
}
extern "C" int scePthreadSetaffinity(void *, std::uint64_t)
{
    return 0;
}
namespace slopfin::images
{
std::uint64_t spare_cores() noexcept
{
    return 0;
}
} // namespace slopfin::images
namespace slopfin::config
{
Settings &current() noexcept
{
    return settings;
}
} // namespace slopfin::config
namespace slopfin::trace
{
void mark(std::string_view) noexcept
{
}
} // namespace slopfin::trace
namespace slopfin::http
{
Response post(std::string_view, int, std::string_view endpoint,
              const std::vector<std::string> &headers, std::string_view body,
              std::string_view) noexcept
{
    std::unique_lock<std::mutex> lock(mock_mutex);
    recorded.push_back({std::string(endpoint), std::string(body), headers.at(0)});
    mock_changed.notify_all();
    mock_changed.wait(lock, [] { return !block_requests; });
    Response response;
    if (fail_stop && endpoint.ends_with("Stopped"))
    {
        fail_stop = false;
        response.status = stop_status;
    }
    else
        response.status = 204;
    return response;
}
} // namespace slopfin::http
int main()
{
    using namespace slopfin;
    const reporter::Session session{"item", "source", "session", "DirectPlay", 1, 4};
    settings.host = "fixture.invalid";
    settings.port = 8096;
    settings.token = "account-one";
    settings.user_id = "user-one";
    settings.device_id = "device";
    assert(reporter::body(session, 1.2345678, true).find("\"PositionTicks\":12345678") !=
           std::string::npos);
    assert(reporter::body(session, -1, false).find("\"PositionTicks\":0") != std::string::npos);
    assert(reporter::flush(20)); // Empty barrier.
    reporter::begin(session, 0.125);
    assert(reporter::flush(1000));
    reporter::update(1.2345678, true);
    assert(reporter::flush(1000));
    {
        std::lock_guard<std::mutex> lock(mock_mutex);
        fail_stop = true;
    }
    for (int i = 0; i < 8; ++i)
        reporter::end(2.7654321);
    assert(reporter::flush(2000));
    auto sent = requests();
    assert(sent.size() == 4);
    assert(sent[0].endpoint == "/Sessions/Playing");
    assert(sent[1].endpoint.ends_with("Progress"));
    assert(sent[1].body.find("\"IsPaused\":true") != std::string::npos);
    assert(sent[1].body.find("\"PositionTicks\":12345678") != std::string::npos);
    assert(sent[2].endpoint.ends_with("Stopped") && sent[3].body == sent[2].body);
    assert(sent[2].body.find("\"PositionTicks\":27654321") != std::string::npos);

    reset_mock(true);
    reporter::begin(session, 3.25);
    assert(wait_requests(1));
    assert(!reporter::flush(25)); // Queue is in flight, not delivered yet.
    unblock();
    assert(reporter::flush(1000));
    reporter::end(4.125);
    assert(reporter::flush(1000));

    // An old-account request may finish with its original token; it must never
    // retry or inherit new-account credentials after invalidation.
    reset_mock(true);
    reporter::begin(session, 5);
    assert(wait_requests(1));
    reporter::end(6);
    reporter::invalidate();
    settings.token = "account-two";
    settings.user_id = "user-two";
    reporter::begin(session, 7);
    unblock();
    assert(reporter::flush(1000));
    sent = requests();
    assert(sent.size() == 2);
    assert(sent[0].authorization.find("account-one") != std::string::npos);
    assert(sent[1].authorization.find("account-two") != std::string::npos);
    assert(sent[1].endpoint == "/Sessions/Playing");
    reporter::end(8);
    assert(reporter::flush(1000));

    reset_mock();
    reporter::begin(session, 8.125);
    assert(reporter::flush(1000));
    reporter::update(8.75, false);
    assert(requests().size() == 1); // Ordinary positions retain the 10-second cadence.
    {
        std::lock_guard<std::mutex> lock(reporter::g_mutex);
        reporter::g_next_progress = reporter::Clock::now();
        reporter::g_changed.notify_all();
    }
    assert(wait_requests(2));
    assert(reporter::flush(1000));
    assert(requests()[1].body.find("\"PositionTicks\":87500000") != std::string::npos);
    {
        std::lock_guard<std::mutex> lock(mock_mutex);
        fail_stop = true;
        stop_status = 400;
    }
    reporter::end(8.875);
    assert(!reporter::flush(1000)); // A permanent rejection cannot claim delivery.
    assert(requests().size() == 3);
    stop_status = 503;

    reset_mock(true);
    reporter::begin(session, 9);
    assert(wait_requests(1));
    for (int i = 0; i < 1000; ++i)
        reporter::update(i + 0.375, i % 2 == 0);
    for (int i = 0; i < 100; ++i)
    {
        reporter::end(i + 0.25);
        reporter::begin(session, i + 0.25);
    }
    reporter::end(100.5);
    {
        std::lock_guard<std::mutex> lock(reporter::g_mutex);
        assert(reporter::g_queue.size() <= reporter::kMaxEvents);
    }
    unblock();
    assert(reporter::flush(2000));
    sent = requests();
    int begins = 0, stops = 0;
    for (const auto &event : sent)
    {
        begins += event.endpoint == "/Sessions/Playing";
        stops += event.endpoint.ends_with("Stopped");
    }
    assert(begins == stops); // Every accepted begin retained its final stop.
    {
        std::lock_guard<std::mutex> lock(reporter::g_mutex);
        reporter::g_stop = true;
        reporter::g_changed.notify_all();
    }
    pthread_join(worker, nullptr);
    std::cout << "Reporter fractional ticks, pause/FIFO, stop retry, barriers, identity and bounds "
                 "passed\n";
}
