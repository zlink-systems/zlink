/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/backend/raw_dealer_port.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/backend/raw_route_port.hpp"

#include <zlink.hpp>

#include <cassert>
#include <chrono>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace backend = zlink::framework::detail::backend;

static_assert (std::is_same_v<
               decltype (std::declval<backend::raw_route_port_t &> ().send (
                 std::declval<const backend::raw_bytes_t &> (),
                 std::declval<const backend::raw_message_t &> ())),
               zlink::framework::task_t<bool>>);
static_assert (std::is_same_v<
               decltype (std::declval<backend::raw_route_port_t &> ().send_result (
                 std::declval<const backend::raw_bytes_t &> (),
                 std::declval<const backend::raw_message_t &> ())),
               zlink::framework::task_t<zlink::submit_result_t>>);
static_assert (std::is_same_v<
               decltype (std::declval<backend::raw_route_port_t &> ().try_receive ()),
               std::optional<backend::raw_received_t>>);
static_assert (std::is_same_v<
               decltype (std::declval<backend::raw_route_port_t &> ().request (
                 std::declval<const backend::raw_bytes_t &> (),
                 std::declval<const backend::raw_message_t &> (),
                 std::chrono::milliseconds (1))),
               zlink::framework::task_t<backend::raw_request_completion_t>>);
static_assert (std::is_same_v<
               decltype (std::declval<backend::raw_route_port_t &> ().reply (
                 std::declval<const backend::raw_received_t &> (),
                 std::declval<const backend::raw_message_t &> ())),
               bool>);
static_assert (std::is_same_v<
               decltype (std::declval<backend::raw_dealer_port_t &> ().send (
                 std::declval<const backend::raw_message_t &> ())),
               zlink::framework::task_t<bool>>);
static_assert (std::is_same_v<
               decltype (std::declval<backend::raw_dealer_port_t &> ().send (
                 std::declval<const backend::raw_message_t &> (),
                 std::chrono::milliseconds (1))),
               zlink::framework::task_t<zlink::submit_result_t>>);
static_assert (std::is_same_v<
               decltype (std::declval<backend::raw_dealer_port_t &> ().request (
                 std::declval<const backend::raw_message_t &> (),
                 std::chrono::milliseconds (1))),
               zlink::framework::task_t<backend::raw_request_completion_t>>);

namespace
{
using namespace std::chrono_literals;

bool wait_for_monitor_event (zlink::socket_monitor_t &monitor,
                             zlink::monitor_event expected,
                             std::chrono::milliseconds timeout)
{
    zlink::poller_t poller;
    poller.add (monitor, zlink::poll_event_flag_t::pollin, 1);
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
          deadline - std::chrono::steady_clock::now ());
        zlink::poll_event_t ready;
        if (poller.wait (&ready, 1, remaining) != 1)
            continue;
        const auto event = monitor.recv (zlink::recv_flags_t::dontwait);
        if (event && event->event == expected)
            return true;
    }
    return false;
}

backend::raw_message_t request_parts ()
{
    return backend::raw_message_t{
      backend::raw_bytes_t{'r', 'e', 'q', 'u', 'e', 's', 't'}};
}

void verify_missing_rid_is_initial_not_connected_without_wait_token ()
{
    zlink::context_t context;
    zlink::router_socket_t router (context);
    router.options ().mandatory (true);
    backend::raw_route_port_t port (router);

    auto request = port.request (
      zlink::routing_id_t::from ("missing-route").to_bytes (),
      request_parts (), 1s);
    assert (request.await_ready ());
    const auto &settled = request.result ();
    assert (settled);
    assert (settled.value ().result
            == backend::raw_request_result_t::route_unavailable);
    assert (settled.value ().failure);
    assert (settled.value ().failure->phase
            == backend::raw_request_failure_phase_t::initial_admission);
    assert (settled.value ().failure->submit_result
            == zlink::submit_result_t::not_connected);
    assert (!settled.value ().failure->request_result);
    assert (settled.value ().failure->internal_errno == EHOSTUNREACH);
    // A token-bearing rejection would remain pending until a WRITABLE record.
    // Synchronous completion here pins the D-B85 ID/token-zero path.
    assert (port.poll (0ms) == zlink::poll_event_flag_t::none);
    port.close ();
}

void verify_disconnect_rid_ends_issued_wait_token_with_enoent ()
{
    zlink::context_t context;
    context.options ().auto_hwm_enabled (false);
    zlink::router_socket_t server (context);
    zlink::router_socket_t client (context);
    server.options ().linger (0ms);
    client.options ().linger (0ms);
    const auto server_rid = zlink::routing_id_t::from ("wait-terminal-server");
    const auto client_rid = zlink::routing_id_t::from ("wait-terminal-client");
    server.set_routing_id (server_rid);
    client.set_routing_id (client_rid);
    server.set_receive_flow_state (zlink::receive_flow_state_t::paused);
    client.options ().connect_routing_id (server_rid);
    auto server_monitor = server.monitor_open (
      zlink::monitor_event::connection_ready);
    auto client_monitor = client.monitor_open (
      zlink::monitor_event::connection_ready);
    const std::string endpoint =
      "inproc://framework-raw-route-wait-token-terminal";
    server.bind (endpoint);
    client.connect (endpoint);
    assert (wait_for_monitor_event (
      server_monitor, zlink::monitor_event::connection_ready, 2s));
    assert (wait_for_monitor_event (
      client_monitor, zlink::monitor_event::connection_ready, 2s));

    backend::raw_route_port_t port (client);
    auto request = port.request (
      server_rid.to_bytes (), request_parts (), 2s);
    assert (!request.await_ready ());

    client.disconnect_rid (server_rid);
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (!request.await_ready ()
           && std::chrono::steady_clock::now () < deadline) {
        (void) port.poll (10ms);
    }
    assert (request.await_ready ());
    const auto &settled = request.result ();
    assert (settled);
    assert (settled.value ().result == backend::raw_request_result_t::failed);
    assert (settled.value ().failure);
    assert (settled.value ().failure->phase
            == backend::raw_request_failure_phase_t::completion_terminal);
    assert (settled.value ().failure->submit_result
            == zlink::submit_result_t::not_found);
    assert (!settled.value ().failure->request_result);
    assert (settled.value ().failure->internal_errno == ENOENT);
    port.close ();
    client_monitor.close ();
    server_monitor.close ();
}
}

int main ()
{
    verify_missing_rid_is_initial_not_connected_without_wait_token ();
    verify_disconnect_rid_ends_issued_wait_token_with_enoent ();
    return 0;
}
