/* SPDX-License-Identifier: MPL-2.0 */

// Projection of the Core ROUTER selected-route observation (Core ROUTER §10.1):
// routes_snapshot(), poll_event_flag_t::pollroute and the route generation
// carried on a received ROUTER record. Scenario mirrors
// core/tests/integration/test_router_selected_route_contract.cpp.

#include "support.hpp"
#include <zlink.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#define REQUIRE(condition_)                                                                        \
    do {                                                                                           \
        if (!(condition_)) {                                                                       \
            std::fprintf (stderr, "%s:%d: requirement failed: %s\n", __FILE__, __LINE__,          \
                          #condition_);                                                            \
            std::abort ();                                                                         \
        }                                                                                          \
    } while (false)

static_assert (static_cast<short> (zlink::poll_event_flag_t::pollroute) == ZLINK_POLLROUTE,
               "poll_event_flag_t::pollroute must match ZLINK_POLLROUTE");

namespace
{

bool connect_ready_now (zlink::socket_t &socket_);

constexpr auto wait_limit = std::chrono::milliseconds (5000);

std::unique_ptr<zlink::router_socket_t> make_router (zlink::context_t &context_,
                                                     const std::string &name_)
{
    auto socket = std::make_unique<zlink::router_socket_t> (context_);
    socket->set_routing_id (zlink::routing_id_t::from (name_));
    socket->options ().linger (std::chrono::milliseconds (0));
    socket->options ().reconnect_interval (std::chrono::milliseconds (0));
    socket->options ().handover (true);
    return socket;
}

std::string bind_loopback (zlink::router_socket_t &socket_)
{
    socket_.bind ("tcp://127.0.0.1:*");
    return socket_.options ().last_endpoint ();
}

void connect_as (zlink::router_socket_t &source_, const std::string &peer_,
                 const std::string &endpoint_)
{
    source_.options ().connect_routing_id (zlink::routing_id_t::from (peer_));
    source_.connect (endpoint_);
}

std::uint64_t find_generation (const std::vector<zlink::router_route_t> &routes_,
                               const std::string &peer_)
{
    const auto expected = zlink::routing_id_t::from (peer_);
    std::uint64_t found = 0;
    std::size_t matches = 0;
    for (const auto &route : routes_) {
        REQUIRE (route.route_generation != 0);
        if (route.routing_id == expected) {
            ++matches;
            found = route.route_generation;
        }
    }
    REQUIRE (matches <= 1);
    return found;
}

// Waits on POLLROUTE until the peer's selected route has a generation other
// than old_; returns it. Core applies route changes while the socket is
// polled, so a socket without its own poller gets one here.
std::uint64_t wait_route (zlink::router_socket_t &socket_, zlink::poller_t *poller_,
                          const std::string &peer_, std::uint64_t old_ = 0)
{
    std::unique_ptr<zlink::poller_t> owned;
    if (poller_ == nullptr) {
        owned = std::make_unique<zlink::poller_t> ();
        owned->add (socket_, zlink::poll_event_flag_t::pollroute, 1);
        poller_ = owned.get ();
    }
    const auto deadline = std::chrono::steady_clock::now () + wait_limit;
    for (;;) {
        const auto generation = find_generation (socket_.routes_snapshot (), peer_);
        if (generation != 0 && generation != old_)
            return generation;
        REQUIRE (std::chrono::steady_clock::now () < deadline);
        zlink::poll_event_t event;
        (void) poller_->wait (&event, 1, std::chrono::milliseconds (10));
    }
}

bool route_ready_now (zlink::poller_t &poller_)
{
    zlink::poll_event_t event;
    const auto count = poller_.wait (&event, 1, std::chrono::milliseconds (0));
    return count == 1
           && (static_cast<short> (event.revents)
               & static_cast<short> (zlink::poll_event_flag_t::pollroute))
                != 0;
}

void send_text (zlink::router_socket_t &socket_, const std::string &peer_, const std::string &text_)
{
    auto part = zlink_cpp_contract::make_message (text_);
    socket_.send (zlink::routing_id_t::from (peer_)).message (part).submit ();
}

zlink::received_t recv_text (zlink::router_socket_t &socket_, const std::string &expected_)
{
    socket_.options ().recv_timeout (wait_limit);
    zlink::received_t received;
    REQUIRE (socket_.recv (received) == 0);
    REQUIRE (received.parts ().size () == 1);
    const auto &part = received.parts ()[0];
    REQUIRE (std::string (reinterpret_cast<const char *> (part.data ()), part.size ()) == expected_);
    return received;
}

void test_snapshot_pollroute_and_record_generation ()
{
    zlink::context_t context;
    auto server = make_router (context, "route-server");
    auto client = make_router (context, "route-client");
    const auto endpoint = bind_loopback (*server);

    REQUIRE (client->routes_snapshot ().empty ());

    zlink::poller_t poller;
    poller.add (*client, zlink::poll_event_flag_t::pollroute, 7);
    connect_as (*client, "route-server", endpoint);

    zlink::poll_event_t event;
    REQUIRE (poller.wait (&event, 1, wait_limit) == 1);
    REQUIRE (event.slot == 7);
    REQUIRE ((static_cast<short> (event.revents)
              & static_cast<short> (zlink::poll_event_flag_t::pollroute))
             != 0);

    REQUIRE (connect_ready_now (*client));
    const auto routes = client->routes_snapshot ();
    REQUIRE (connect_ready_now (*client));
    REQUIRE (client->connection_attempts_snapshot ().size () == 1);
    REQUIRE (!connect_ready_now (*client));
    REQUIRE (routes.size () == 1);
    const auto selected = find_generation (routes, "route-server");
    REQUIRE (selected != 0);
    // A successful snapshot that saw every change clears the level readiness.
    REQUIRE (!route_ready_now (poller));

    (void) wait_route (*server, nullptr, "route-client");
    send_text (*server, "route-client", "selected");
    auto input = zlink::poll_item_t::from_socket (*client, zlink::poll_event_flag_t::pollin);
    REQUIRE (zlink::poll (&input, 1, wait_limit) == 1);
    (void) client->connection_attempts_snapshot ();
    REQUIRE (zlink::poll (&input, 1, std::chrono::milliseconds (0)) == 1);
    REQUIRE ((static_cast<short> (input.revents) & ZLINK_POLLIN) != 0);
    const auto received = recv_text (*client, "selected");
    REQUIRE (received.route_generation () == selected);

    // A DEALER record carries no ROUTER route generation.
    zlink::dealer_socket_t dealer (context);
    dealer.set_routing_id (zlink::routing_id_t::from ("route-dealer"));
    dealer.options ().linger (std::chrono::milliseconds (0));
    dealer.connect (endpoint);
    (void) wait_route (*server, nullptr, "route-dealer");
    send_text (*server, "route-dealer", "dealer");
    dealer.options ().recv_timeout (wait_limit);
    zlink::received_t dealer_received;
    REQUIRE (dealer.recv (dealer_received) == 0);
    REQUIRE (dealer_received.route_generation () == 0);

    poller.close ();
}

void test_replaced_route_changes_generation ()
{
    zlink::context_t context;
    auto old_server = make_router (context, "route-server");
    auto new_server = make_router (context, "route-server");
    auto client = make_router (context, "route-client");
    const auto old_endpoint = bind_loopback (*old_server);
    const auto new_endpoint = bind_loopback (*new_server);

    zlink::poller_t poller;
    poller.add (*client, zlink::poll_event_flag_t::pollroute, 9);
    connect_as (*client, "route-server", old_endpoint);
    const auto first = wait_route (*client, &poller, "route-server");

    // Handover: the new pipe for the same RID takes over the selected route.
    connect_as (*client, "route-server", new_endpoint);
    const auto second = wait_route (*client, &poller, "route-server", first);
    REQUIRE (second != first);

    (void) wait_route (*new_server, nullptr, "route-client");
    send_text (*new_server, "route-client", "new");
    REQUIRE (recv_text (*client, "new").route_generation () == second);

    // The selected pipe ends: the retained standby is promoted with a new generation.
    new_server.reset ();
    const auto promoted = wait_route (*client, &poller, "route-server", second);
    REQUIRE (promoted != second);

    poller.close ();
}

void test_snapshot_grows_past_initial_capacity ()
{
    zlink::context_t context;
    auto client = make_router (context, "route-client");
    std::vector<std::unique_ptr<zlink::router_socket_t>> servers;
    for (int index = 0; index < 12; ++index) {
        const auto name = "route-server-" + std::to_string (index);
        servers.push_back (make_router (context, name));
        connect_as (*client, name, bind_loopback (*servers.back ()));
    }
    for (int index = 0; index < 12; ++index)
        (void) wait_route (*client, nullptr, "route-server-" + std::to_string (index));
    REQUIRE (client->routes_snapshot ().size () == 12);
}

bool connect_ready_now (zlink::socket_t &socket_)
{
    auto item = zlink::poll_item_t::from_socket (socket_, zlink::poll_event_flag_t::pollconnect);
    REQUIRE (zlink::poll (&item, 1, std::chrono::milliseconds (0)) >= 0);
    return (static_cast<short> (item.revents) & ZLINK_POLLCONNECT) != 0;
}

zlink::connection_attempt_t wait_connection_state (
  zlink::socket_t &socket_, zlink::connection_attempt_state_t state_)
{
    const auto deadline = std::chrono::steady_clock::now () + wait_limit;
    auto rows = socket_.connection_attempts_snapshot ();
    REQUIRE (rows.size () == 1);
    if (rows[0].state == state_)
        return rows[0];
    for (;;) {
        const auto now = std::chrono::steady_clock::now ();
        REQUIRE (now < deadline);
        const auto timeout = std::chrono::ceil<std::chrono::milliseconds> (deadline - now);
        auto item = zlink::poll_item_t::from_socket (socket_, zlink::poll_event_flag_t::pollconnect);
        const int count = zlink::poll (&item, 1, timeout);
        REQUIRE (count >= 0);
        if (count > 0
            && (static_cast<short> (item.revents) & ZLINK_POLLCONNECT) != 0) {
            rows = socket_.connection_attempts_snapshot ();
            REQUIRE (rows.size () == 1);
            if (rows[0].state == state_)
                return rows[0];
        }
    }
}

void test_connection_snapshot_lifetime ()
{
    zlink::context_t context;
    zlink::dealer_socket_t client (context);
    client.options ().linger (std::chrono::milliseconds (0));
    REQUIRE (client.connection_attempts_snapshot ().empty ());
    bool rejected = false;
    try { client.connect ("invalid-endpoint"); }
    catch (const zlink::connect_error_t &) { rejected = true; }
    REQUIRE (rejected);
    REQUIRE (client.connection_attempts_snapshot ().empty ());
    const auto endpoint = zlink_cpp_contract::unique_inproc ("connection-\xed\x95\x9c\xea\xb8\x80");
    client.connect (endpoint);
    client.connect (endpoint);
    const auto rows = client.connection_attempts_snapshot ();
    REQUIRE (rows.size () == 2);
    REQUIRE (rows[0].endpoint == endpoint && rows[1].endpoint == endpoint);
    REQUIRE (rows[0].intent_id != 0 && rows[1].intent_id != 0);
    REQUIRE (rows[0].intent_id != rows[1].intent_id);
    REQUIRE (rows[0].revision != 0 && rows[1].revision != 0);
    REQUIRE (rows[0].last_failure_id == 0 && rows[1].last_failure_id == 0);
    client.disconnect (endpoint);
    REQUIRE (client.connection_attempts_snapshot ().empty ());
    client.close ();
    REQUIRE (rows[0].endpoint == endpoint && rows[1].endpoint == endpoint);
    std::puts ("connection lifetime: empty=1 rejected_connect=1 duplicate_intents=2 removal=1 owned_utf8_after_close=1");
}

void test_connection_failure_recovery ()
{
    zlink::context_t context;
    zlink::router_socket_t server (context);
    zlink::dealer_socket_t client (context);
    server.options ().linger (std::chrono::milliseconds (0));
    client.options ().linger (std::chrono::milliseconds (0));
    server.bind ("tcp://127.0.0.1:*");
    const auto endpoint = server.options ().last_endpoint ();
    server.unbind (endpoint);
    client.connect (endpoint);
    REQUIRE (connect_ready_now (client));
    auto failed = wait_connection_state (client, zlink::connection_attempt_state_t::attempt_failed);
    REQUIRE (failed.last_failure_id != 0);
    REQUIRE (failed.last_failure.kind == zlink::connection_failure_kind_t::refused);
    REQUIRE (failed.last_failure.stage == zlink::connection_failure_stage_t::transport);
    REQUIRE (failed.last_failure.normalized_errno == ECONNREFUSED);
    server.bind (endpoint);
    auto connected = wait_connection_state (client, zlink::connection_attempt_state_t::connected);
    REQUIRE (connected.intent_id == failed.intent_id);
    REQUIRE (connected.revision != failed.revision);
    REQUIRE (connected.last_failure_id != 0);
    // The last failure can advance during reconnect; Core must retain it on success.
    REQUIRE (connected.last_failure.kind == failed.last_failure.kind);
    REQUIRE (connected.last_failure.stage == failed.last_failure.stage);
    REQUIRE (connected.last_failure.normalized_errno == failed.last_failure.normalized_errno);
    REQUIRE (!connect_ready_now (client));
    client.disconnect (endpoint);
    REQUIRE (connect_ready_now (client));
    REQUIRE (client.connection_attempts_snapshot ().empty ());
    REQUIRE (!connect_ready_now (client));
    client.connect (endpoint);
    REQUIRE (client.connection_attempts_snapshot ()[0].intent_id != failed.intent_id);
    std::puts ("connection recovery: failed_query_is_data=1 connected_history=1 removal_readiness=1 new_intent=1");
}

} // namespace

int main ()
{
    test_connection_snapshot_lifetime ();
    test_connection_failure_recovery ();
    test_snapshot_pollroute_and_record_generation ();
    test_replaced_route_changes_generation ();
    test_snapshot_grows_past_initial_capacity ();
    return 0;
}
