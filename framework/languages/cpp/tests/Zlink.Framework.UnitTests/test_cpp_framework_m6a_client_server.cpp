/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6a_runtime_tests.hpp"
#include "mailbox_claim_fixture.hpp"
#include "../support/m6a_mesh_fixture.hpp"


#include "../support/runtime_failure_fixture.hpp"


#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include <limits>
#include <zlink/framework/contracts/dispatch/request_task.hpp>
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/mesh/user_spot_terminal_mapping.hpp"
#include "runtime/locations/service_descriptor_registry.hpp"
#include "runtime/fanout/raw_fanout_owner.hpp"
#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>
#include <zlink/socket/api.h>
#include "runtime/client_server/weighted_selector.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <service_wire_pilot_codec.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <future>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace zlink::framework::test::m6a
{
namespace mesh = zlink::framework::runtime::mesh;
namespace locations = zlink::framework::runtime::locations;
namespace fanout = zlink::framework::runtime::fanout;
namespace client_server = zlink::framework::runtime::client_server;
namespace foundation = zlink::framework::runtime::foundation;
namespace protocol = zlink::framework::runtime::protocol;
namespace runtime = zlink::framework::runtime;
using namespace std::chrono_literals;

void verify_client_server_stale_admission_reply_is_discarded ()
{
    /* Spec 51 §4 physical-connection replacement: an admission reply that
     * settles after its physical pair terminated must not admit the client
     * — the parked completion is fenced by the connection generation it
     * was requested on and discarded on mismatch. */
    protocol::client_server_server_admission_t server_descriptor{
      "client-server-stale",
      bytes ("server-s"),
      41,
      1,
      100,
      mesh::service_node_state_t::serving,
      "security-s",
      16u * 1024u * 1024u,
      "tcp://127.0.0.1:0"};
    auto server = std::make_unique<client_server::raw_client_server_server_t> (
      zlink::framework::test::runtime_failure_options (
        client_server::raw_client_server_server_options_t{{server_descriptor}}));
    server->start ();
    auto expected_server = server->descriptor ();
    auto manual_server = expected_server;
    manual_server.server_routing_id.clear ();
    manual_server.lifecycle_generation = 0;
    manual_server.descriptor_revision = 0;
    client_server::raw_client_server_client_t client (
      zlink::framework::test::runtime_failure_options (
        client_server::raw_client_server_client_options_t{
          bytes ("client-s"),
          {expected_server.channel_name, "security-s", 16u * 1024u * 1024u},
          std::move (manual_server)}));
    client.start ();

    /* Fire the hello request and let the server answer it, without ever
     * pumping the client — the admit completion parks unapplied. */
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    bool served_hello = false;
    while (!served_hello && std::chrono::steady_clock::now () < deadline) {
        (void) server->drain_monitor_events (std::chrono::steady_clock::now ());
        (void) client.drain_monitor_events (std::chrono::steady_clock::now ()).result ().value ();
        const auto pump = server->pump_one (std::chrono::steady_clock::now ()).result ().value ();
        served_hello = pump == client_server::client_server_pump_result_t::infrastructure;
        if (!served_hello)
            std::this_thread::sleep_for (1ms);
    }
    assert (served_hello);
    /* Give the admit reply time to settle into the parked state. */
    std::this_thread::sleep_for (300ms);

    /* Terminate the physical pair before the parked reply is applied, and
     * bring up a replacement server on the same endpoint. The client
     * reconnects (same connection identity bytes, new physical pair), but
     * the replacement server never answers the new hello — so the only
     * admission the client could apply is the stale parked one. */
    const auto endpoint = expected_server.advertised_endpoint;
    server->close ();
    server.reset ();
    auto replacement_descriptor = server_descriptor;
    replacement_descriptor.advertised_endpoint = endpoint;
    auto replacement = std::make_unique<client_server::raw_client_server_server_t> (
      zlink::framework::test::runtime_failure_options (
        client_server::raw_client_server_server_options_t{{replacement_descriptor}}));
    replacement->start ();
    /* Drain the client so it observes the disconnect and the reconnect to
     * the replacement (new physical pair; fires a fresh hello the
     * replacement deliberately leaves unanswered). */
    const auto drain_until = std::chrono::steady_clock::now () + 2s;
    while (std::chrono::steady_clock::now () < drain_until) {
        (void) client.drain_monitor_events (std::chrono::steady_clock::now ()).result ().value ();
        std::this_thread::sleep_for (10ms);
    }

    /* Applying the parked admit from the previous pair must discard it as
     * stale — the client stays unadmitted until the replacement answers. */
    const auto assert_deadline = std::chrono::steady_clock::now () + 800ms;
    while (std::chrono::steady_clock::now () < assert_deadline) {
        (void) client.pump_one (std::chrono::steady_clock::now ()).result ().value ();
        assert (!client.ready ());
        std::this_thread::sleep_for (10ms);
    }
    assert (!client.ready ());
    replacement->close ();
}

void verify_client_server_close_drains_parked_control_reply ()
{
    zlink::framework::runtime::offload_executor_t infrastructure (1);
    protocol::client_server_server_admission_t descriptor{"client-server-close-drain",
                                                          bytes ("server-close-drain"),
                                                          41,
                                                          1,
                                                          100,
                                                          mesh::service_node_state_t::serving,
                                                          "security-close-drain",
                                                          1024 * 1024,
                                                          "tcp://127.0.0.1:0"};
    client_server::raw_client_server_server_t server (
      zlink::framework::test::runtime_failure_options (
        client_server::raw_client_server_server_options_t{descriptor}),
      {}, &infrastructure);
    server.start ();
    const auto expected = server.descriptor ();
    std::promise<void> entered;
    auto callback_entered = entered.get_future ();
    std::promise<void> release;
    const auto released = release.get_future ().share ();
    std::atomic<runtime::state_lane_t *> deadline_owner{nullptr};
    zlink::poller_t client_poller;
    auto options = zlink::framework::test::runtime_failure_options (
      client_server::raw_client_server_client_options_t{
        bytes ("client-close-drain"),
        {expected.channel_name, expected.security_identity, expected.effective_max_message_bytes},
        expected});
    options.transport_poller = &client_poller;
    options.control_reply_parked = [&] {
        auto *current = runtime::state_lane_t::current ();
        assert (current);
        runtime::state_lane_t *unobserved = nullptr;
        // The monitor drain registers the request deadline before the server is pumped.
        // Deadline wakes use that owner; parked replies use the separate reply lane.
        if (deadline_owner.compare_exchange_strong (unobserved, current) || unobserved == current)
            return;
        entered.set_value ();
        released.wait ();
    };
    client_server::raw_client_server_client_t client (std::move (options));
    client.start ();
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (callback_entered.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < deadline) {
        (void) client.drain_monitor_events (std::chrono::steady_clock::now ()).result ().value ();
        (void) server.drain_monitor_events (std::chrono::steady_clock::now ());
        (void) server.pump_one (std::chrono::steady_clock::now ()).result ().value ();
        // Drive the binding completion owner without applying the parked reply.
        zlink::poll_event_t event;
        (void) client_poller.wait (&event, 1, 0ms);
        std::this_thread::sleep_for (1ms);
    }
    const bool parked = callback_entered.wait_for (0ms) == std::future_status::ready;
    if (!parked) {
        release.set_value ();
        assert (parked);
        return;
    }
    auto closing = client.close_task ();
    const bool waited_for_reply_lane = !closing.await_ready ();
    // Release before asserting so a regression cannot strand the fixture's callback.
    release.set_value ();
    closing.result ().value ();
    assert (waited_for_reply_lane);
    client.close_task ().result ().value ();
    server.close ();
}

void verify_client_server_plain_hello_is_rejected ()
{
    /* Spec 51 §4 (ClientServer direction): admission rides the Core
     * request/reply envelope — the server answers hello only on the reply
     * of the client's request. A plain routed hello cannot be answered
     * and must end as a protocol error instead of a raw routed admit. */
    protocol::client_server_server_admission_t server_descriptor{
      "client-server-plain",
      bytes ("server-p"),
      41,
      1,
      100,
      mesh::service_node_state_t::preparing,
      "security-p",
      16u * 1024u * 1024u,
      "tcp://127.0.0.1:0"};
    client_server::raw_client_server_server_t server (
      zlink::framework::test::runtime_failure_options (
        client_server::raw_client_server_server_options_t{server_descriptor}));
    server.start ();

    auto context = std::make_shared<zlink::context_t> ();
    zlink::dealer_socket_t dealer (*context);
    dealer.set_routing_id (zlink::routing_id_t::from (bytes ("client-p")));
    dealer.connect (server.endpoint ());
    zlink::poller_t dealer_poller;
    zlink::framework::detail::backend::raw_dealer_port_t port (dealer, nullptr, &dealer_poller);
    const zlink::framework::detail::backend::raw_message_t hello_message{
      protocol::encode_client_server_client_admission (
        protocol::command::hello,
        {server_descriptor.channel_name, "security-p", 16u * 1024u * 1024u})};
    const auto send_deadline = std::chrono::steady_clock::now () + 5s;
    bool sent = false;
    auto send_task = port.send (hello_message);
    while (!send_task.await_ready () && std::chrono::steady_clock::now () < send_deadline) {
        /* The binding's DONTWAIT send keeps an unready-target wait token.
         * Its public poller owner must drain WRITABLE before the awaitable
         * can resubmit and settle. */
        zlink::poll_event_t event;
        (void) dealer_poller.wait (&event, 1, 10ms);
    }
    if (send_task.await_ready ())
        sent = send_task.result ().value ();
    assert (sent);

    const auto deadline = std::chrono::steady_clock::now () + 5s;
    auto result = client_server::client_server_pump_result_t::no_data;
    while (result == client_server::client_server_pump_result_t::no_data
           && std::chrono::steady_clock::now () < deadline) {
        (void) server.drain_monitor_events (std::chrono::steady_clock::now ());
        result = server.pump_one (std::chrono::steady_clock::now ()).result ().value ();
        if (result == client_server::client_server_pump_result_t::no_data)
            std::this_thread::sleep_for (1ms);
    }
    assert (result == client_server::client_server_pump_result_t::protocol_error);
    port.close ();
}

void verify_client_server_independent_raw_path ()
{
    protocol::client_server_server_admission_t server_descriptor{
      "client-server-alpha",
      bytes ("server-a"),
      41,
      1,
      100,
      mesh::service_node_state_t::preparing,
      "security-a",
      16u * 1024u * 1024u,
      "tcp://127.0.0.1:0"};
    client_server::raw_client_server_server_t server (
      zlink::framework::test::runtime_failure_options (
        client_server::raw_client_server_server_options_t{server_descriptor}));
    server.start ();
    auto expected_server = server.descriptor ();
    auto manual_server = expected_server;
    manual_server.server_routing_id.clear ();
    manual_server.lifecycle_generation = 0;
    manual_server.descriptor_revision = 0;
    client_server::raw_client_server_client_options_t client_options{
      bytes ("client-a"),
      {expected_server.channel_name, "security-a", 16u * 1024u * 1024u},
      std::move (manual_server)};
    client_options.runtime_failures =
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ();
    client_server::raw_client_server_client_t client (std::move (client_options));
    client.start ();
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (!client.ready () && std::chrono::steady_clock::now () < deadline) {
        const auto now = std::chrono::steady_clock::now ();
        (void) server.drain_monitor_events (now);
        (void) client.drain_monitor_events (now).result ().value ();
        const auto server_pump = server.pump_one (now).result ().value ();
        const auto client_pump = client.pump_one (now).result ().value ();
        assert (server_pump != client_server::client_server_pump_result_t::protocol_error);
        assert (client_pump != client_server::client_server_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (client.ready ());

    const auto liveness_base = std::chrono::steady_clock::now ();
    const auto first_probe = client.tick_liveness (liveness_base + 5s).result ().value ();
    assert (first_probe.probes.size () == 1);
    client_server::client_server_pump_result_t probe_pump =
      client_server::client_server_pump_result_t::no_data;
    while (probe_pump == client_server::client_server_pump_result_t::no_data
           && std::chrono::steady_clock::now () < deadline) {
        probe_pump = server.pump_one (std::chrono::steady_clock::now ()).result ().value ();
    }
    assert (probe_pump == client_server::client_server_pump_result_t::infrastructure);
    client_server::client_server_pump_result_t ack_pump =
      client_server::client_server_pump_result_t::no_data;
    while (ack_pump == client_server::client_server_pump_result_t::no_data
           && std::chrono::steady_clock::now () < deadline) {
        ack_pump = client.pump_one (std::chrono::steady_clock::now ()).result ().value ();
    }
    assert (ack_pump == client_server::client_server_pump_result_t::infrastructure);
    const auto next_probe = client.tick_liveness (liveness_base + 10s).result ().value ();
    assert (next_probe.probes.size () == 1);
    assert (next_probe.probes.front ().probe_id != first_probe.probes.front ().probe_id);

    assert (
      client.send ({"ClientServerSend", "application/json", bytes ("send")}).result ().value ()
      == zlink::submit_result_t::ok);
    client_server::client_server_pump_result_t send_pump =
      client_server::client_server_pump_result_t::no_data;
    while (send_pump != client_server::client_server_pump_result_t::application
           && std::chrono::steady_clock::now () < deadline) {
        send_pump = server.pump_one (std::chrono::steady_clock::now ()).result ().value ();
    }
    assert (send_pump == client_server::client_server_pump_result_t::application);
    auto send_claim = zlink::framework::test::claim_mailbox (
      server.mailbox (), mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (send_claim && send_claim->records.size () == 1);
    {
        const auto send_header = runtime::messaging::envelope_codec_t{}.decode_header (
          send_claim->records.front ()->parts ().front ().copy (), false);
        assert (send_header);
        assert (send_header.value ().channel_name == expected_server.channel_name);
        assert (send_header.value ().kind == runtime::messaging::message_kind_t::command);
        assert (send_header.value ().message_name == "ClientServerSend");
    }
    assert (server.mailbox ().release (*send_claim));

    /* request() reads its borrowed payload after the first suspension, so the
     * payload must outlive the returned task. */
    const protocol::application_payload_t request_payload{"ClientServerRequest", "application/json",
                                                          bytes ("request")};
    auto request_task = client.request (request_payload, 2s);
    client_server::client_server_pump_result_t request_pump =
      client_server::client_server_pump_result_t::no_data;
    while (request_pump != client_server::client_server_pump_result_t::application
           && std::chrono::steady_clock::now () < deadline) {
        request_pump = server.pump_one (std::chrono::steady_clock::now ()).result ().value ();
    }
    assert (request_pump == client_server::client_server_pump_result_t::application);
    auto request_claim = zlink::framework::test::claim_mailbox (
      server.mailbox (), mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (request_claim && request_claim->records.size () == 1);
    assert (request_claim->records.front ()->reply_token ());
    assert (server
              .reply ((*request_claim->records.front ()),
                      {"ClientServerReply", "application/json", bytes ("reply")})
              .result ()
              .value ());
    assert (server.mailbox ().release (*request_claim));
    while (!request_task.await_ready () && std::chrono::steady_clock::now () < deadline) {
        const auto pump = client.pump_one (std::chrono::steady_clock::now ()).result ().value ();
        assert (pump != client_server::client_server_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (request_task.await_ready ());
    const auto result = request_task.result ().value ();
    assert (result.terminal == foundation::operation_terminal_t::completed);
    assert (!result.error_code);
    assert (result.content_type == "application/json");
    const auto expected_reply = bytes ("reply");
    assert (result.payload.size () == expected_reply.size ());
    assert (std::memcmp (result.payload.data (), expected_reply.data (), expected_reply.size ())
            == 0);

    const protocol::application_payload_t rejected_payload{"RejectedRequest", "application/json",
                                                           bytes ("request")};
    auto rejected_task = client.request (rejected_payload, 2s);
    request_pump = client_server::client_server_pump_result_t::no_data;
    while (request_pump != client_server::client_server_pump_result_t::application
           && std::chrono::steady_clock::now () < deadline) {
        request_pump = server.pump_one (std::chrono::steady_clock::now ()).result ().value ();
    }
    assert (request_pump == client_server::client_server_pump_result_t::application);
    request_claim = zlink::framework::test::claim_mailbox (
      server.mailbox (), mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (request_claim && request_claim->records.size () == 1);
    assert (server
              .reply ((*request_claim->records.front ()),
                      zlink::framework::framework_exception_t (
                        zlink::framework::framework_error_kind_t::rejected,
                        "ClientServer request was rejected."))
              .result ()
              .value ());
    assert (server.mailbox ().release (*request_claim));
    while (!rejected_task.await_ready () && std::chrono::steady_clock::now () < deadline) {
        const auto pump = client.pump_one (std::chrono::steady_clock::now ()).result ().value ();
        assert (pump != client_server::client_server_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (rejected_task.await_ready ());
    const auto rejected = rejected_task.result ().value ();
    assert (rejected.terminal == foundation::operation_terminal_t::completed);
    assert (rejected.error_code && *rejected.error_code == "rejected");

    // The advertised ClientServer limit must remain usable for a frame that
    // is larger than the core automatic HWM default.
    const auto large_payload =
      std::vector<std::uint8_t> (1024u * 1024u, static_cast<std::uint8_t> ('p'));
    const protocol::application_payload_t large_request{"LargePayloadRequest", "application/json",
                                                        large_payload};
    auto large_task = client.request (large_request, 3s);
    const auto large_deadline = std::chrono::steady_clock::now () + 5s;
    auto large_claim = zlink::framework::test::claim_mailbox (
      server.mailbox (), mesh::service_mailbox_domain_t::application, 1, 2u * 1024u * 1024u);
    while (!large_claim && std::chrono::steady_clock::now () < large_deadline) {
        const auto pump = server.pump_one (std::chrono::steady_clock::now ()).result ().value ();
        assert (pump != client_server::client_server_pump_result_t::protocol_error);
        large_claim = zlink::framework::test::claim_mailbox (
          server.mailbox (), mesh::service_mailbox_domain_t::application, 1, 2u * 1024u * 1024u);
        if (!large_claim)
            std::this_thread::sleep_for (1ms);
    }
    assert (large_claim && large_claim->records.size () == 1);
    const auto large_reply = bytes ("large payload accepted");
    assert (server
              .reply ((*large_claim->records.front ()),
                      {"LargePayloadReply", "application/json", large_reply})
              .result ()
              .value ());
    assert (server.mailbox ().release (*large_claim));
    while (!large_task.await_ready () && std::chrono::steady_clock::now () < large_deadline) {
        const auto pump = client.pump_one (std::chrono::steady_clock::now ()).result ().value ();
        assert (pump != client_server::client_server_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (large_task.await_ready ());
    const auto large_result = large_task.result ().value ();
    assert (large_result.terminal == foundation::operation_terminal_t::completed);
    assert (!large_result.error_code);
    assert (large_result.payload.size () == large_reply.size ());
    assert (std::memcmp (large_result.payload.data (), large_reply.data (), large_reply.size ())
            == 0);
}

void verify_client_server_admits_before_monitor_drain ()
{
    protocol::client_server_server_admission_t server_descriptor{
      "client-server-monitor-order",
      bytes ("server-monitor-order"),
      41,
      1,
      100,
      mesh::service_node_state_t::preparing,
      "security-monitor-order",
      1024 * 1024,
      "tcp://127.0.0.1:0"};
    client_server::raw_client_server_server_t server (
      zlink::framework::test::runtime_failure_options (
        client_server::raw_client_server_server_options_t{server_descriptor}));
    server.start ();
    const auto expected_server = server.descriptor ();

    std::vector<std::unique_ptr<client_server::raw_client_server_client_t>> clients;
    for (const auto &client_id : {"client-monitor-a", "client-monitor-b"}) {
        clients.push_back (std::make_unique<client_server::raw_client_server_client_t> (
          zlink::framework::test::runtime_failure_options (
            client_server::raw_client_server_client_options_t{
              bytes (client_id),
              {expected_server.channel_name, expected_server.security_identity,
               expected_server.effective_max_message_bytes},
              expected_server})));
        clients.back ()->start ();
    }

    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (std::chrono::steady_clock::now () < deadline) {
        const auto now = std::chrono::steady_clock::now ();
        for (auto &client : clients) {
            (void) client->drain_monitor_events (now).result ().value ();
            const auto client_pump = client->pump_one (now).result ().value ();
            assert (client_pump != client_server::client_server_pump_result_t::protocol_error);
        }

        // The server monitor queue is deliberately not drained here. A
        // received hello already identifies the route that must receive the
        // admission response.
        const auto server_pump = server.pump_one (now).result ().value ();
        assert (server_pump != client_server::client_server_pump_result_t::protocol_error);

        bool all_ready = true;
        for (auto &client : clients) {
            (void) client->pump_one (now).result ().value ();
            all_ready = all_ready && client->ready ();
        }
        if (all_ready)
            break;
        std::this_thread::sleep_for (1ms);
    }

    for (const auto &client : clients)
        assert (client->ready ());
    for (auto &client : clients)
        client->close ();
    server.close ();
}

void verify_client_server_weighted_selection ()
{
    client_server::smooth_weighted_selector_t selector;
    const std::vector<client_server::weighted_candidate_t> weighted{
      {"api-a", 300}, {"api-b", 100}, {"disabled", 0}};
    std::map<std::string, std::size_t> selected;
    for (std::size_t index = 0; index < 400; ++index) {
        const auto key = selector.select (weighted);
        assert (key);
        ++selected[*key];
        assert (selector.state_size () == 2);
        assert (selector.maximum_absolute_credit () <= 400);
    }
    assert (selected["api-a"] == 300);
    assert (selected["api-b"] == 100);
    assert (!selected.contains ("disabled"));

    const std::vector<client_server::weighted_candidate_t> after_api_b_enters_draining{
      {"api-a", 300}};
    for (std::size_t index = 0; index < 32; ++index) {
        const auto key = selector.select (after_api_b_enters_draining);
        assert (key && *key == "api-a");
        assert (selector.state_size () == 1);
        assert (selector.maximum_absolute_credit () <= 300);
    }

    const std::vector<client_server::weighted_candidate_t> none{{"disabled", 0}};
    assert (!selector.select (none));
    assert (selector.state_size () == 0);

    client_server::smooth_weighted_selector_t rid_tiebreak;
    const std::vector<client_server::weighted_candidate_t> tied{{"connection-z", 100, "rid-a"},
                                                                {"connection-a", 100, "rid-b"}};
    const auto tie_selected = rid_tiebreak.select (tied);
    assert (tie_selected && *tie_selected == "connection-z");

    zlink::framework::runtime::state_lane_t topology_lane;

    mesh::service_topology_registry_t topology (topology_lane, descriptor ("route-local"));
    auto route_a = descriptor ("route-a");
    auto route_b = descriptor ("route-b");
    route_a.state = mesh::service_node_state_t::serving;
    route_b.state = mesh::service_node_state_t::serving;
    assert (topology.admit (route_a, bytes ("route-connection-a"))
            == mesh::peer_admission_result_t::admitted);
    assert (topology.admit (route_b, bytes ("route-connection-b"))
            == mesh::peer_admission_result_t::admitted);
    const std::vector<std::vector<std::uint8_t>> expected_route_ids{
      bytes ("route-a"), bytes ("route-b"), bytes ("route-a"), bytes ("route-b")};
    for (const auto &expected : expected_route_ids) {
        const auto selected_route = topology.select ("alpha");
        assert (selected_route && selected_route.value () == expected);
    }
    assert (!topology.select ("unknown-before-change"));
    const auto before_change = topology.select ("alpha");
    assert (before_change && before_change.value () == bytes ("route-a"));
    auto route_c = descriptor ("route-c");
    route_c.state = mesh::service_node_state_t::serving;
    assert (topology.admit (route_c, bytes ("route-connection-c"))
            == mesh::peer_admission_result_t::admitted);
    const auto retained_after_change = topology.select ("alpha");
    assert (retained_after_change && retained_after_change.value () == bytes ("route-b"));
    assert (!topology.select ("unknown-after-change"));

    auto weighted_route_a = descriptor ("weighted-route-a");
    auto weighted_route_b = descriptor ("weighted-route-b");
    weighted_route_a.channels.front ().weight = 300;
    weighted_route_b.channels.front ().weight = 100;
    weighted_route_a.state = mesh::service_node_state_t::serving;
    weighted_route_b.state = mesh::service_node_state_t::serving;
    zlink::framework::runtime::state_lane_t weighted_topology_lane;
    mesh::service_topology_registry_t weighted_topology (weighted_topology_lane,
                                                         descriptor ("weighted-route-local"));
    assert (weighted_topology.admit (weighted_route_a, bytes ("weighted-route-connection-a"))
            == mesh::peer_admission_result_t::admitted);
    assert (weighted_topology.admit (weighted_route_b, bytes ("weighted-route-connection-b"))
            == mesh::peer_admission_result_t::admitted);
    std::map<std::vector<std::uint8_t>, std::size_t> weighted_selected;
    for (std::size_t index = 0; index < 400; ++index) {
        const auto selected_route = weighted_topology.select ("alpha");
        assert (selected_route);
        ++weighted_selected[selected_route.value ()];
    }
    assert (weighted_selected[bytes ("weighted-route-a")] == 300);
    assert (weighted_selected[bytes ("weighted-route-b")] == 100);

    weighted_route_b.descriptor_revision = 2;
    weighted_route_b.state = mesh::service_node_state_t::retiring;
    assert (weighted_topology.admit (weighted_route_b, bytes ("weighted-route-connection-b"),
                                     mesh::service_liveness_registry_t::clock_t::now (),
                                     protocol::command::update)
            == mesh::peer_admission_result_t::admitted);
    for (std::size_t index = 0; index < 32; ++index) {
        const auto selected_route = weighted_topology.select ("alpha");
        assert (selected_route && selected_route.value () == bytes ("weighted-route-a"));
    }

    zlink::framework::runtime::state_lane_t cycle_topology_lane;

    mesh::service_topology_registry_t cycle_topology (cycle_topology_lane,
                                                      descriptor ("cycle-route-local"));
    auto cycle_route_a = descriptor ("cycle-route-a");
    auto cycle_route_b = descriptor ("cycle-route-b");
    cycle_route_a.channels.front ().weight = 5;
    cycle_route_b.channels.front ().weight = 3;
    cycle_route_a.state = mesh::service_node_state_t::serving;
    cycle_route_b.state = mesh::service_node_state_t::serving;
    assert (cycle_topology.admit (cycle_route_a, bytes ("cycle-route-connection-a"))
            == mesh::peer_admission_result_t::admitted);
    assert (cycle_topology.admit (cycle_route_b, bytes ("cycle-route-connection-b"))
            == mesh::peer_admission_result_t::admitted);
    client_server::smooth_weighted_selector_t cycle_reference;
    std::vector<client_server::weighted_candidate_t> cycle_candidates{{"cycle-route-a", 5},
                                                                      {"cycle-route-b", 3}};
    for (std::size_t index = 0; index < 257; ++index) {
        const auto expected = cycle_reference.select (cycle_candidates);
        const auto actual = cycle_topology.select ("alpha");
        assert (expected && actual && actual.value () == bytes (*expected));
    }

    auto cycle_route_c = descriptor ("cycle-route-c");
    cycle_route_c.channels.front ().weight = 2;
    cycle_route_c.state = mesh::service_node_state_t::serving;
    assert (cycle_topology.admit (cycle_route_c, bytes ("cycle-route-connection-c"))
            == mesh::peer_admission_result_t::admitted);
    cycle_candidates.push_back ({"cycle-route-c", 2});
    for (std::size_t index = 0; index < 211; ++index) {
        const auto expected = cycle_reference.select (cycle_candidates);
        const auto actual = cycle_topology.select ("alpha");
        assert (expected && actual && actual.value () == bytes (*expected));
    }
}
}
