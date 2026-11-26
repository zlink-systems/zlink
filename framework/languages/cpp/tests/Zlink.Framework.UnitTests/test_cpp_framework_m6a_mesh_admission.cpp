/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6a_actor_join_request.hpp"
#include "../support/m6a_actor_create_request.hpp"
#include "../support/m6a_bound_session_fixture.hpp"
#include "m6a_runtime_tests.hpp"
#include "../support/m6a_mesh_fixture.hpp"

#include "mailbox_claim_fixture.hpp"

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

#include <service_wire_codec.hpp>
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

void verify_topology_snapshot_and_connection_fence ()
{
    zlink::framework::runtime::state_lane_t topology_lane;
    mesh::service_topology_registry_t topology (topology_lane, descriptor ("local"));
    auto peer = descriptor ("peer", "tcp://127.0.0.1:7001");
    peer.state = mesh::service_node_state_t::serving;
    const auto first_connection = bytes ("connection-a");
    assert (topology.admit (peer, first_connection) == mesh::peer_admission_result_t::admitted);
    assert (topology.select ("alpha").value () == bytes ("peer"));

    auto older = peer;
    older.descriptor_revision = 0;
    assert (topology.admit (older, bytes ("ignored"))
            == mesh::peer_admission_result_t::invalid_descriptor);

    const auto replacement_connection = bytes ("connection-b");
    assert (topology.admit (peer, replacement_connection)
            == mesh::peer_admission_result_t::admitted);
    assert (!topology.disconnect (bytes ("peer"), first_connection));
    assert (topology.peer (bytes ("peer"))->connection_id == replacement_connection);

    auto equal_revision_mutation = peer;
    equal_revision_mutation.state = mesh::service_node_state_t::retiring;
    assert (topology.admit (equal_revision_mutation, replacement_connection)
            == mesh::peer_admission_result_t::stale_descriptor);

    auto immutable_mutation = peer;
    immutable_mutation.descriptor_revision = 2;
    immutable_mutation.security_identity = "changed-security";
    assert (topology.admit (immutable_mutation, replacement_connection,
                            mesh::service_liveness_registry_t::clock_t::now (),
                            protocol::command::update)
            == mesh::peer_admission_result_t::stale_descriptor);

    peer.descriptor_revision = 2;
    peer.state = mesh::service_node_state_t::retiring;
    assert (topology.admit (peer, replacement_connection,
                            mesh::service_liveness_registry_t::clock_t::now (),
                            protocol::command::update)
            == mesh::peer_admission_result_t::admitted);
    assert (!topology.select ("alpha"));
}

// RouteMesh admission's capability is part of the wire descriptor contract.
// Keeping the default bound to the generated schema constant prevents a C++
// peer from sending a descriptor that a newer language rejects before its
// identity guard can run.
void verify_route_mesh_descriptor_uses_generated_capability ()
{
    const auto local = descriptor ("generated-capability-local");
    assert (local.protocol_capabilities.size () == 2);
    assert (local.protocol_capabilities.at (0) == "framework-service-v12");
    assert (local.protocol_capabilities.at (1) == protocol::required_capability);
    zlink::framework::runtime::state_lane_t topology_lane;
    mesh::service_topology_registry_t topology (topology_lane, local);
    (void) topology;

    auto stale = local;
    stale.protocol_capabilities = {"framework-service-v12"};
    bool rejected = false;
    try {
        zlink::framework::runtime::state_lane_t invalid_lane;
        mesh::service_topology_registry_t invalid (invalid_lane, std::move (stale));
        (void) invalid;
    }
    catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert (rejected);
}

void verify_admission_follows_latest_observed_route ()
{
    // The connection identity is the Core selected-route generation (Core
    // ROUTER §10.1). Only the owner's route observer decides which route is
    // current; the registry admits the route it is given and treats a repeat
    // of the same route and descriptor as a duplicate.
    auto lower = descriptor ("aa");
    auto higher = descriptor ("zz", "tcp://127.0.0.1:7003");
    higher.state = mesh::service_node_state_t::serving;
    zlink::framework::runtime::state_lane_t lower_topology_lane;
    mesh::service_topology_registry_t lower_topology (lower_topology_lane, lower);

    assert (lower_topology.admit (higher, bytes ("route-1"))
            == mesh::peer_admission_result_t::admitted);
    assert (lower_topology.admit (higher, bytes ("route-1"))
            == mesh::peer_admission_result_t::duplicate_connection);
    assert (lower_topology.admit (higher, bytes ("route-2"))
            == mesh::peer_admission_result_t::admitted);
    assert (lower_topology.peer (bytes ("zz"))->connection_id == bytes ("route-2"));
    assert (!lower_topology.disconnect (bytes ("zz"), bytes ("route-1")));
    assert (lower_topology.peer (bytes ("zz"))->connection_id == bytes ("route-2"));
    zlink::framework::runtime::state_lane_t liveness_lane;
    mesh::service_liveness_registry_t liveness (liveness_lane);
    liveness.admit (bytes ("zz"), bytes ("route-2"),
                    mesh::service_liveness_registry_t::clock_t::now ());
    assert (!liveness.disconnect (bytes ("zz"), bytes ("route-1")));
    assert (liveness.size () == 1);
    assert (lower_topology.disconnect (bytes ("zz"), bytes ("route-2")));
    assert (!lower_topology.peer (bytes ("zz")));
}

void verify_lifecycle_token_requires_current_discovery_expectation ()
{
    zlink::framework::runtime::state_lane_t topology_lane;
    mesh::service_topology_registry_t topology (topology_lane, descriptor ("local"));
    auto generation_99 = descriptor ("peer", "tcp://127.0.0.1:7099");
    generation_99.lifecycle_generation = 99;
    generation_99.state = mesh::service_node_state_t::serving;
    assert (topology.admit (generation_99, bytes ("generation-99"), generation_99)
            == mesh::peer_admission_result_t::admitted);

    auto generation_3 = generation_99;
    generation_3.lifecycle_generation = 3;
    generation_3.descriptor_revision = 1;
    assert (topology.admit (generation_3, bytes ("generation-3-without-expectation"))
            == mesh::peer_admission_result_t::stale_descriptor);
    assert (topology.peer (bytes ("peer"))->descriptor.lifecycle_generation == 99);

    auto wrong_expectation = generation_3;
    wrong_expectation.lifecycle_generation = 4;
    assert (topology.admit (generation_3, bytes ("generation-3-mismatch"), wrong_expectation)
            == mesh::peer_admission_result_t::stale_descriptor);
    wrong_expectation = generation_3;
    wrong_expectation.advertised_endpoint = "tcp://127.0.0.1:7100";
    assert (
      topology.admit (generation_3, bytes ("generation-3-endpoint-mismatch"), wrong_expectation)
      == mesh::peer_admission_result_t::stale_descriptor);
    wrong_expectation = generation_3;
    wrong_expectation.security_identity = "different-security";
    assert (
      topology.admit (generation_3, bytes ("generation-3-security-mismatch"), wrong_expectation)
      == mesh::peer_admission_result_t::stale_descriptor);

    assert (topology.admit (generation_3, bytes ("generation-3"), generation_3)
            == mesh::peer_admission_result_t::admitted);
    assert (topology.peer (bytes ("peer"))->descriptor.lifecycle_generation == 3);
}

void verify_raw_admission_rejects_lifecycle_mismatch ()
{
    mesh::raw_mesh_node_owner_t first (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("lifecycle-a"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t second (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("lifecycle-b"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    first.start ();
    second.start ();
    auto expected_second = second.topology ().local_descriptor ();
    ++expected_second.lifecycle_generation;
    assert (first.connect_peer (second.endpoint (), expected_second));

    const auto deadline = std::chrono::steady_clock::now () + 500ms;
    while (std::chrono::steady_clock::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) first.observe_routes ();
        (void) second.observe_routes ();
        (void) await_task (first.pump_one (now));
        (void) await_task (second.pump_one (now));
        std::this_thread::sleep_for (1ms);
    }
    assert (!first.topology ().peer (expected_second.node_routing_id));
}

void verify_object_client_connection_requirement ()
{
    auto local = descriptor ("client-a");
    local.object_role = mesh::service_object_role_t::client;
    local.channels.clear ();
    auto remote = descriptor ("client-b", "tcp://127.0.0.1:7002");
    remote.object_role = mesh::service_object_role_t::client;
    remote.channels.clear ();
    remote.state = mesh::service_node_state_t::serving;

    assert (mesh::route_mesh_connection_not_required (local, remote));
    zlink::framework::runtime::state_lane_t topology_lane;
    mesh::service_topology_registry_t topology (topology_lane, local);
    assert (topology.admit (remote, bytes ("client-only-connection"))
            == mesh::peer_admission_result_t::not_required);
    assert (topology.peers ().empty ());
    assert (topology.not_required_peers ().size () == 1);

    auto zero_weight_server = remote;
    zero_weight_server.lifecycle_generation = 2;
    zero_weight_server.descriptor_revision = 1;
    zero_weight_server.channels = {{"audit", 0}};
    assert (!mesh::route_mesh_connection_not_required (local, zero_weight_server));
    assert (topology.admit (zero_weight_server, bytes ("required-connection"), zero_weight_server)
            == mesh::peer_admission_result_t::admitted);
    assert (topology.peers ().size () == 1);
    assert (topology.not_required_peers ().empty ());

    assert (topology.admit (remote, bytes ("stale-client-only-connection"))
            == mesh::peer_admission_result_t::stale_descriptor);
    assert (topology.peers ().size () == 1);
    assert (topology.not_required_peers ().empty ());

    auto local_server_membership = local;
    local_server_membership.descriptor_revision = 2;
    local_server_membership.channels = {{"commands", 0}};
    assert (!mesh::route_mesh_connection_not_required (local_server_membership, remote));
}

void verify_manual_object_client_pair_ends_not_required ()
{
    auto first_descriptor = descriptor ("manual-client-a", "tcp://127.0.0.1:0");
    first_descriptor.object_role = mesh::service_object_role_t::client;
    first_descriptor.channels.clear ();
    auto second_descriptor = descriptor ("manual-client-b", "tcp://127.0.0.1:0");
    second_descriptor.object_role = mesh::service_object_role_t::client;
    second_descriptor.channels.clear ();

    mesh::raw_mesh_node_owner_t first (zlink::framework::test::runtime_failure_options (
      zlink::framework::runtime::mesh::raw_mesh_node_options_t{first_descriptor}));
    mesh::raw_mesh_node_owner_t second (zlink::framework::test::runtime_failure_options (
      zlink::framework::runtime::mesh::raw_mesh_node_options_t{second_descriptor}));
    first.start ();
    second.start ();
    assert (first.connect_peer (second.endpoint ()));

    const auto deadline = mesh::service_liveness_registry_t::clock_t::now () + 2s;
    while ((first.topology ().not_required_peers ().empty ()
            || second.topology ().not_required_peers ().empty ())
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) first.observe_routes ();
        (void) second.observe_routes ();
        (void) await_task (first.pump_one (now));
        (void) await_task (second.pump_one (now));
        std::this_thread::sleep_for (1ms);
    }

    assert (first.topology ().peers ().empty ());
    assert (second.topology ().peers ().empty ());
    assert (first.topology ().not_required_peers ().size () == 1);
    assert (second.topology ().not_required_peers ().size () == 1);
    assert (
      await_task (first.tick_liveness (mesh::service_liveness_registry_t::clock_t::now () + 5s))
        .probes.empty ());
    assert (
      await_task (second.tick_liveness (mesh::service_liveness_registry_t::clock_t::now () + 5s))
        .probes.empty ());

    first.close ();
    second.close ();
}

void verify_signed_weight_contract ()
{
    auto local = descriptor ("weight-local");
    local.channels = {{"weighted", 100}};
    local.placement_weight = 100;
    zlink::framework::runtime::state_lane_t topology_lane;
    mesh::service_topology_registry_t topology (topology_lane, local);

    auto weight_100 = descriptor ("weight-100", "tcp://127.0.0.1:7101");
    weight_100.channels = {{"weighted", 100}};
    weight_100.state = mesh::service_node_state_t::serving;
    auto weight_300 = descriptor ("weight-300", "tcp://127.0.0.1:7102");
    weight_300.channels = {{"weighted", 300}};
    weight_300.state = mesh::service_node_state_t::serving;
    auto weight_zero = descriptor ("weight-zero", "tcp://127.0.0.1:7103");
    weight_zero.channels = {{"weighted", 0}};
    weight_zero.state = mesh::service_node_state_t::serving;
    assert (topology.admit (weight_100, bytes ("weight-connection-100"))
            == mesh::peer_admission_result_t::admitted);
    assert (topology.admit (weight_300, bytes ("weight-connection-300"))
            == mesh::peer_admission_result_t::admitted);
    assert (topology.admit (weight_zero, bytes ("weight-connection-zero"))
            == mesh::peer_admission_result_t::admitted);

    std::size_t selected_100 = 0;
    std::size_t selected_300 = 0;
    for (std::size_t index = 0; index < 400; ++index) {
        const auto selected = topology.select ("weighted");
        assert (selected);
        if (selected.value () == bytes ("weight-100"))
            ++selected_100;
        else if (selected.value () == bytes ("weight-300"))
            ++selected_300;
        else
            assert (false);
    }
    assert (selected_100 == 100);
    assert (selected_300 == 300);

    const auto multicast = topology.multicast_targets ("weighted");
    assert (multicast.size () == 2);
    assert (std::count_if (multicast.begin (), multicast.end (),
                           [] (const auto &peer) {
                               return peer.descriptor.node_routing_id == bytes ("weight-100");
                           })
            == 1);
    assert (std::count_if (multicast.begin (), multicast.end (),
                           [] (const auto &peer) {
                               return peer.descriptor.node_routing_id == bytes ("weight-300");
                           })
            == 1);

    auto revision = weight_100;
    revision.descriptor_revision = 2;
    revision.channels.front ().weight = 0;
    assert (topology.admit (revision, bytes ("weight-connection-100"),
                            mesh::service_liveness_registry_t::clock_t::now (),
                            protocol::command::update)
            == mesh::peer_admission_result_t::admitted);
    const auto after_revision = topology.multicast_targets ("weighted");
    assert (after_revision.size () == 1);
    assert (after_revision.front ().descriptor.node_routing_id == bytes ("weight-300"));

    auto invalid_negative = local;
    invalid_negative.channels.front ().weight = -1;
    bool rejected_negative = false;
    try {
        zlink::framework::runtime::state_lane_t invalid_lane;
        mesh::service_topology_registry_t invalid (invalid_lane, invalid_negative);
    }
    catch (const std::invalid_argument &) {
        rejected_negative = true;
    }
    assert (rejected_negative);

    auto invalid_upper = local;
    invalid_upper.placement_weight = 10001;
    bool rejected_upper = false;
    try {
        zlink::framework::runtime::state_lane_t invalid_lane;
        mesh::service_topology_registry_t invalid (invalid_lane, invalid_upper);
    }
    catch (const std::invalid_argument &) {
        rejected_upper = true;
    }
    assert (rejected_upper);

    std::vector<int> overflow_safe_weights (430000, 10000);
    assert (mesh::sum_service_weights (overflow_safe_weights) == 4'300'000'000ull);
}

// Do not pump the sender: its selected route has not completed admission.
// Let the operation deadline end the observation, rather than delaying a pump.
void verify_durable_attempt_waits_for_admit (protocol::command kind)
{
    constexpr auto unadmitted_timeout = 300ms;
    mesh::raw_mesh_node_owner_t source (zlink::framework::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("preadmit-source")}));
    mesh::raw_mesh_node_owner_t target (zlink::framework::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("preadmit-target")}));
    source.start ();
    target.start ();
    const auto local = source.topology ().local_descriptor ();
    const auto remote = target.topology ().local_descriptor ();
    source.expect_peer (remote);
    using completion_t = foundation::operation_terminal_t;
    std::promise<completion_t> promise;
    auto completed = promise.get_future ();
    std::optional<zlink::framework::task_t<mesh::actor_join_wire_outcome_t>> join;
    const auto callback = [&promise] (completion_t terminal,
                                      ::zlink::framework::result_t<zlink::message_t>) {
        promise.set_value (terminal);
    };
    if (kind == protocol::command::boundSessionBind)
        assert (await_task (source.request_bound_session_bind (
          remote.node_routing_id, bound_session_bind_request (remote, "preadmit-actor"),
          unadmitted_timeout, callback)));
    else if (kind == protocol::command::actorCreate)
        assert (await_task (source.request_actor_create (
          remote.node_routing_id, actor_create_request (local, remote, "preadmit-actor"),
          unadmitted_timeout, callback)));
    else {
        assert (kind == protocol::command::actorJoin);
        join.emplace (source.request_actor_join (
          remote.node_routing_id, actor_join_request (local, remote, 81, "preadmit-actor", "spot"),
          std::nullopt, unadmitted_timeout));
    }
    assert (source.connect_peer (target.endpoint (), remote));
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while ((join ? !join->await_ready () : completed.wait_for (0ms) != std::future_status::ready)
           && std::chrono::steady_clock::now () < deadline) {
        (void) source.expire_requests (std::chrono::steady_clock::now ()).result ().value ();
        const auto pumped = await_task (target.pump_one (std::chrono::steady_clock::now ()));
        assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        assert (
          !target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096));
    }
    if (join) {
        assert (join->await_ready ());
        assert (await_task (std::move (*join)).failure
                == mesh::actor_join_wire_failure_t::deadline_exceeded);
    } else {
        assert (completed.wait_for (0ms) == std::future_status::ready);
        assert (completed.get () == completion_t::timed_out);
    }
    // A new operation on the same unobserved route starts at the ADMIT transition.
    std::promise<std::pair<completion_t, std::vector<std::uint8_t>>> ready_promise;
    auto ready_completion = ready_promise.get_future ();
    assert (await_task (source.request_bound_session_bind (
      remote.node_routing_id, bound_session_bind_request (remote, "after-admit"), 2s,
      [&ready_promise] (completion_t terminal,
                        ::zlink::framework::result_t<zlink::message_t> payload) {
          ready_promise.set_value ({terminal, payload.value ().to_bytes ()});
      })));
    admit_pair (source, target, remote);
    complete_bound_session_bind (source, target, ready_completion, "after-admit");
    source.close ();
    target.close ();
}


void verify_topology_ready_is_remote_admit_on_current_route ()
{
    zlink::framework::runtime::state_lane_t topology_lane;
    mesh::service_topology_registry_t topology (topology_lane, descriptor ("ready-local"));
    auto remote = descriptor ("ready-remote");
    remote.state = mesh::service_node_state_t::serving;
    auto observe = topology.observe_peer (remote.node_routing_id);
    auto initial = observe ();
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    assert (topology.admit (remote, bytes ("route-one"), now, protocol::command::hello)
            == mesh::peer_admission_result_t::admitted);
    assert (initial.changed.await_ready ());
    auto hello = observe ();
    assert (hello.peer && !hello.peer->ready);
    assert (!topology.select ("alpha"));
    assert (topology.multicast_targets ("alpha").empty ());
    assert (topology.admit (remote, bytes ("route-one"), now, protocol::command::admit)
            == mesh::peer_admission_result_t::duplicate_connection);
    assert (hello.changed.await_ready ());
    auto ready = observe ();
    assert (ready.peer && ready.peer->ready);
    assert (ready.peer->admission_epoch == hello.peer->admission_epoch);
    assert (topology.select ("alpha"));
    assert (topology.multicast_targets ("alpha").size () == 1);
    assert (topology.admit (remote, bytes ("route-one"), now, protocol::command::hello)
            == mesh::peer_admission_result_t::duplicate_connection);
    assert (!ready.changed.await_ready ());
    assert (topology.admit (remote, bytes ("route-two"), now, protocol::command::hello)
            == mesh::peer_admission_result_t::admitted);
    assert (ready.changed.await_ready ());
    assert (!observe ().peer->ready);
}

void verify_application_ingress_after_local_admit ()
{
    const auto local = descriptor ("ingress-target");
    const auto remote = descriptor ("ingress-source");
    mesh::raw_mesh_node_owner_t target (
      zlink::framework::test::runtime_failure_options (mesh::raw_mesh_node_options_t{local}));
    target.start ();

    zlink::context_t context;
    zlink::router_socket_t source (context);
    source.set_routing_id (zlink::routing_id_t::from (remote.node_routing_id));
    source.options ().mandatory (true);
    source.options ().linger (0ms);
    source.options ().connect_routing_id (zlink::routing_id_t::from (local.node_routing_id));
    source.connect (target.endpoint ());
    zlink::framework::detail::backend::raw_route_port_t port (source);
    const auto route_deadline = std::chrono::steady_clock::now () + 2s;
    while (source.routes_snapshot ().empty () && std::chrono::steady_clock::now () < route_deadline)
        (void) port.poll (0ms);
    assert (!source.routes_snapshot ().empty ());

    const auto pump_record = [&] {
        const auto deadline = std::chrono::steady_clock::now () + 2s;
        auto result = mesh::raw_mesh_pump_result_t::no_data;
        while (result == mesh::raw_mesh_pump_result_t::no_data
               && std::chrono::steady_clock::now () < deadline)
            result = await_task (target.pump_one (std::chrono::steady_clock::now ()));
        return result;
    };
    namespace wire = protocol::generated::detail;
    wire::logicalMulticast_23_t multicast;
    multicast.channelName.value = "alpha";
    multicast.topic.value = "ingress";
    multicast.sourceSpotId.value = "source-spot";
    wire::writer_t header;
    assert (wire::encode_head_logicalMulticast_23 (header, multicast) == wire::error_code::ok);
    const auto send_multicast = [&] (std::vector<std::uint8_t> payload) {
        assert (await_task (
          port.send (local.node_routing_id,
                     {header.data, protocol::encode_application_payload (
                                     {"Ingress", "application/json", std::move (payload)})})));
    };
    constexpr auto application = mesh::service_mailbox_domain_t::application;
    send_multicast (bytes ("\"before HELLO\""));
    assert (pump_record () == mesh::raw_mesh_pump_result_t::infrastructure);
    assert (target.mailbox ().pending_messages (application) == 0);

    assert (await_task (port.send (local.node_routing_id, {protocol::encode_route_mesh_admission (
                                                            protocol::command::hello, remote)})));
    assert (pump_record () == mesh::raw_mesh_pump_result_t::infrastructure);
    const auto admitted = target.topology ().peer (remote.node_routing_id);
    assert (admitted && !admitted->ready);

    const auto payload = bytes ("\"after HELLO\"");
    send_multicast (payload);
    assert (pump_record () == mesh::raw_mesh_pump_result_t::application);
    auto claim = zlink::framework::test::claim_mailbox (target.mailbox (), application, 2, 4096);
    assert (claim && claim->records.size () == 1);
    const auto &record = *claim->records.front ();
    assert (record.source_routing_id == remote.node_routing_id);
    assert (protocol::decode_application_payload (protocol::message_bytes (record.parts ().back ()))
              .payload_bytes ()
            == payload);
    assert (target.mailbox ().release (*claim));

    port.close ();
    target.close ();
}

void verify_unadmitted_route (bool repeat_hello)
{
    mesh::raw_mesh_node_owner_t target (zlink::framework::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("unadmitted-target")}));
    target.start ();
    // Publish never-due with no admitted peers before the selected route appears.
    // HELLO retry must retain a management deadline without admitted peers.
    const auto now = mesh::service_liveness_registry_t::clock_t::time_point{} + 1s;
    (void) target.liveness ().tick (now);
    assert (!target.liveness ().due (now));
    zlink::context_t context;
    zlink::router_socket_t source (context);
    source.set_routing_id (zlink::routing_id_t::from ("unadmitted-source"));
    source.options ().mandatory (true);
    source.options ().linger (0ms);
    source.options ().connect_routing_id (zlink::routing_id_t::from ("unadmitted-target"));
    source.connect (target.endpoint ());
    zlink::framework::detail::backend::raw_route_port_t port (source);
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (source.routes_snapshot ().empty () && std::chrono::steady_clock::now () < deadline)
        (void) port.poll (0ms);
    assert (!source.routes_snapshot ().empty ());
    if (repeat_hello) {
        const auto receive_hello = [&] {
            while (std::chrono::steady_clock::now () < deadline) {
                (void) target.observe_routes ();
                if (auto record = port.try_receive ()) {
                    assert (
                      protocol::decode_header (protocol::message_bytes (record->parts ().front ()))
                        .kind
                      == protocol::command::hello);
                    return true;
                }
            }
            return false;
        };
        assert (receive_hello ());
        assert (target.liveness ().size () == 0);
        const auto started = std::chrono::steady_clock::now ();
        const auto next_activity = target.liveness ().next_activity ();
        constexpr auto probe_interval = 5s;
        constexpr int periods = 4;
        constexpr int rotations_per_period = 10;
        constexpr auto rotation_interval =
          std::chrono::duration_cast<std::chrono::milliseconds> (probe_interval)
          / rotations_per_period;
        int hello_count = 0;
        bool deadlines_follow_probe_interval = true;
        // Ten management rotations per probe period, with no ADMIT from the peer.
        // Consume every due HELLO so the count observes submissions, not socket timing.
        for (int rotation = 0; rotation <= periods * rotations_per_period; ++rotation) {
            const auto virtual_now = started + rotation * rotation_interval;
            const bool due = target.liveness ().due (virtual_now);
            (void) await_task (target.tick_liveness (virtual_now));
            if (due) {
                assert (receive_hello ());
                ++hello_count;
                deadlines_follow_probe_interval &=
                  target.liveness ().next_activity () == virtual_now + probe_interval
                  && !target.liveness ().due (virtual_now);
            }
        }
        assert (hello_count == periods);
        assert (deadlines_follow_probe_interval);
        assert (next_activity && *next_activity > started);
        assert (*next_activity <= started + probe_interval);
        const auto remote = descriptor ("unadmitted-source");
        assert (await_task (
          port.send (bytes ("unadmitted-target"),
                     {protocol::encode_route_mesh_admission (protocol::command::hello, remote)})));
        while (!target.topology ().peer (remote.node_routing_id)
               && std::chrono::steady_clock::now () < deadline)
            (void) await_task (target.pump_one (std::chrono::steady_clock::now ()));
        const auto peer = target.topology ().peer (remote.node_routing_id);
        assert (peer && !peer->ready);
        bool admit_received = false;
        while (!admit_received && std::chrono::steady_clock::now () < deadline)
            if (auto record = port.try_receive ()) {
                assert (
                  protocol::decode_header (protocol::message_bytes (record->parts ().front ())).kind
                  == protocol::command::admit);
                admit_received = true;
            }
        assert (admit_received);
        (void) await_task (target.tick_liveness (std::chrono::steady_clock::now ()));
        assert (receive_hello ());
        assert (await_task (
          port.send (bytes ("unadmitted-target"),
                     {protocol::encode_route_mesh_admission (protocol::command::admit, remote)})));
        while (!target.topology ().peer (remote.node_routing_id)->ready
               && std::chrono::steady_clock::now () < deadline)
            (void) await_task (target.pump_one (std::chrono::steady_clock::now ()));
        assert (target.topology ().peer (remote.node_routing_id)->ready);
        // ADMIT retires the HELLO bound in its owner turn, before another tick.
        assert (target.liveness ().next_activity ().value () > now);
        (void) await_task (target.tick_liveness (std::chrono::steady_clock::now ()));
        assert (await_task (target.send_to_node (
          remote.node_routing_id, {"AfterAdmit", "application/json", bytes ("{}")})));
        // The following DATA is an ordering barrier for the same Application lane.
        bool data_received = false;
        while (!data_received && std::chrono::steady_clock::now () < deadline)
            if (auto record = port.try_receive ()) {
                const auto kind =
                  protocol::decode_header (protocol::message_bytes (record->parts ().front ()))
                    .kind;
                assert (kind == protocol::command::nodeSend
                        || kind == protocol::command::livenessProbe);
                data_received = kind == protocol::command::nodeSend;
            }
        assert (data_received);
    } else {
        const auto update = protocol::encode_route_mesh_admission (
          protocol::command::update, descriptor ("unadmitted-source"));
        assert (await_task (port.send (bytes ("unadmitted-target"), {update})));
        auto update_result = mesh::raw_mesh_pump_result_t::no_data;
        while (update_result == mesh::raw_mesh_pump_result_t::no_data
               && std::chrono::steady_clock::now () < deadline)
            update_result = await_task (target.pump_one (std::chrono::steady_clock::now ()));
        assert (update_result == mesh::raw_mesh_pump_result_t::infrastructure);
        assert (!target.topology ().peer (bytes ("unadmitted-source")));
        auto pending = port.request (
          bytes ("unadmitted-target"),
          {protocol::encode_node_request_header (91),
           protocol::encode_application_payload ({"Silent", "application/json", bytes ("{}")})},
          100ms);
        while (!pending.await_ready () && std::chrono::steady_clock::now () < deadline) {
            (void) await_task (target.pump_one (std::chrono::steady_clock::now ()));
            (void) port.poll (0ms);
        }
        assert (pending.await_ready ());
        assert (await_task (std::move (pending)).terminal == zlink::request_result_t::timed_out);
        assert (!zlink::framework::test::claim_mailbox (
          target.mailbox (), mesh::service_mailbox_domain_t::application, 1, 4096));
    }
    port.close ();
    target.close ();
}
}
