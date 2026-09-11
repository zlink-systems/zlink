/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/http/http_host_service.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <gtest/gtest.h>

#include <cerrno>
#include <string>

namespace
{
using namespace zlink::framework;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

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
                            + std::to_string (expected.value ())), std::string::npos);
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
    runtime::http_host_service_t host (options, health, 1);

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
    runtime::http_host_service_t host (options, health, 1);
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
    runtime::http_host_service_t host (options, health, 1);
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
        runtime::http_host_service_t host (options, health, 1);
        ASSERT_TRUE (host.start (services).result ().has_value ());
        EXPECT_EQ (health.report ().status, health_status_t::healthy);
        probe.bind (endpoint, error);
        EXPECT_EQ (error, asio::error::address_in_use);
    }
    EXPECT_EQ (health.report ().status, health_status_t::unhealthy);
    probe.bind (endpoint, error);
    EXPECT_FALSE (error) << error.message ();
}
} // namespace
