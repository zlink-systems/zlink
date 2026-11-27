/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/http/http_host_service.hpp"
#include "runtime/http/http_startup_test_probe.hpp"
#include <zlink/framework.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace zlink::framework;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;
constexpr termination_result_t stopped_terminal{termination_outcome_t::stopped,
                                                termination_reason_t::none};

std::string http_uri (const tcp::endpoint &endpoint)
{
    return "http://127.0.0.1:" + std::to_string (endpoint.port ());
}

void expect_bind_failure (const result_t<void> &result, const std::string &endpoint)
{
    ASSERT_FALSE (result.has_value ());
    ASSERT_NE (result.error (), nullptr);
    EXPECT_EQ (result.error_kind (), framework_error_kind_t::unavailable);
    const std::string message = result.error ()->what ();
    const boost::system::error_code expected = asio::error::address_in_use;
    EXPECT_NE (message.find (endpoint), std::string::npos);
    EXPECT_NE (message.find (expected.message ()), std::string::npos);
    EXPECT_NE (message.find ("error_code=" + std::string (expected.category ().name ()) + ":"
                             + std::to_string (expected.value ())),
               std::string::npos);
#ifndef _WIN32
    // On POSIX, the system-category value is the original bind errno.
    EXPECT_EQ (expected.value (), EADDRINUSE);
    EXPECT_NE (message.find (":" + std::to_string (EADDRINUSE) + "]"), std::string::npos);
#endif
}

TEST (HttpStartup, OccupiedPortReturnsFailureAndUnhealthy)
{
    asio::io_context io;
    tcp::acceptor occupied (io, {asio::ip::address_v4::loopback (), 0});
    const auto endpoint = http_uri (occupied.local_endpoint ());
    http_options_snapshot_t options;
    options.endpoints = {{endpoint, {}}};
    health_builder_t health;
    service_provider_t services;
    runtime::http_host_service_t host (options, health, 1,
                                       std::make_shared<runtime::listener_status_registry_t> ());

    const auto result = host.start (services).result ();
    ASSERT_NO_FATAL_FAILURE (expect_bind_failure (result, endpoint));
    EXPECT_EQ (health.report ().status, health_status_t::unhealthy);
    EXPECT_FALSE (health.report ().ready ());
    ASSERT_EQ (health.report ().checks.size (), 1u);
    EXPECT_EQ (health.report ().checks.front ().message, result.error ()->what ());
    EXPECT_TRUE (occupied.is_open ());
    host.stop ();
    host.stop ();
    EXPECT_EQ (health.report ().status, health_status_t::unhealthy);
    EXPECT_EQ (health.report ().checks.front ().message, result.error ()->what ());
}

TEST (HttpStartup, LaterBindFailureReleasesEarlierListenerBeforeReturning)
{
    asio::io_context io;
    tcp::acceptor reservation (io, {asio::ip::address_v4::loopback (), 0});
    const auto first = reservation.local_endpoint ();
    tcp::acceptor occupied (io, {asio::ip::address_v4::loopback (), 0});
    const auto second = occupied.local_endpoint ();
    http_options_snapshot_t options;
    options.endpoints = {{http_uri (first), {}}, {http_uri (second), {}}};
    health_builder_t health;
    service_provider_t services;
    runtime::http_host_service_t host (options, health, 1,
                                       std::make_shared<runtime::listener_status_registry_t> ());
    reservation.close ();

    ASSERT_NO_FATAL_FAILURE (
      expect_bind_failure (host.start (services).result (), http_uri (second)));
    EXPECT_EQ (health.report ().status, health_status_t::unhealthy);
    // Keep the failed host alive: its destructor must not be needed to free the port.
    tcp::acceptor reclaimed (io);
    reclaimed.open (first.protocol ());
    boost::system::error_code error;
    reclaimed.bind (first, error);
    ASSERT_FALSE (error) << error.message ();
    reclaimed.close ();
    occupied.close ();

    // The same host can start cleanly after the external conflict is removed.
    ASSERT_TRUE (host.start (services).result ().has_value ());
    EXPECT_EQ (health.report ().status, health_status_t::healthy);
    host.request_stop ();
    EXPECT_FALSE (health.report ().ready ());
    host.stop ();
    reclaimed.open (first.protocol ());
    reclaimed.bind (first, error);
    EXPECT_FALSE (error) << error.message ();
}

TEST (HttpStartup, DuplicateListenerProvesFirstBindWasRolledBack)
{
    asio::io_context io;
    tcp::acceptor reservation (io, {asio::ip::address_v4::loopback (), 0});
    const auto endpoint = reservation.local_endpoint ();
    const auto uri = http_uri (endpoint);
    http_options_snapshot_t options;
    options.endpoints = {{uri, {}}, {uri, {}}};
    health_builder_t health;
    service_provider_t services;
    runtime::http_host_service_t host (options, health, 1,
                                       std::make_shared<runtime::listener_status_registry_t> ());
    reservation.close ();

    // The second bind conflicts with this host's first listener.
    ASSERT_NO_FATAL_FAILURE (expect_bind_failure (host.start (services).result (), uri));
    EXPECT_EQ (health.report ().status, health_status_t::unhealthy);
    tcp::acceptor reclaimed (io);
    reclaimed.open (endpoint.protocol ());
    boost::system::error_code error;
    reclaimed.bind (endpoint, error);
    EXPECT_FALSE (error) << error.message ();
}

TEST (HttpStartup, SuccessfulStartOwnsThePortUntilDestruction)
{
    asio::io_context io;
    tcp::acceptor reservation (io, {asio::ip::address_v4::loopback (), 0});
    const auto endpoint = reservation.local_endpoint ();
    http_options_snapshot_t options;
    options.endpoints = {{http_uri (endpoint), {}}};
    health_builder_t health;
    service_provider_t services;
    reservation.close ();
    tcp::acceptor probe (io);
    probe.open (endpoint.protocol ());
    boost::system::error_code error;
    {
        runtime::http_host_service_t host (
          options, health, 1, std::make_shared<runtime::listener_status_registry_t> ());
        ASSERT_TRUE (host.start (services).result ().has_value ());
        EXPECT_EQ (health.report ().status, health_status_t::healthy);
        probe.bind (endpoint, error);
        EXPECT_EQ (error, asio::error::address_in_use);
    }
    EXPECT_EQ (health.report ().status, health_status_t::unhealthy);
    probe.bind (endpoint, error);
    EXPECT_FALSE (error) << error.message ();
}

TEST (HttpStartup, RequestStopAfterFailedRunReturnsCompletedShutdown)
{
    asio::io_context io;
    tcp::acceptor occupied (io, {asio::ip::address_v4::loopback (), 0});
    {
        auto app = app_t::create ();
        app.add_zlink_framework ().http ().listen (http_uri (occupied.local_endpoint ()));
        ASSERT_EQ (app.run (0, nullptr), 1);
        app.request_stop ();
        auto shutdown = app.shutdown (1s);
        EXPECT_TRUE (shutdown.await_ready ());
        EXPECT_EQ (shutdown.result ().value (), stopped_terminal);
    }
    EXPECT_TRUE (occupied.is_open ());
}

TEST (HttpStartup, StopAfterFailedRunAllowsDestruction)
{
    asio::io_context io;
    tcp::acceptor occupied (io, {asio::ip::address_v4::loopback (), 0});
    {
        auto app = app_t::create ();
        app.add_zlink_framework ().http ().listen (http_uri (occupied.local_endpoint ()));
        ASSERT_EQ (app.run (0, nullptr), 1);
        app.stop ();
    }
    EXPECT_TRUE (occupied.is_open ());
}

TEST (HttpStartup, ShutdownTwiceAfterFailedRunReturnsSameCompletedResult)
{
    asio::io_context io;
    tcp::acceptor occupied (io, {asio::ip::address_v4::loopback (), 0});
    auto app = app_t::create ();
    app.add_zlink_framework ().http ().listen (http_uri (occupied.local_endpoint ()));
    ASSERT_EQ (app.run (0, nullptr), 1);
    auto first = app.shutdown (1s);
    EXPECT_TRUE (first.await_ready ());
    const auto terminal = first.result ().value ();
    EXPECT_EQ (terminal, stopped_terminal);
    auto second = app.shutdown (1s);
    EXPECT_TRUE (second.await_ready ());
    EXPECT_EQ (second.result ().value (), terminal);
}

TEST (HttpStartup, ShutdownAfterSuccessfulRunReturnsSameCompletedResult)
{
    auto app = app_t::create ();
    app.add_zlink_framework ().http ().listen ("http://127.0.0.1:0");
    auto running = std::async (std::launch::async, [&] { return app.run (0, nullptr); });
    const auto deadline = std::chrono::steady_clock::now () + 3s;
    while (!app.is_ready () && std::chrono::steady_clock::now () < deadline)
        std::this_thread::sleep_for (1ms);
    EXPECT_TRUE (app.is_ready ());
    const auto terminal = app.shutdown (1s).result ().value ();
    EXPECT_EQ (running.get (), 0);
    EXPECT_EQ (terminal, stopped_terminal);
    auto second = app.shutdown (1s);
    EXPECT_TRUE (second.await_ready ());
    EXPECT_EQ (second.result ().value (), terminal);
}

class teardown_barrier_service_t final : public hosted_service_t
{
  public:
    std::promise<void> entered;
    std::promise<void> release;

    task_t<void> start (service_provider_t &) override { co_return; }

    void stop () noexcept override
    {
        entered.set_value ();
        release.get_future ().wait ();
    }
};

TEST (HttpStartup, ShutdownDuringFailedRunTeardownSharesTerminalResult)
{
    asio::io_context io;
    tcp::acceptor occupied (io, {asio::ip::address_v4::loopback (), 0});
    auto app = app_t::create ();
    auto service = std::make_unique<teardown_barrier_service_t> ();
    auto *barrier = service.get ();
    auto entered = barrier->entered.get_future ();
    app.add_hosted_service (std::move (service));
    app.add_zlink_framework ().http ().listen (http_uri (occupied.local_endpoint ()));
    auto running = std::async (std::launch::async, [&] { return app.run (0, nullptr); });
    const auto status = entered.wait_for (3s);
    EXPECT_EQ (status, std::future_status::ready);
    auto shutdown = app.shutdown (1s);
    EXPECT_FALSE (shutdown.await_ready ());
    barrier->release.set_value ();
    EXPECT_EQ (running.get (), 1);
    EXPECT_TRUE (shutdown.await_ready ());
    const auto terminal = shutdown.result ().value ();
    EXPECT_EQ (terminal, stopped_terminal);
    EXPECT_EQ (app.shutdown (1s).result ().value (), terminal);
}

#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
using runtime::http_startup_event_t;

struct http_startup_probe_scope_t
{
    explicit http_startup_probe_scope_t (std::function<void (http_startup_event_t)> observe)
    {
        runtime::http_startup_test_probe = std::move (observe);
    }

    ~http_startup_probe_scope_t () { runtime::http_startup_test_probe = {}; }
};

TEST (HttpStartup, ShutdownBeforeStartupFailureJoinsWorkerBeforeReturning)
{
    asio::io_context io;
    tcp::acceptor occupied (io, {asio::ip::address_v4::loopback (), 0});
    const auto endpoint = occupied.local_endpoint ();
    auto app = app_t::create ();
    app.add_zlink_framework ().http ().listen (http_uri (endpoint));

    std::promise<void> before_bind;
    std::promise<void> bind_release;
    std::promise<void> before_seal;
    std::promise<void> seal_release;
    std::promise<void> rolled_back;
    std::mutex events_mutex;
    std::vector<std::string> events;
    const auto record = [&] (const char *event) {
        const std::lock_guard lock (events_mutex);
        events.emplace_back (event);
    };
    const http_startup_probe_scope_t probe ([&] (http_startup_event_t event) {
        switch (event) {
            case http_startup_event_t::before_bind:
                record ("listener published");
                before_bind.set_value ();
                bind_release.get_future ().wait ();
                break;
            case http_startup_event_t::before_seal_listener:
                record ("worker acquired listener");
                before_seal.set_value ();
                seal_release.get_future ().wait ();
                break;
            case http_startup_event_t::after_seal_listener:
                record ("worker finished listener call");
                break;
            case http_startup_event_t::listener_destroyed:
                record ("listener destroyed");
                break;
            case http_startup_event_t::rollback_complete:
                record ("rollback complete");
                rolled_back.set_value ();
                break;
            case http_startup_event_t::shutdown_worker_joined:
                record ("worker joined");
                break;
            case http_startup_event_t::accept_started:
                ADD_FAILURE () << "A listener with a failed bind started accepting";
                break;
        }
    });

    auto running = std::async (std::launch::async, [&] {
        const int result = app.run (0, nullptr);
        record ("run returned");
        return result;
    });
    const auto bind_status = before_bind.get_future ().wait_for (3s);
    if (bind_status != std::future_status::ready) {
        bind_release.set_value ();
        seal_release.set_value ();
        (void) running.get ();
        FAIL () << "Startup did not publish the listener before bind";
    }

    auto shutdown = app.shutdown (1s);
    const auto seal_status = before_seal.get_future ().wait_for (3s);
    EXPECT_FALSE (shutdown.await_ready ());
    bind_release.set_value ();
    const auto rollback_status = rolled_back.get_future ().wait_for (3s);
    const std::vector<std::string> through_rollback{
      "listener published", "worker acquired listener", "rollback complete"};
    {
        const std::lock_guard lock (events_mutex);
        EXPECT_EQ (events, through_rollback);
    }
    seal_release.set_value ();
    EXPECT_EQ (running.get (), 1);
    EXPECT_EQ (seal_status, std::future_status::ready);
    EXPECT_EQ (rollback_status, std::future_status::ready);
    EXPECT_EQ (events,
               (std::vector<std::string>{"listener published", "worker acquired listener",
                                         "rollback complete", "worker finished listener call",
                                         "listener destroyed", "worker joined", "run returned"}));
    RecordProperty ("lifecycle_order", ::testing::PrintToString (events));
    ASSERT_TRUE (shutdown.await_ready ());
    EXPECT_EQ (app.shutdown (1s).result ().value (), shutdown.result ().value ());

    occupied.close ();
    tcp::acceptor reclaimed (io);
    reclaimed.open (endpoint.protocol ());
    boost::system::error_code error;
    reclaimed.bind (endpoint, error);
    EXPECT_FALSE (error) << error.message ();
}

TEST (HttpStartup, SealBeforeStartPreventsAcceptLoop)
{
    http_options_snapshot_t options;
    options.endpoints = {{"http://127.0.0.1:0", {}}};
    health_builder_t health;
    service_provider_t services;
    runtime::http_host_service_t host (options, health, 1,
                                       std::make_shared<runtime::listener_status_registry_t> ());
    bool accept_started = false;
    const http_startup_probe_scope_t probe ([&] (http_startup_event_t event) {
        if (event == http_startup_event_t::accept_started)
            accept_started = true;
    });

    host.seal_application_dispatch ();
    ASSERT_TRUE (host.start (services).result ().has_value ());
    EXPECT_FALSE (accept_started);
    EXPECT_FALSE (health.report ().ready ());
}

#else
class startup_barrier_service_t final : public hosted_service_t
{
  public:
    std::promise<void> entered;
    std::promise<void> release;

    task_t<void> start (service_provider_t &) override
    {
        entered.set_value ();
        release.get_future ().wait ();
        co_return;
    }

    void stop () noexcept override {}
};

TEST (HttpStartup, ShutdownBeforeStartupFailureJoinsWorkerBeforeReturning)
{
    asio::io_context io;
    tcp::acceptor occupied (io, {asio::ip::address_v4::loopback (), 0});
    auto app = app_t::create ();
    auto service = std::make_unique<startup_barrier_service_t> ();
    auto *barrier = service.get ();
    auto entered = barrier->entered.get_future ();
    app.add_hosted_service (std::move (service));
    app.add_zlink_framework ().http ().listen (http_uri (occupied.local_endpoint ()));
    auto running = std::async (std::launch::async, [&] { return app.run (0, nullptr); });
    if (entered.wait_for (3s) != std::future_status::ready) {
        barrier->release.set_value ();
        (void) running.get ();
        FAIL () << "Startup did not reach the barrier";
    }
    auto shutdown = app.shutdown (1s);
    EXPECT_FALSE (shutdown.await_ready ());
    barrier->release.set_value ();
    EXPECT_EQ (running.get (), 1);
    EXPECT_TRUE (shutdown.await_ready ());
    EXPECT_EQ (app.shutdown (1s).result ().value (), shutdown.result ().value ());
}
#endif
} // namespace
