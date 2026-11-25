/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/mesh/route_mesh_connection_policy.hpp"
#include "metric_test_reader.hpp"
#include "mailbox_claim_fixture.hpp"
#include "test_completion_poller_driver.hpp"
#include "../support/m6b_cases.hpp"
#include "../support/m6b_task.hpp"
#include "../support/m6b_mesh_fixture.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace foundation = zlink::framework::runtime::foundation;
namespace mesh = zlink::framework::runtime::mesh;
namespace protocol = zlink::framework::runtime::protocol;
namespace host = zlink::framework::runtime::host;
using namespace std::chrono_literals;

namespace zlink::framework::test::m6b
{
zlink::framework::task_t<zlink::request_result_t>
await_native_request_terminal (zlink::async_result_t<std::vector<zlink::message_t>> pending)
{
    try {
        (void) co_await std::move (pending);
        co_return zlink::request_result_t::ok;
    }
    catch (const zlink::request_error_t &error) {
        co_return error.result ();
    }
}

void verify_mesh_node_role_is_available_before_local_descriptor_publish ()
{
    auto state = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::mesh_node_builder_state_t> ("discovery-role");
    state->object_role = zlink::framework::object_role_t::client;
    state->channels.emplace ("object-channel",
                             zlink::framework::detail::mesh_channel_registration_t{
                               .weight = 100, .role_selected = true, .server = false});

    zlink::framework::detail::mesh_node_runtime_t mesh_runtime (state);
    assert (mesh_runtime.object_role () == zlink::framework::object_role_t::client);
    assert (mesh_runtime.channel_weights ().empty ());

    zlink::framework::mesh_node_descriptor_t remote;
    remote.object_role = zlink::framework::object_role_t::client;
    assert (zlink::framework::runtime::mesh::route_mesh_connection_not_required (
      mesh_runtime.object_role (), !mesh_runtime.channel_weights ().empty (), remote.object_role,
      !remote.channel_weights.empty ()));
}

void verify_mesh_stop_drains_admitted_request_completion ()
{
    using namespace zlink::framework;

    auto source_state = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::mesh_node_builder_state_t> ("m6b-mesh");
    source_state->listen_endpoint = "tcp://127.0.0.1:0";
    source_state->routing_id = zlink::routing_id_t::from ("completion-drain-source");
    const auto core_context = std::make_shared<zlink::context_t> ();
    source_state->core_context = core_context;
    auto target_state = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::mesh_node_builder_state_t> ("m6b-mesh");
    target_state->listen_endpoint = "tcp://127.0.0.1:0";
    target_state->routing_id = zlink::routing_id_t::from ("completion-drain-target");
    target_state->core_context = core_context;

    detail::mesh_node_runtime_t source (source_state);
    detail::mesh_node_runtime_t target (target_state);
    target.start ().result ().value ();
    source.start ().result ().value ();
    const auto target_status = target.status ();
    source.connect_peer (target_status.routing_id (), target_status.local_endpoint (),
                         target_status.lifecycle_generation ());

    const auto connected_deadline = std::chrono::steady_clock::now () + 5s;
    const auto discard = [] (const host::ready_record_t &, const host::receive_record_t &,
                             std::vector<zlink::message_t>, auto) {};
    while ((!source.has_admitted_peer (target_status.routing_id (),
                                       target_status.lifecycle_generation ())
            || !target.has_admitted_peer (source.status ().routing_id (),
                                          source.status ().lifecycle_generation ()))
           && std::chrono::steady_clock::now () < connected_deadline) {
        (void) source.dispatch_ready (discard);
        (void) target.dispatch_ready (discard);
        std::this_thread::sleep_for (1ms);
    }
    assert (source.has_admitted_peer (target_status.routing_id (),
                                      target_status.lifecycle_generation ()));

    host::pending_operation_t operation;
    assert (source
              .request_to_node (target_status.routing_id (),
                                {zlink::message_t::from (std::string ("message-follow"))},
                                operation, 2s)
              .result ()
              .value ()
            == zlink::submit_result_t::ok);

    std::optional<host::reply_token_t> delayed_reply;
    const auto admitted_deadline = std::chrono::steady_clock::now () + 2s;
    while (!delayed_reply && std::chrono::steady_clock::now () < admitted_deadline) {
        // Await so the suspended coroutine cannot outlive the callback and its captures.
        (void) target
          .dispatch_ready ([&] (const host::ready_record_t &, const host::receive_record_t &record,
                                std::vector<zlink::message_t>, auto) {
              if (record.kind == host::record_kind_t::node_request)
                  delayed_reply = record.reply_token;
          })
          .result ();
        std::this_thread::sleep_for (1ms);
    }
    assert (delayed_reply);

    // The test owns the poller-driving reference until completion is
    // observed; stop() may release the mesh's reference before the async
    // waiter's future is published.
    const auto completion_owner = source.native_node ().shared_from_this ();
    auto completion = std::async (std::launch::async, [&] {
        return source.wait_for_completion (operation, 2s, target_status.routing_id ());
    });
    const auto waiter_deadline = std::chrono::steady_clock::now () + 1s;
    while ((source.active_completion_waiters () != 1 || source.pending_transport_operations () != 1)
           && std::chrono::steady_clock::now () < waiter_deadline) {
        std::this_thread::yield ();
    }
    assert (source.active_completion_waiters () == 1);
    assert (source.pending_transport_operations () == 1);

    auto stopped = std::async (std::launch::async, [&] { source.stop (); });
    assert (stopped.wait_for (20ms) == std::future_status::timeout);
    assert (host::reply (*delayed_reply, {zlink::message_t::from (std::string ("settled"))})
            == zlink::submit_result_t::ok);

    const auto completion_deadline = std::chrono::steady_clock::now () + 2s;
    while (completion.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < completion_deadline) {
        // PollCompletion transfers drain ownership to the existing poller
        // (binding async execution §4). E5 needs that wait, but no host
        // dispatch turn or ordinary receive. The retained owner remains safe
        // if stop closes its port concurrently with this wait.
        (void) completion_owner->transport ().wait_for_activity (
          std::max (0ms, std::chrono::ceil<std::chrono::milliseconds> (
                           completion_deadline - std::chrono::steady_clock::now ())),
          false);
    }
    assert (completion.wait_for (0ms) == std::future_status::ready);
    const auto settled = completion.get ();
    assert (settled);
    assert (settled.value ().record.terminal_result
            == static_cast<int> (zlink::request_result_t::ok));
    assert (settled.value ().parts.size () == 1);
    assert (settled.value ().parts.front ().to_string () == "settled");
    stopped.get ();
    assert (!completion_owner->transport ().started ());
    target.stop ();
}
void verify_node_request_requires_remote_admission ()
{
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("request-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("request-target"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    target.expect_peer (source_descriptor);
    assert (source.connect_peer (target.endpoint (), target_descriptor));

    using request_result_t = std::pair<foundation::operation_terminal_t, std::vector<std::uint8_t>>;
    std::promise<request_result_t> promise;
    auto future = promise.get_future ();
    const auto deadline = mesh::service_liveness_registry_t::clock_t::now () + 5s;
    // Submit §5 keeps this request pending until remote service admission.
    auto submitted = source.request_to_node (
      target_descriptor.node_routing_id, {"DeferredRequest", "application/json", bytes ("request")},
      2s,
      [&promise] (foundation::operation_terminal_t terminal,
                  ::zlink::framework::result_t<zlink::message_t> completion_payload) {
          auto native_payload = std::move (completion_payload.value ());
          promise.set_value ({terminal, native_payload.to_bytes ()});
      },
      4242);
    assert (!submitted.await_ready ());
    assert (target.mailbox ().pending_messages (mesh::service_mailbox_domain_t::application) == 0);
    assert (future.wait_for (0ms) != std::future_status::ready);

    while ((!ready_peer (source.topology (), target_descriptor.node_routing_id)
            || !ready_peer (target.topology (), source_descriptor.node_routing_id))
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) source.observe_routes ();
        (void) target.observe_routes ();
        const auto source_pump = source.pump_one (now).result ().value ();
        const auto target_pump = target.pump_one (now).result ().value ();
        assert (source_pump != mesh::raw_mesh_pump_result_t::protocol_error);
        assert (target_pump != mesh::raw_mesh_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (source.topology ().peer (target_descriptor.node_routing_id));
    assert (target.topology ().peer (source_descriptor.node_routing_id));

    assert (submitted.result ().value ());

    zlink::framework::test::mailbox_claim_t claim;
    while (!claim && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) source.observe_routes ();
        (void) target.observe_routes ();
        const auto source_pump = source.pump_one (now).result ().value ();
        const auto target_pump = target.pump_one (now).result ().value ();
        assert (source_pump != mesh::raw_mesh_pump_result_t::protocol_error);
        assert (target_pump != mesh::raw_mesh_pump_result_t::protocol_error);
        claim = zlink::framework::test::claim_mailbox (
          target.mailbox (), mesh::service_mailbox_domain_t::application, 1, 1024);
        if (!claim)
            std::this_thread::sleep_for (1ms);
    }
    assert (claim && claim->records.size () == 1);
    const auto &request = (*claim->records.front ());
    assert (protocol::decode_header (protocol::message_bytes (request.parts ().front ())).kind
            == protocol::command::nodeRequest);
    assert (request.correlation && *request.correlation == 4242);
    assert (protocol::decode_application_payload (protocol::message_bytes (request.parts ().at (1)))
              .payload_bytes ()
            == bytes ("request"));
    assert (target.reply (request, {"DeferredReply", "application/json", bytes ("reply")}));
    assert (target.mailbox ().release (*claim));

    while (future.wait_for (0ms) != std::future_status::ready
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        const auto pumped = source.pump_one (now).result ().value ();
        assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (future.wait_for (0ms) == std::future_status::ready);
    const auto result = future.get ();
    assert (result.first == foundation::operation_terminal_t::completed);
    assert (protocol::decode_application_payload (result.second).payload_bytes ()
            == bytes ("reply"));
    source.close ();
    target.close ();
}

void verify_unadmitted_request_is_dropped_without_framework_queue ()
{
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("overflow-target"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    target.start ();

    zlink::context_t context;
    zlink::dealer_socket_t source (context);
    zlink::framework::test::completion_poller_driver_t completion_owner (source);
    source.set_routing_id (zlink::routing_id_t::from ("overflow-source"));
    source.connect (target.endpoint ());

    const auto payload = protocol::encode_application_payload (
      {"DeferredRequest", "application/json", bytes ("request")});
    auto header = zlink::message_t::from (protocol::encode_node_request_header (1));
    auto body = zlink::message_t::from (payload);
    auto request =
      std::move (source.request ().message (header).message (body)).timeout (5s).async ().reply;

    mesh::raw_mesh_pump_result_t pumped = mesh::raw_mesh_pump_result_t::no_data;
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (pumped == mesh::raw_mesh_pump_result_t::no_data
           && std::chrono::steady_clock::now () < deadline) {
        (void) target.observe_routes ();
        pumped =
          target.pump_one (mesh::service_liveness_registry_t::clock_t::now ()).result ().value ();
    }
    assert (pumped == mesh::raw_mesh_pump_result_t::infrastructure);
    assert (target.mailbox ().pending_messages (mesh::service_mailbox_domain_t::application) == 0);

    zlink::received_t hello;
    assert (source.recv (hello) == 0);
    assert (hello.parts ().size () == 1);
    assert (protocol::decode_header (hello.parts ().front ().to_bytes ()).kind
            == protocol::command::hello);

    // MeshNode §7.1: admission rejection is silent; only the original native deadline ends it.
    assert (await_native_request_terminal (std::move (request)).result ().value ()
            == zlink::request_result_t::timed_out);
    target.close ();
}

void verify_queued_owner_accepts_request_without_blocking_other_owner ()
{
    auto target_descriptor = descriptor ("owner-capacity-target");
    target_descriptor.channels.push_back ({"independent-channel", 100});
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("owner-capacity-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    metric_test::provider_t metric_provider;
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = target_descriptor,
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
      .metric_provider = metric_provider.api ()});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto published_target = target.topology ().local_descriptor ();
    const auto deadline = mesh::service_liveness_registry_t::clock_t::now () + 5s;
    assert (source.connect_peer (target.endpoint (), published_target));
    while ((!ready_peer (source.topology (), published_target.node_routing_id)
            || !ready_peer (target.topology (), source_descriptor.node_routing_id))
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) source.observe_routes ();
        (void) target.observe_routes ();
        assert (source.pump_one (now).result ().value ()
                != mesh::raw_mesh_pump_result_t::protocol_error);
        assert (target.pump_one (now).result ().value ()
                != mesh::raw_mesh_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (source.topology ().peer (published_target.node_routing_id));
    assert (target.topology ().peer (source_descriptor.node_routing_id));

    const protocol::application_payload_t payload{"OwnerCapacity", "application/json",
                                                  bytes ("request")};
    assert (source.send_to_node (published_target.node_routing_id, payload).result ().value ());
    mesh::raw_mesh_pump_result_t first = mesh::raw_mesh_pump_result_t::no_data;
    while (first != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        first =
          target.pump_one (mesh::service_liveness_registry_t::clock_t::now ()).result ().value ();
        assert (first != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (first == mesh::raw_mesh_pump_result_t::application);

    std::promise<std::pair<foundation::operation_terminal_t, std::vector<std::uint8_t>>>
      accepted_promise;
    auto accepted = accepted_promise.get_future ();
    assert (
      source
        .request_to_node (
          published_target.node_routing_id, payload, 5s,
          [&accepted_promise] (foundation::operation_terminal_t terminal,
                               ::zlink::framework::result_t<zlink::message_t> completion_payload) {
              auto failure = std::move (completion_payload.value ());
              accepted_promise.set_value ({terminal, failure.to_bytes ()});
          })
        .result ()
        .value ());
    mesh::raw_mesh_pump_result_t queued = mesh::raw_mesh_pump_result_t::no_data;
    while (queued != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        queued =
          target.pump_one (mesh::service_liveness_registry_t::clock_t::now ()).result ().value ();
        assert (queued != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (queued == mesh::raw_mesh_pump_result_t::application);
    std::size_t queued_records = 0, queued_requests = 0;
    for (unsigned i = 0; i != 2; ++i) {
        auto queued_claim = zlink::framework::test::claim_mailbox (
          target.mailbox (), mesh::service_mailbox_domain_t::application, 1, 4096);
        assert (queued_claim && queued_claim->records.size () == 1);
        ++queued_records;
        auto &record = *queued_claim->records.front ();
        if (record.reply_token ()) {
            ++queued_requests;
            assert (
              target.reply (record, {"OwnerCapacityReply", "application/json", bytes ("reply")}));
        }
        assert (target.mailbox ().release (*queued_claim));
    }
    assert (queued_records == 2 && queued_requests == 1);
    while (accepted.wait_for (0ms) != std::future_status::ready
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        assert (
          source.pump_one (mesh::service_liveness_registry_t::clock_t::now ()).result ().value ()
          != mesh::raw_mesh_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (accepted.wait_for (0ms) == std::future_status::ready);
    auto [accepted_terminal, accepted_payload] = accepted.get ();
    assert (accepted_terminal == foundation::operation_terminal_t::completed);
    assert (protocol::decode_application_payload (accepted_payload).payload_bytes ()
            == bytes ("reply"));

    // Queued requests remain observable as ordinary deliveries, never drops.
    for (auto fields : metric_provider.collect_fields ()) {
        if (fields["name"] == "zlink.mesh_node.messages.dropped") {
            assert (std::stod (fields["value"]) == 0);
        }
    }

    std::promise<foundation::operation_terminal_t> independent_promise;
    auto independent = independent_promise.get_future ();
    assert (source
              .request_to_channel (
                "independent-channel", payload, 5s,
                [&independent_promise] (foundation::operation_terminal_t terminal,
                                        ::zlink::framework::result_t<zlink::message_t>) {
                    independent_promise.set_value (terminal);
                })
              .result ()
              .value ());
    mesh::raw_mesh_pump_result_t independent_pump = mesh::raw_mesh_pump_result_t::no_data;
    while (independent_pump != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        independent_pump =
          target.pump_one (mesh::service_liveness_registry_t::clock_t::now ()).result ().value ();
        assert (independent_pump != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (independent_pump == mesh::raw_mesh_pump_result_t::application);
    auto claim = zlink::framework::test::claim_mailbox_owner (
      target.mailbox (), mesh::service_mailbox_domain_t::application, "channel:independent-channel",
      1, 1024);
    assert (claim && claim->records.size () == 1);
    assert (target.reply ((*claim->records.front ()),
                          {"OwnerCapacityReply", "application/json", bytes ("reply")}));
    assert (target.mailbox ().release (*claim));
    while (independent.wait_for (0ms) != std::future_status::ready
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        assert (
          source.pump_one (mesh::service_liveness_registry_t::clock_t::now ()).result ().value ()
          != mesh::raw_mesh_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (independent.wait_for (0ms) == std::future_status::ready);
    assert (independent.get () == foundation::operation_terminal_t::completed);
    source.close ();
    target.close ();
}
void verify_local_control_admission ()
{
    mesh::raw_mesh_node_owner_t node (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("local-control"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    node.start ();
    const auto local = node.topology ().local_descriptor ();
    const protocol::relocation_coordinator_fence_t coordinator{
      "local-coordinator", 1, local.node_routing_id, local.lifecycle_generation, "v1"};
    protocol::session_relocation_route_t route{
      {11, 12},
      coordinator,
      protocol::relocation_role_t::source,
      {"actor", 1},
      local.node_routing_id,
      local.lifecycle_generation,
      "session-owner",
      1,
      {'s'},
      1,
      {protocol::session_relocation_route_action_t::abort, 0, 0, {}, 0, 1, 0}};
    assert (await_task (node.send_session_relocation_route (local.node_routing_id, route)));
    assert (node.wait_for_activity (-1ms, false));
    auto claim =
      node.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 8, 65536);
    assert (claim && claim->records.size () == 1);
    const auto &record = *claim->records.front ();
    assert (record.source_routing_id == local.node_routing_id);
    assert (record.source_node_generation == local.lifecycle_generation);
    assert (
      protocol::decode_session_relocation_route (protocol::message_bytes (record.parts ().front ()))
      == route);
    assert (node.mailbox ().release (*claim));
    auto invalid_route = route;
    ++invalid_route.coordinator.node_generation;
    assert (
      !await_task (node.send_session_relocation_route (local.node_routing_id, invalid_route)));
    invalid_route = route;
    invalid_route.coordinator.node_routing_id = bytes ("forged-coordinator");
    assert (
      !await_task (node.send_session_relocation_route (local.node_routing_id, invalid_route)));
    invalid_route = route;
    ++invalid_route.session_owner_node_generation;
    assert (
      !await_task (node.send_session_relocation_route (local.node_routing_id, invalid_route)));
    protocol::reply_relay_ack_t ack{
      route.relocation,
      coordinator,
      {13, 14},
      15,
      {"request-owner", 1, local.node_routing_id, local.lifecycle_generation},
      protocol::reply_relay_ack_status_t::terminal_received};
    auto invalid_ack = ack;
    ++invalid_ack.request_source.node_generation;
    assert (!await_task (node.send_reply_relay_ack (local.node_routing_id, invalid_ack)));
    invalid_ack = ack;
    invalid_ack.request_source.node_routing_id = bytes ("forged-source");
    assert (!await_task (node.send_reply_relay_ack (local.node_routing_id, invalid_ack)));
    assert (!node.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 8, 65536));
    assert (await_task (node.send_reply_relay_ack (local.node_routing_id, ack)));
    claim = node.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 8, 65536);
    assert (claim && claim->records.size () == 1);
    assert (protocol::decode_reply_relay_ack (
              protocol::message_bytes (claim->records.front ()->parts ().front ()))
            == ack);
    assert (node.mailbox ().release (*claim));
    node.close ();
}

}
