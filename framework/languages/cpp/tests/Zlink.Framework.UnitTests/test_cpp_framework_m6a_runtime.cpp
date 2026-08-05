/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/locations/service_descriptor_registry.hpp"
#include "runtime/fanout/raw_fanout_owner.hpp"
#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/client_server/weighted_selector.hpp"
#include "runtime/dispatch/inbound_dispatch_budget.hpp"
#include "runtime/dispatch/completion_admission_owner.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace mesh = zlink::framework::runtime::mesh;
namespace locations = zlink::framework::runtime::locations;
namespace fanout = zlink::framework::runtime::fanout;
namespace client_server = zlink::framework::runtime::client_server;
namespace foundation = zlink::framework::runtime::foundation;
namespace protocol = zlink::framework::runtime::protocol;
namespace runtime = zlink::framework::runtime;
using namespace std::chrono_literals;

namespace
{

std::vector<std::uint8_t> bytes (std::string value)
{
    return {value.begin (), value.end ()};
}

void verify_host_wide_application_byte_budget ()
{
    runtime::inbound_dispatch_budget_t budget (100);
    assert (budget.can_start_application_receive ());

    budget.received (120);
    auto paused = budget.snapshot ();
    assert (paused.application_hwm_bytes == 100);
    assert (paused.pending_payload_bytes == 120);
    assert (paused.queued_payload_bytes == 120);
    assert (paused.active_payload_bytes == 0);
    assert (paused.application_receive_paused);
    assert (!budget.can_start_application_receive ());

    budget.handler_started (120);
    auto active = budget.snapshot ();
    assert (active.pending_payload_bytes == 120);
    assert (active.queued_payload_bytes == 0);
    assert (active.active_payload_bytes == 120);
    assert (active.application_receive_paused);

    budget.completed (120, true);
    auto resumed = budget.snapshot ();
    assert (resumed.pending_payload_bytes == 0);
    assert (resumed.queued_payload_bytes == 0);
    assert (resumed.active_payload_bytes == 0);
    assert (!resumed.application_receive_paused);
    assert (budget.can_start_application_receive ());

    runtime::inbound_dispatch_budget_t exact (100);
    exact.received (99);
    assert (exact.can_start_application_receive ());
    exact.received (1);
    const auto at_limit = exact.snapshot ();
    assert (at_limit.pending_payload_bytes == 100);
    assert (at_limit.application_receive_paused);
    assert (!exact.can_start_application_receive ());
    exact.completed (1, false);
    const auto below_limit = exact.snapshot ();
    assert (below_limit.pending_payload_bytes == 99);
    assert (!below_limit.application_receive_paused);
    assert (exact.can_start_application_receive ());
    exact.completed (99, false);

    runtime::inbound_dispatch_budget_t unlimited (0);
    unlimited.received (1024);
    assert (unlimited.can_start_application_receive ());
    unlimited.completed (1024, false);
    assert (unlimited.snapshot ().pending_payload_bytes == 0);
}

void verify_host_wide_budget_waits_for_terminal_completion ()
{
    runtime::inbound_dispatch_budget_t budget (10);
    budget.received (10);
    auto waiting = std::async (
      std::launch::async, [&budget] { return budget.wait_for_application_capacity (); });
    assert (waiting.wait_for (20ms) == std::future_status::timeout);

    budget.completed (10, false);
    assert (waiting.wait_for (1s) == std::future_status::ready);
    assert (waiting.get ());
}

void verify_host_wide_completion_send_permits ()
{
    auto owner =
      std::make_shared<runtime::completion_admission_owner_t> (1);
    auto first = owner->acquire ();
    assert (first);
    auto waiting = std::async (
      std::launch::async, [owner] { return owner->acquire (); });
    while (owner->snapshot ().pending_completion_sends != 2)
        std::this_thread::yield ();
    const auto saturated = owner->snapshot ();
    assert (saturated.pending_completion_sends == 2);
    assert (saturated.completion_send_limit == 1);
    first = {};
    auto second = waiting.get ();
    assert (second);
    assert (owner->snapshot ().pending_completion_sends == 1);
    second = {};
    assert (owner->snapshot ().pending_completion_sends == 0);
}

void verify_actor_create_command_49_roundtrip ()
{
    const protocol::actor_create_header_t request{
      19,
      {20, 21},
      bytes ("source"),
      22,
      "actor-1",
      "player",
      {"reservation-1", "store-1", 23, 24,
       bytes ("target"), 25, "owner-1", 26, 1},
      27};
    assert (protocol::decode_actor_create_header (
              protocol::encode_actor_create_header (request))
            == request);

    const auto created = protocol::decode_actor_create_reply (
      protocol::encode_actor_create_reply (
        19, 0, 0, protocol::actor_create_result_t::created,
        bytes ("target"), "actor-1", 23));
    assert (created.header.correlation == 19);
    assert (created.result
            == protocol::actor_create_result_t::created);
    assert (created.node_routing_id == bytes ("target"));
    assert (created.actor_id == "actor-1");
    assert (created.object_generation == 23);

    const auto rejected = protocol::decode_actor_create_reply (
      protocol::encode_actor_create_reply (
        19, 0, 0, protocol::actor_create_result_t::rejected,
        {}, {}, 0));
    assert (rejected.result
            == protocol::actor_create_result_t::rejected);
    assert (rejected.node_routing_id.empty ());
}

mesh::service_node_descriptor_t descriptor (
  std::string rid,
  std::string endpoint = "tcp://127.0.0.1:0")
{
    return mesh::service_node_descriptor_t{
      "m6a-mesh", bytes (std::move (rid)), 1, 1, std::move (endpoint),
      {{"alpha", 100}, {"beta", 50}},
      mesh::service_node_state_t::preparing};
}

void verify_topology_snapshot_and_connection_fence ()
{
    mesh::service_topology_registry_t topology (descriptor ("local"));
    auto peer = descriptor ("peer", "tcp://127.0.0.1:7001");
    peer.state = mesh::service_node_state_t::serving;
    const auto first_connection = bytes ("connection-a");
    assert (topology.admit (peer, first_connection)
            == mesh::peer_admission_result_t::admitted);
    assert (topology.select ("alpha")->descriptor.node_routing_id == bytes ("peer"));

    auto older = peer;
    older.descriptor_revision = 0;
    assert (topology.admit (older, bytes ("ignored"))
            == mesh::peer_admission_result_t::invalid_descriptor);

    const auto replacement_connection = bytes ("connection-b");
    assert (topology.admit (peer, replacement_connection)
            == mesh::peer_admission_result_t::admitted);
    assert (!topology.disconnect (bytes ("peer"), first_connection));
    assert (topology.peer (bytes ("peer"))->connection_id
            == replacement_connection);

    auto equal_revision_mutation = peer;
    equal_revision_mutation.state = mesh::service_node_state_t::retiring;
    assert (topology.admit (equal_revision_mutation, replacement_connection)
            == mesh::peer_admission_result_t::stale_descriptor);

    auto immutable_mutation = peer;
    immutable_mutation.descriptor_revision = 2;
    immutable_mutation.security_identity = "changed-security";
    assert (topology.admit (immutable_mutation, replacement_connection)
            == mesh::peer_admission_result_t::stale_descriptor);

    peer.descriptor_revision = 2;
    peer.state = mesh::service_node_state_t::retiring;
    assert (topology.admit (peer, replacement_connection)
            == mesh::peer_admission_result_t::admitted);
    assert (!topology.select ("alpha"));
}

void verify_duplicate_connection_survivor_is_symmetric ()
{
    auto lower = descriptor ("aa");
    auto higher = descriptor ("zz", "tcp://127.0.0.1:7003");
    higher.state = mesh::service_node_state_t::serving;
    mesh::service_topology_registry_t lower_topology (lower);

    assert (lower_topology.admit (
              higher, bytes ("inbound"),
              mesh::service_connection_direction_t::inbound)
            == mesh::peer_admission_result_t::admitted);
    assert (lower_topology.admit (
              higher, bytes ("outbound"),
              mesh::service_connection_direction_t::outbound)
            == mesh::peer_admission_result_t::admitted);
    assert (lower_topology.peer (bytes ("zz"))->connection_id
            == bytes ("outbound"));
    assert (lower_topology.admit (
              higher, bytes ("second-inbound"),
              mesh::service_connection_direction_t::inbound)
            == mesh::peer_admission_result_t::duplicate_connection);
    assert (!lower_topology.disconnect (
      bytes ("zz"), bytes ("second-inbound")));
    assert (lower_topology.peer (bytes ("zz"))->connection_id
            == bytes ("outbound"));
    mesh::service_liveness_registry_t liveness;
    liveness.admit (
      bytes ("zz"), bytes ("outbound"),
      mesh::service_liveness_registry_t::clock_t::now ());
    assert (!liveness.disconnect (
      bytes ("zz"), bytes ("second-inbound")));
    assert (liveness.size () == 1);

    auto lower_peer = descriptor (
      "aa", "tcp://127.0.0.1:7004");
    lower_peer.state = mesh::service_node_state_t::serving;
    mesh::service_topology_registry_t higher_topology (
      descriptor ("zz"));
    assert (higher_topology.admit (
              lower_peer, bytes ("outbound"),
              mesh::service_connection_direction_t::outbound)
            == mesh::peer_admission_result_t::admitted);
    assert (higher_topology.admit (
              lower_peer, bytes ("inbound"),
              mesh::service_connection_direction_t::inbound)
            == mesh::peer_admission_result_t::admitted);
    assert (higher_topology.peer (bytes ("aa"))->connection_id
            == bytes ("inbound"));
}

void verify_lifecycle_token_requires_current_discovery_expectation ()
{
    mesh::service_topology_registry_t topology (
      descriptor ("local"));
    auto generation_99 = descriptor (
      "peer", "tcp://127.0.0.1:7099");
    generation_99.lifecycle_generation = 99;
    generation_99.state =
      mesh::service_node_state_t::serving;
    assert (topology.admit (
              generation_99, bytes ("generation-99"),
              mesh::service_connection_direction_t::outbound,
              generation_99)
            == mesh::peer_admission_result_t::admitted);

    auto generation_3 = generation_99;
    generation_3.lifecycle_generation = 3;
    generation_3.descriptor_revision = 1;
    assert (topology.admit (
              generation_3, bytes ("generation-3-without-expectation"),
              mesh::service_connection_direction_t::outbound)
            == mesh::peer_admission_result_t::stale_descriptor);
    assert (topology.peer (bytes ("peer"))->descriptor.lifecycle_generation
            == 99);

    auto wrong_expectation = generation_3;
    wrong_expectation.lifecycle_generation = 4;
    assert (topology.admit (
              generation_3, bytes ("generation-3-mismatch"),
              mesh::service_connection_direction_t::outbound,
              wrong_expectation)
            == mesh::peer_admission_result_t::stale_descriptor);
    wrong_expectation = generation_3;
    wrong_expectation.advertised_endpoint =
      "tcp://127.0.0.1:7100";
    assert (topology.admit (
              generation_3, bytes ("generation-3-endpoint-mismatch"),
              mesh::service_connection_direction_t::outbound,
              wrong_expectation)
            == mesh::peer_admission_result_t::stale_descriptor);
    wrong_expectation = generation_3;
    wrong_expectation.security_identity =
      "different-security";
    assert (topology.admit (
              generation_3, bytes ("generation-3-security-mismatch"),
              mesh::service_connection_direction_t::outbound,
              wrong_expectation)
            == mesh::peer_admission_result_t::stale_descriptor);

    assert (topology.admit (
              generation_3, bytes ("generation-3"),
              mesh::service_connection_direction_t::outbound,
              generation_3)
            == mesh::peer_admission_result_t::admitted);
    assert (topology.peer (bytes ("peer"))->descriptor.lifecycle_generation
            == 3);
}

void verify_physical_candidates_preserve_survivor ()
{
    mesh::raw_mesh_connection_candidates_t candidates;
    const auto peer = bytes ("peer");
    const auto inbound = bytes ("inbound-physical");
    const auto outbound = bytes ("outbound-physical");
    const auto late_inbound = bytes ("late-inbound-physical");
    candidates.ready (
      peer, inbound,
      mesh::service_connection_direction_t::inbound,
      "tcp://127.0.0.1:7101");
    candidates.ready (
      peer, outbound,
      mesh::service_connection_direction_t::outbound,
      "tcp://127.0.0.1:7102");
    assert (candidates.size (peer) == 2);
    assert (candidates.for_handshake (
              peer,
              mesh::service_connection_direction_t::inbound)
              ->connection_id
            == inbound);
    assert (candidates.for_handshake (
              peer,
              mesh::service_connection_direction_t::inbound)
              ->remote_endpoint
            == "tcp://127.0.0.1:7101");
    assert (candidates.for_handshake (
              peer,
              mesh::service_connection_direction_t::outbound)
              ->connection_id
            == outbound);
    assert (candidates.for_handshake (
              peer,
              mesh::service_connection_direction_t::outbound)
              ->remote_endpoint
            == "tcp://127.0.0.1:7102");

    candidates.ready (
      peer, late_inbound,
      mesh::service_connection_direction_t::inbound,
      "tcp://127.0.0.1:7103");
    assert (candidates.size (peer) == 3);
    assert (candidates.for_handshake (
              peer,
              mesh::service_connection_direction_t::inbound)
              ->connection_id
            == late_inbound);
    assert (candidates.for_handshake (
              peer,
              mesh::service_connection_direction_t::outbound)
              ->connection_id
            == outbound);

    assert (candidates.disconnect (peer, inbound));
    assert (!candidates.disconnect (peer, inbound));
    assert (candidates.disconnect (peer, late_inbound));
    assert (candidates.size (peer) == 1);
    assert (candidates.for_handshake (
              peer,
              mesh::service_connection_direction_t::inbound)
              ->connection_id
            == outbound);
}

void verify_bilateral_raw_connection_without_public_pipe_id_keeps_survivor ()
{
    /* Public ROUTER receive exposes the peer RID but not the physical
     * connection ID. Exercise the runtime's deterministic direction rule
     * with both physical directions present, then drain late monitor and
     * handshake events to prove that they cannot replace the survivor. */
    mesh::raw_mesh_node_owner_t lower (
      mesh::raw_mesh_node_options_t{
        descriptor ("bilateral-aa")});
    mesh::raw_mesh_node_owner_t higher (
      mesh::raw_mesh_node_options_t{
        descriptor ("bilateral-zz")});
    lower.start ();
    higher.start ();
    const auto lower_descriptor =
      lower.topology ().local_descriptor ();
    const auto higher_descriptor =
      higher.topology ().local_descriptor ();
    lower.expect_peer (higher_descriptor);
    higher.expect_peer (lower_descriptor);
    assert (lower.connect_peer (
      higher.endpoint (), higher_descriptor));
    assert (higher.connect_peer (
      lower.endpoint (), lower_descriptor));

    const auto deadline =
      std::chrono::steady_clock::now () + 5s;
    while (std::chrono::steady_clock::now () < deadline) {
        const auto now =
          mesh::service_liveness_registry_t::clock_t::now ();
        (void) lower.drain_monitor_events (now);
        (void) higher.drain_monitor_events (now);
        (void) lower.pump_one (now);
        (void) higher.pump_one (now);
        const auto lower_peer =
          lower.topology ().peer (
            higher_descriptor.node_routing_id);
        const auto higher_peer =
          higher.topology ().peer (
            lower_descriptor.node_routing_id);
        if (lower_peer && higher_peer
            && lower_peer->direction
                 == mesh::service_connection_direction_t::outbound
            && higher_peer->direction
                 == mesh::service_connection_direction_t::inbound)
            break;
        std::this_thread::sleep_for (1ms);
    }

    const auto lower_survivor =
      lower.topology ().peer (
        higher_descriptor.node_routing_id);
    const auto higher_survivor =
      higher.topology ().peer (
        lower_descriptor.node_routing_id);
    assert (lower_survivor);
    assert (higher_survivor);
    assert (lower_survivor->direction
            == mesh::service_connection_direction_t::outbound);
    assert (higher_survivor->direction
            == mesh::service_connection_direction_t::inbound);

    const auto settle_deadline =
      std::chrono::steady_clock::now () + 100ms;
    while (std::chrono::steady_clock::now ()
           < settle_deadline) {
        const auto now =
          mesh::service_liveness_registry_t::clock_t::now ();
        (void) lower.drain_monitor_events (now);
        (void) higher.drain_monitor_events (now);
        (void) lower.pump_one (now);
        (void) higher.pump_one (now);
        std::this_thread::sleep_for (1ms);
    }
    assert (lower.topology ().peer (
              higher_descriptor.node_routing_id)
              ->connection_id
            == lower_survivor->connection_id);
    assert (higher.topology ().peer (
              lower_descriptor.node_routing_id)
              ->connection_id
            == higher_survivor->connection_id);
}

void verify_raw_admission_rejects_lifecycle_mismatch ()
{
    mesh::raw_mesh_node_owner_t first (
      mesh::raw_mesh_node_options_t{
        descriptor ("lifecycle-a")});
    mesh::raw_mesh_node_owner_t second (
      mesh::raw_mesh_node_options_t{
        descriptor ("lifecycle-b")});
    first.start ();
    second.start ();
    auto expected_second =
      second.topology ().local_descriptor ();
    ++expected_second.lifecycle_generation;
    assert (first.connect_peer (
      second.endpoint (), expected_second));

    const auto deadline =
      std::chrono::steady_clock::now () + 500ms;
    while (std::chrono::steady_clock::now () < deadline) {
        const auto now =
          mesh::service_liveness_registry_t::clock_t::now ();
        (void) first.drain_monitor_events (now);
        (void) second.drain_monitor_events (now);
        (void) first.pump_one (now);
        (void) second.pump_one (now);
        std::this_thread::sleep_for (1ms);
    }
    assert (!first.topology ().peer (
      expected_second.node_routing_id));
}

void verify_object_client_connection_requirement ()
{
    auto local = descriptor ("client-a");
    local.object_role = mesh::service_object_role_t::client;
    local.channels.clear ();
    auto remote = descriptor (
      "client-b", "tcp://127.0.0.1:7002");
    remote.object_role = mesh::service_object_role_t::client;
    remote.channels.clear ();
    remote.state = mesh::service_node_state_t::serving;

    assert (mesh::route_mesh_connection_not_required (
      local, remote));
    mesh::service_topology_registry_t topology (local);
    assert (topology.admit (
              remote, bytes ("client-only-connection"))
            == mesh::peer_admission_result_t::not_required);
    assert (topology.peers ().empty ());
    assert (topology.not_required_peers ().size () == 1);

    auto zero_weight_server = remote;
    zero_weight_server.lifecycle_generation = 2;
    zero_weight_server.descriptor_revision = 1;
    zero_weight_server.channels = {{"audit", 0}};
    assert (!mesh::route_mesh_connection_not_required (
      local, zero_weight_server));
    assert (topology.admit (
              zero_weight_server, bytes ("required-connection"),
              mesh::service_connection_direction_t::inbound,
              zero_weight_server)
            == mesh::peer_admission_result_t::admitted);
    assert (topology.peers ().size () == 1);
    assert (topology.not_required_peers ().empty ());

    assert (topology.admit (
              remote, bytes ("stale-client-only-connection"))
            == mesh::peer_admission_result_t::stale_descriptor);
    assert (topology.peers ().size () == 1);
    assert (topology.not_required_peers ().empty ());

    auto local_server_membership = local;
    local_server_membership.descriptor_revision = 2;
    local_server_membership.channels = {{"commands", 0}};
    assert (!mesh::route_mesh_connection_not_required (
      local_server_membership, remote));
}

void verify_manual_object_client_pair_ends_not_required ()
{
    auto first_descriptor = descriptor (
      "manual-client-a", "tcp://127.0.0.1:0");
    first_descriptor.object_role =
      mesh::service_object_role_t::client;
    first_descriptor.channels.clear ();
    auto second_descriptor = descriptor (
      "manual-client-b", "tcp://127.0.0.1:0");
    second_descriptor.object_role =
      mesh::service_object_role_t::client;
    second_descriptor.channels.clear ();

    mesh::raw_mesh_node_owner_t first (
      {first_descriptor, 16, 1024, 16, 1024});
    mesh::raw_mesh_node_owner_t second (
      {second_descriptor, 16, 1024, 16, 1024});
    first.start ();
    second.start ();
    assert (first.connect_peer (second.endpoint ()));

    const auto deadline =
      mesh::service_liveness_registry_t::clock_t::now () + 2s;
    while ((first.topology ().not_required_peers ().empty ()
            || second.topology ().not_required_peers ().empty ())
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        const auto now =
          mesh::service_liveness_registry_t::clock_t::now ();
        (void) first.drain_monitor_events (now);
        (void) second.drain_monitor_events (now);
        (void) first.pump_one (now);
        (void) second.pump_one (now);
        std::this_thread::sleep_for (1ms);
    }

    assert (first.topology ().peers ().empty ());
    assert (second.topology ().peers ().empty ());
    assert (first.topology ().not_required_peers ().size () == 1);
    assert (second.topology ().not_required_peers ().size () == 1);
    assert (first.tick_liveness (
              mesh::service_liveness_registry_t::clock_t::now ()
              + 5s).probes.empty ());
    assert (second.tick_liveness (
              mesh::service_liveness_registry_t::clock_t::now ()
              + 5s).probes.empty ());

    first.close ();
    second.close ();
}

void verify_signed_weight_contract ()
{
    auto local = descriptor ("weight-local");
    local.channels = {{"weighted", 100}};
    local.placement_weight = 100;
    mesh::service_topology_registry_t topology (local);

    auto weight_100 =
      descriptor ("weight-100", "tcp://127.0.0.1:7101");
    weight_100.channels = {{"weighted", 100}};
    weight_100.state = mesh::service_node_state_t::serving;
    auto weight_300 =
      descriptor ("weight-300", "tcp://127.0.0.1:7102");
    weight_300.channels = {{"weighted", 300}};
    weight_300.state = mesh::service_node_state_t::serving;
    auto weight_zero =
      descriptor ("weight-zero", "tcp://127.0.0.1:7103");
    weight_zero.channels = {{"weighted", 0}};
    weight_zero.state = mesh::service_node_state_t::serving;
    assert (
      topology.admit (weight_100, bytes ("weight-connection-100"))
      == mesh::peer_admission_result_t::admitted);
    assert (
      topology.admit (weight_300, bytes ("weight-connection-300"))
      == mesh::peer_admission_result_t::admitted);
    assert (
      topology.admit (weight_zero, bytes ("weight-connection-zero"))
      == mesh::peer_admission_result_t::admitted);

    std::size_t selected_100 = 0;
    std::size_t selected_300 = 0;
    for (std::size_t index = 0; index < 400; ++index) {
        const auto selected = topology.select ("weighted");
        assert (selected);
        if (selected->descriptor.node_routing_id
            == bytes ("weight-100"))
            ++selected_100;
        else if (selected->descriptor.node_routing_id
                 == bytes ("weight-300"))
            ++selected_300;
        else
            assert (false);
    }
    assert (selected_100 == 100);
    assert (selected_300 == 300);

    const auto multicast =
      topology.multicast_targets ("weighted");
    assert (multicast.size () == 2);
    assert (std::count_if (
              multicast.begin (), multicast.end (),
              [] (const auto &peer) {
                  return peer.descriptor.node_routing_id
                         == bytes ("weight-100");
              })
            == 1);
    assert (std::count_if (
              multicast.begin (), multicast.end (),
              [] (const auto &peer) {
                  return peer.descriptor.node_routing_id
                         == bytes ("weight-300");
              })
            == 1);

    auto revision = weight_100;
    revision.descriptor_revision = 2;
    revision.channels.front ().weight = 0;
    assert (
      topology.admit (
        revision, bytes ("weight-connection-100"))
      == mesh::peer_admission_result_t::admitted);
    const auto after_revision =
      topology.multicast_targets ("weighted");
    assert (after_revision.size () == 1);
    assert (after_revision.front ().descriptor.node_routing_id
            == bytes ("weight-300"));

    auto invalid_negative = local;
    invalid_negative.channels.front ().weight = -1;
    bool rejected_negative = false;
    try {
        mesh::service_topology_registry_t invalid (
          invalid_negative);
    }
    catch (const std::invalid_argument &) {
        rejected_negative = true;
    }
    assert (rejected_negative);

    auto invalid_upper = local;
    invalid_upper.placement_weight = 10001;
    bool rejected_upper = false;
    try {
        mesh::service_topology_registry_t invalid (
          invalid_upper);
    }
    catch (const std::invalid_argument &) {
        rejected_upper = true;
    }
    assert (rejected_upper);

    std::vector<int> overflow_safe_weights (
      430000, 10000);
    assert (
      mesh::sum_service_weights (overflow_safe_weights)
      == 4'300'000'000ull);
}

void verify_independent_mailbox_domains_and_claim_fence ()
{
    constexpr auto fixed_work_cost = runtime::dispatch_limits::fixed_work_byte_cost;
    mesh::service_mailbox_t owner_scoped (1, 4096, 1, 4096);
    assert (owner_scoped.try_enqueue (
      {"owner-a", mesh::service_mailbox_domain_t::application,
       {{1}}}));
    auto owner_a_claim = owner_scoped.try_claim_owner (
      mesh::service_mailbox_domain_t::application, "owner-a", 1, 4096);
    assert (owner_a_claim && owner_a_claim->records.size () == 1);
    assert (!owner_scoped.try_enqueue (
      {"owner-a", mesh::service_mailbox_domain_t::application,
       {{2}}}));
    assert (owner_scoped.try_enqueue (
      {"owner-b", mesh::service_mailbox_domain_t::application,
       {{3}}}));
    assert (owner_scoped.release (*owner_a_claim));
    assert (owner_scoped.try_enqueue (
      {"owner-a", mesh::service_mailbox_domain_t::application,
       {{6}}}));

    mesh::service_mailbox_t mailbox (4, 4096, 2, 4096);
    assert (mailbox.try_enqueue (
      {"owner-a", mesh::service_mailbox_domain_t::application,
       {{1, 2, 3}}}));
    assert (mailbox.try_enqueue (
      {"owner-a", mesh::service_mailbox_domain_t::application,
       {{4, 5}}}));
    assert (mailbox.try_enqueue (
      {"peer-a", mesh::service_mailbox_domain_t::infrastructure,
       {{9}}}));
    assert (mailbox.try_enqueue (
      {"owner-b", mesh::service_mailbox_domain_t::application,
       {{7}}}));

    auto owner_b = mailbox.try_claim_owner (
      mesh::service_mailbox_domain_t::application, "owner-b", 1, 64);
    assert (owner_b && owner_b->records.size () == 1);
    assert (mailbox.release (*owner_b));

    auto application =
      mailbox.try_claim (mesh::service_mailbox_domain_t::application, 1, 64);
    assert (application && application->records.size () == 1);
    assert (!mailbox.try_claim (
      mesh::service_mailbox_domain_t::application, 1, 64));

    auto infrastructure =
      mailbox.try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 32);
    assert (infrastructure && infrastructure->records.size () == 1);
    assert (mailbox.release (*infrastructure));
    assert (!mailbox.release (*infrastructure));

    assert (mailbox.release (*application));
    auto remaining =
      mailbox.try_claim (mesh::service_mailbox_domain_t::application, 2, 64);
    assert (remaining && remaining->records.size () == 1);
    assert (mailbox.pending_messages (
              mesh::service_mailbox_domain_t::application)
            == 0);
    assert (mailbox.pending_bytes (
              mesh::service_mailbox_domain_t::application)
            == 0);
    assert (mailbox.release (*remaining));

    mesh::service_mailbox_t accounting (2, fixed_work_cost + 3, 1,
                                        fixed_work_cost + 3);
    assert (accounting.try_enqueue (
      {"accounted", mesh::service_mailbox_domain_t::application,
       {{1, 2, 3}}}));
    assert (accounting.pending_bytes (
              mesh::service_mailbox_domain_t::application)
            == fixed_work_cost + 3);
    assert (!accounting.try_enqueue (
      {"accounted", mesh::service_mailbox_domain_t::application,
       {{4}}}));
    auto accounted = accounting.try_claim (
      mesh::service_mailbox_domain_t::application, 1, 1);
    assert (accounted && accounted->records.size () == 1);
    assert (accounting.release (*accounted));

    assert (mailbox.try_enqueue (
      {"owner-large", mesh::service_mailbox_domain_t::application,
       {std::vector<std::uint8_t> (20, 7)}}));
    auto oversized =
      mailbox.try_claim (mesh::service_mailbox_domain_t::application, 1, 1);
    assert (oversized && oversized->records.size () == 1);
    assert (mailbox.release (*oversized));

    mesh::service_mailbox_t saturated (1, 1024, 1, 1024);
    assert (saturated.try_enqueue (
      {"first", mesh::service_mailbox_domain_t::application,
       {{1}}}));
    mesh::service_mailbox_record_t retained{
      "first", mesh::service_mailbox_domain_t::application,
      {{2, 3, 4}}};
    assert (!saturated.try_enqueue (std::move (retained)));
    assert (retained.owner == "first");
    assert ((retained.parts
             == std::vector<std::vector<std::uint8_t>>{{2, 3, 4}}));
    auto first = saturated.try_claim (
      mesh::service_mailbox_domain_t::application, 1, 64);
    assert (first && saturated.release (*first));
    assert (saturated.try_enqueue (std::move (retained)));
    auto second = saturated.try_claim (
      mesh::service_mailbox_domain_t::application, 1, 64);
    assert (second && second->records.size () == 1);
    assert (second->records.front ().owner == "first");
    assert ((second->records.front ().parts
             == std::vector<std::vector<std::uint8_t>>{{2, 3, 4}}));
    assert (saturated.release (*second));
}

void verify_liveness_reuses_probe_and_fences_reconnect ()
{
    mesh::service_liveness_registry_t liveness (10ms, 30ms);
    const auto node = bytes ("peer");
    const auto first_connection = bytes ("connection-a");
    const auto replacement_connection = bytes ("connection-b");
    const auto start = mesh::service_liveness_registry_t::clock_t::now ();
    liveness.admit (node, first_connection, start);

    const auto first = liveness.tick (start + 10ms);
    assert (first.probes.size () == 1);
    const auto probe_id = first.probes.front ().probe_id;
    const auto retransmit = liveness.tick (start + 20ms);
    assert (retransmit.probes.size () == 1);
    assert (retransmit.probes.front ().probe_id == probe_id);
    assert (!liveness.acknowledge (
      node, first_connection, probe_id + 1, start + 21ms));
    assert (liveness.acknowledge (
      node, first_connection, probe_id, start + 21ms));

    liveness.admit (node, replacement_connection, start + 22ms);
    assert (!liveness.disconnect (node, first_connection));
    assert (!liveness.acknowledge (
      node, first_connection, probe_id, start + 23ms));
    assert (liveness.tick (start + 53ms).timed_out_nodes
            == std::vector<std::vector<std::uint8_t>>{node});
}

void verify_location_descriptor_cas_snapshot_and_watch ()
{
    locations::service_descriptor_registry_t registry;
    std::vector<locations::service_descriptor_event_t> events;
    const auto watch = registry.watch (
      {locations::service_descriptor_kind_t::client_server, "alpha"},
      [&events] (locations::service_descriptor_event_t event) {
          events.push_back (std::move (event));
      });
    locations::service_descriptor_record_t record{
      {locations::service_descriptor_kind_t::client_server, "alpha",
       bytes ("server-a")},
      11,
      1,
      "tcp://127.0.0.1:7001",
      "security-a",
      1024 * 1024,
      mesh::service_node_state_t::serving,
      100,
      "owner-a",
      17};
    assert (registry.publish (record, std::nullopt)
            == locations::service_descriptor_publish_status_t::inserted);
    auto stale = record;
    stale.descriptor_revision = 2;
    assert (registry.publish (stale, 9)
            == locations::service_descriptor_publish_status_t::conflict);
    auto immutable_mutation = record;
    immutable_mutation.descriptor_revision = 2;
    immutable_mutation.endpoint = "tcp://127.0.0.1:7002";
    assert (registry.publish (immutable_mutation, 1)
            == locations::service_descriptor_publish_status_t::conflict);
    auto updated = record;
    updated.descriptor_revision = 2;
    updated.weight = 0;
    updated.state = mesh::service_node_state_t::draining;
    assert (registry.publish (updated, 1)
            == locations::service_descriptor_publish_status_t::updated);
    const auto snapshot = registry.snapshot (
      {locations::service_descriptor_kind_t::client_server, "alpha"});
    assert (snapshot.change_stamp == 2);
    assert (snapshot.records == std::vector{updated});
    assert (!registry.remove (
      updated.key, 1, updated.owner_id, updated.owner_lease_generation));
    assert (registry.remove (
      updated.key, 2, updated.owner_id, updated.owner_lease_generation));
    assert (events.size () == 3);
    assert (events.back ().change
            == locations::service_descriptor_change_t::removed);
    assert (events.back ().change_stamp == 3);
    assert (registry.unwatch (watch));
    assert (!registry.unwatch (watch));
}

void verify_manual_and_automatic_classic_fanout ()
{
    fanout::raw_fanout_publisher_t publisher ("tcp://127.0.0.1:0");
    publisher.start ();
    fanout::raw_fanout_subscriber_t manual;
    const auto publisher_id = bytes ("publisher-a");
    assert (manual.connect_manual (publisher_id, publisher.endpoint ()));

    auto receive_now = std::chrono::steady_clock::now ();
    bool beacon_received = false;
    const auto deadline = receive_now + 2s;
    std::size_t beacon_tick = 1;
    while (!beacon_received && std::chrono::steady_clock::now () < deadline) {
        (void) publisher.tick (
          receive_now
          + fanout::fanout_beacon_interval
              * static_cast<int> (beacon_tick++));
        const auto [status, received] = manual.try_receive (receive_now);
        static_cast<void> (received);
        beacon_received = status == fanout::fanout_receive_status_t::beacon;
        if (!beacon_received) {
            std::this_thread::sleep_for (2ms);
        }
    }
    assert (beacon_received);
    assert (manual.ready (publisher_id));
    bool application_received = false;
    for (std::size_t attempt = 0; attempt < 100 && !application_received;
         ++attempt) {
        assert (publisher.publish (
          "topic-a",
          {"FanoutProbe", "application/json", bytes ("fanout")}));
        const auto [status, received] = manual.try_receive (receive_now);
        if (status == fanout::fanout_receive_status_t::application) {
            assert (received);
            assert (received->publisher_routing_id == publisher_id);
            assert (received->topic == "topic-a");
            const protocol::application_payload_t expected{
              "FanoutProbe", "application/json", bytes ("fanout")};
            assert (received->payload == expected);
            application_received = true;
        } else {
            std::this_thread::sleep_for (2ms);
        }
    }
    assert (application_received);
    assert (manual.tick (
              receive_now + fanout::fanout_receive_deadline)
            == std::vector<std::vector<std::uint8_t>>{publisher_id});
    assert (!manual.ready (publisher_id));

    fanout::raw_fanout_subscriber_t automatic;
    fanout::fanout_publisher_intent_t automatic_descriptor{
      publisher_id,
      1,
      publisher.endpoint (),
      mesh::service_node_state_t::serving};
    automatic.reconcile_automatic ({automatic_descriptor});
    assert (automatic.publisher_count () == 1);
    bool automatic_ready = false;
    for (std::size_t attempt = 0; attempt < 100 && !automatic_ready;
         ++attempt) {
        (void) publisher.tick (
          receive_now
          + fanout::fanout_beacon_interval
              * static_cast<int> (beacon_tick++));
        const auto [status, received] =
          automatic.try_receive (receive_now);
        static_cast<void> (received);
        automatic_ready =
          status == fanout::fanout_receive_status_t::beacon;
        if (!automatic_ready) {
            std::this_thread::sleep_for (2ms);
        }
    }
    assert (automatic_ready);
    assert (automatic.ready (publisher_id));

    automatic_descriptor.lifecycle_generation = 2;
    automatic.reconcile_automatic ({automatic_descriptor});
    assert (automatic.publisher_count () == 1);
    assert (!automatic.ready (publisher_id));

    automatic_descriptor.state = mesh::service_node_state_t::draining;
    automatic.reconcile_automatic ({automatic_descriptor});
    assert (automatic.publisher_count () == 0);

    bool reserved_rejected = false;
    try {
        static_cast<void> (publisher.publish (
          fanout::raw_fanout_publisher_t::reserved_topic (),
          {"Reserved", "application/json", {}}));
    }
    catch (const std::invalid_argument &) {
        reserved_rejected = true;
    }
    assert (reserved_rejected);
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
      {{server_descriptor}});
    server.start ();
    auto expected_server = server.descriptor ();
    auto manual_server = expected_server;
    manual_server.server_routing_id.clear ();
    manual_server.lifecycle_generation = 0;
    manual_server.descriptor_revision = 0;
    client_server::raw_client_server_client_options_t client_options{
      bytes ("client-a"),
      {expected_server.channel_name,
       "security-a",
       16u * 1024u * 1024u},
      std::move (manual_server)};
    client_server::raw_client_server_client_t client (
      std::move (client_options));
    client.start ();
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (!client.ready () && std::chrono::steady_clock::now () < deadline) {
        const auto now = std::chrono::steady_clock::now ();
        (void) server.drain_monitor_events (now);
        (void) client.drain_monitor_events (now);
        const auto server_pump = server.pump_one (now);
        const auto client_pump = client.pump_one (now);
        assert (
          server_pump
          != client_server::client_server_pump_result_t::protocol_error);
        assert (
          client_pump
          != client_server::client_server_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (client.ready ());

    const auto liveness_base = std::chrono::steady_clock::now ();
    const auto first_probe =
      client.tick_liveness (liveness_base + 5s);
    assert (first_probe.probes.size () == 1);
    client_server::client_server_pump_result_t probe_pump =
      client_server::client_server_pump_result_t::no_data;
    while (probe_pump
             == client_server::client_server_pump_result_t::no_data
           && std::chrono::steady_clock::now () < deadline) {
        probe_pump = server.pump_one (std::chrono::steady_clock::now ());
    }
    assert (probe_pump
            == client_server::client_server_pump_result_t::infrastructure);
    client_server::client_server_pump_result_t ack_pump =
      client_server::client_server_pump_result_t::no_data;
    while (ack_pump
             == client_server::client_server_pump_result_t::no_data
           && std::chrono::steady_clock::now () < deadline) {
        ack_pump = client.pump_one (std::chrono::steady_clock::now ());
    }
    assert (ack_pump
            == client_server::client_server_pump_result_t::infrastructure);
    const auto next_probe =
      client.tick_liveness (liveness_base + 10s);
    assert (next_probe.probes.size () == 1);
    assert (next_probe.probes.front ().probe_id
            != first_probe.probes.front ().probe_id);

    assert (client.send (
      {"ClientServerSend", "application/json", bytes ("send")}));
    client_server::client_server_pump_result_t send_pump =
      client_server::client_server_pump_result_t::no_data;
    while (send_pump
             != client_server::client_server_pump_result_t::application
           && std::chrono::steady_clock::now () < deadline) {
        send_pump = server.pump_one (std::chrono::steady_clock::now ());
    }
    assert (send_pump
            == client_server::client_server_pump_result_t::application);
    auto send_claim = server.mailbox ().try_claim (
      mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (send_claim && send_claim->records.size () == 1);
    assert (protocol::decode_channel_send_header (
              send_claim->records.front ().parts.front ())
            == expected_server.channel_name);
    assert (server.mailbox ().release (*send_claim));

    using request_result_t =
      client_server::client_server_request_completion_t;
    std::promise<request_result_t> promise;
    auto future = promise.get_future ();
    assert (client.request (
      {"ClientServerRequest", "application/json", bytes ("request")},
      2s,
      [&promise] (request_result_t completion) {
          promise.set_value (std::move (completion));
      }));
    client_server::client_server_pump_result_t request_pump =
      client_server::client_server_pump_result_t::no_data;
    while (request_pump
             != client_server::client_server_pump_result_t::application
           && std::chrono::steady_clock::now () < deadline) {
        request_pump = server.pump_one (std::chrono::steady_clock::now ());
    }
    assert (request_pump
            == client_server::client_server_pump_result_t::application);
    auto request_claim = server.mailbox ().try_claim (
      mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (request_claim && request_claim->records.size () == 1);
    assert (request_claim->records.front ().request_sequence);
    assert (request_claim->records.front ().correlation);
    assert (server.reply (
      request_claim->records.front (),
      {"ClientServerReply", "application/json", bytes ("reply")}));
    assert (server.mailbox ().release (*request_claim));
    while (future.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < deadline) {
        const auto pump = client.pump_one (std::chrono::steady_clock::now ());
        assert (
          pump != client_server::client_server_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (future.wait_for (0ms) == std::future_status::ready);
    const auto result = future.get ();
    assert (result.terminal
            == foundation::operation_terminal_t::completed);
    assert (result.reply_header.terminal_result == 0);
    assert (result.reply_header.failure_code == 0);
    const protocol::application_payload_t expected_reply{
      "ClientServerReply", "application/json", bytes ("reply")};
    assert (protocol::decode_application_payload (result.payload)
            == expected_reply);

    std::promise<request_result_t> rejected_promise;
    auto rejected_future = rejected_promise.get_future ();
    assert (client.request (
      {"RejectedRequest", "application/json", bytes ("request")},
      2s,
      [&rejected_promise] (request_result_t completion) {
          rejected_promise.set_value (std::move (completion));
      }));
    request_pump =
      client_server::client_server_pump_result_t::no_data;
    while (request_pump
             != client_server::client_server_pump_result_t::application
           && std::chrono::steady_clock::now () < deadline) {
        request_pump = server.pump_one (
          std::chrono::steady_clock::now ());
    }
    assert (request_pump
            == client_server::client_server_pump_result_t::application);
    request_claim = server.mailbox ().try_claim (
      mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (request_claim && request_claim->records.size () == 1);
    assert (server.reply (
      request_claim->records.front (), 106,
      protocol::framework_error_code::requestRejected));
    assert (server.mailbox ().release (*request_claim));
    while (rejected_future.wait_for (0ms)
             != std::future_status::ready
           && std::chrono::steady_clock::now () < deadline) {
        const auto pump = client.pump_one (
          std::chrono::steady_clock::now ());
        assert (
          pump != client_server::client_server_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (rejected_future.wait_for (0ms)
            == std::future_status::ready);
    const auto rejected = rejected_future.get ();
    assert (rejected.terminal
            == foundation::operation_terminal_t::completed);
    assert (rejected.reply_header.terminal_result == 106);
    assert (
      rejected.reply_header.failure_code
      == static_cast<std::uint32_t> (
        protocol::framework_error_code::requestRejected));
    assert (rejected.payload.empty ());

    // The advertised ClientServer limit must remain usable for a frame that
    // is larger than the core automatic HWM default.
    const auto large_payload = std::vector<std::uint8_t> (
      1024u * 1024u, static_cast<std::uint8_t> ('p'));
    std::promise<request_result_t> large_promise;
    auto large_future = large_promise.get_future ();
    assert (client.request (
      {"LargePayloadRequest", "application/json", large_payload},
      3s,
      [&large_promise] (request_result_t completion) {
          large_promise.set_value (std::move (completion));
      }));
    const auto large_deadline = std::chrono::steady_clock::now () + 5s;
    auto large_claim = server.mailbox ().try_claim (
      mesh::service_mailbox_domain_t::application, 1, 2u * 1024u * 1024u);
    while (!large_claim
           && std::chrono::steady_clock::now () < large_deadline) {
        const auto pump = server.pump_one (
          std::chrono::steady_clock::now ());
        assert (
          pump != client_server::client_server_pump_result_t::protocol_error);
        large_claim = server.mailbox ().try_claim (
          mesh::service_mailbox_domain_t::application,
          1,
          2u * 1024u * 1024u);
        if (!large_claim)
            std::this_thread::sleep_for (1ms);
    }
    assert (large_claim && large_claim->records.size () == 1);
    const auto large_reply = bytes ("large payload accepted");
    assert (server.reply (
      large_claim->records.front (),
      {"LargePayloadReply", "application/json", large_reply}));
    assert (server.mailbox ().release (*large_claim));
    while (large_future.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < large_deadline) {
        const auto pump = client.pump_one (std::chrono::steady_clock::now ());
        assert (
          pump != client_server::client_server_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (large_future.wait_for (0ms) == std::future_status::ready);
    const auto large_result = large_future.get ();
    assert (large_result.terminal
            == foundation::operation_terminal_t::completed);
    assert (large_result.reply_header.terminal_result == 0);
    const protocol::application_payload_t expected_large_reply{
      "LargePayloadReply", "application/json", large_reply};
    assert (protocol::decode_application_payload (large_result.payload)
            == expected_large_reply);
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
      {{server_descriptor}});
    server.start ();
    const auto expected_server = server.descriptor ();

    std::vector<std::unique_ptr<client_server::raw_client_server_client_t>>
      clients;
    for (const auto &client_id : {"client-monitor-a", "client-monitor-b"}) {
        clients.push_back (
          std::make_unique<client_server::raw_client_server_client_t> (
            client_server::raw_client_server_client_options_t{
              bytes (client_id),
              {expected_server.channel_name,
               expected_server.security_identity,
               expected_server.effective_max_message_bytes},
              expected_server}));
        clients.back ()->start ();
    }

    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (std::chrono::steady_clock::now () < deadline) {
        const auto now = std::chrono::steady_clock::now ();
        for (auto &client : clients) {
            (void) client->drain_monitor_events (now);
            const auto client_pump = client->pump_one (now);
            assert (
              client_pump
              != client_server::client_server_pump_result_t::protocol_error);
        }

        // The server monitor queue is deliberately not drained here. A
        // received hello already identifies the route that must receive the
        // admission response.
        const auto server_pump = server.pump_one (now);
        assert (
          server_pump
          != client_server::client_server_pump_result_t::protocol_error);

        bool all_ready = true;
        for (auto &client : clients) {
            (void) client->pump_one (now);
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

    const std::vector<client_server::weighted_candidate_t>
      after_api_b_enters_draining{
      {"api-a", 300}};
    for (std::size_t index = 0; index < 32; ++index) {
        const auto key = selector.select (
          after_api_b_enters_draining);
        assert (key && *key == "api-a");
        assert (selector.state_size () == 1);
        assert (selector.maximum_absolute_credit () <= 300);
    }

    const std::vector<client_server::weighted_candidate_t> none{
      {"disabled", 0}};
    assert (!selector.select (none));
    assert (selector.state_size () == 0);

    client_server::smooth_weighted_selector_t rid_tiebreak;
    const std::vector<client_server::weighted_candidate_t> tied{
      {"connection-z", 100, "rid-a"},
      {"connection-a", 100, "rid-b"}};
    const auto tie_selected = rid_tiebreak.select (tied);
    assert (tie_selected && *tie_selected == "connection-z");

    mesh::service_topology_registry_t topology (descriptor ("route-local"));
    auto route_a = descriptor ("route-a");
    auto route_b = descriptor ("route-b");
    route_a.state = mesh::service_node_state_t::serving;
    route_b.state = mesh::service_node_state_t::serving;
    assert (topology.admit (route_a, bytes ("route-connection-a"))
            == mesh::peer_admission_result_t::admitted);
    assert (topology.admit (route_b, bytes ("route-connection-b"))
            == mesh::peer_admission_result_t::admitted);
    const std::vector<std::vector<std::uint8_t>> expected_route_ids{
      bytes ("route-a"), bytes ("route-b"), bytes ("route-a"),
      bytes ("route-b")};
    for (const auto &expected : expected_route_ids) {
        const auto selected_route = topology.select ("alpha");
        assert (selected_route
                && selected_route->descriptor.node_routing_id == expected);
    }

    auto weighted_route_a = descriptor ("weighted-route-a");
    auto weighted_route_b = descriptor ("weighted-route-b");
    weighted_route_a.channels.front ().weight = 300;
    weighted_route_b.channels.front ().weight = 100;
    weighted_route_a.state = mesh::service_node_state_t::serving;
    weighted_route_b.state = mesh::service_node_state_t::serving;
    mesh::service_topology_registry_t weighted_topology (
      descriptor ("weighted-route-local"));
    assert (weighted_topology.admit (
              weighted_route_a, bytes ("weighted-route-connection-a"))
            == mesh::peer_admission_result_t::admitted);
    assert (weighted_topology.admit (
              weighted_route_b, bytes ("weighted-route-connection-b"))
            == mesh::peer_admission_result_t::admitted);
    std::map<std::vector<std::uint8_t>, std::size_t> weighted_selected;
    for (std::size_t index = 0; index < 400; ++index) {
        const auto selected_route = weighted_topology.select ("alpha");
        assert (selected_route);
        ++weighted_selected[selected_route->descriptor.node_routing_id];
    }
    assert (weighted_selected[bytes ("weighted-route-a")] == 300);
    assert (weighted_selected[bytes ("weighted-route-b")] == 100);

    weighted_route_b.descriptor_revision = 2;
    weighted_route_b.state = mesh::service_node_state_t::retiring;
    assert (weighted_topology.admit (
              weighted_route_b, bytes ("weighted-route-connection-b"))
            == mesh::peer_admission_result_t::admitted);
    for (std::size_t index = 0; index < 32; ++index) {
        const auto selected_route = weighted_topology.select ("alpha");
        assert (selected_route
                && selected_route->descriptor.node_routing_id
                     == bytes ("weighted-route-a"));
    }
}

void verify_raw_owner_node_send_and_liveness ()
{
    mesh::raw_mesh_node_owner_t first (
      mesh::raw_mesh_node_options_t{descriptor ("raw-a")});
    mesh::raw_mesh_node_owner_t second (
      mesh::raw_mesh_node_options_t{
        descriptor ("raw-b"),
        1,
        16u * 1024u * 1024u,
        1024,
        4u * 1024u * 1024u});
    assert (first.topology ().local_descriptor ().state
            == mesh::service_node_state_t::preparing);
    assert (second.topology ().local_descriptor ().state
            == mesh::service_node_state_t::preparing);
    first.start ();
    second.start ();
    assert (first.started () && second.started ());
    assert (first.topology ().local_descriptor ().state
            == mesh::service_node_state_t::serving);
    assert (second.topology ().local_descriptor ().state
            == mesh::service_node_state_t::serving);

    auto first_descriptor = first.topology ().local_descriptor ();
    auto second_descriptor = second.topology ().local_descriptor ();
    // The higher RID does not dial, but it retains discovery data to
    // validate the inbound endpoint and security identity.
    second.expect_peer (first_descriptor);
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    const auto deadline = now + 5s;
    assert (first.connect_peer (second.endpoint (), second_descriptor));
    while ((!first.topology ().peer (second_descriptor.node_routing_id)
            || !second.topology ().peer (first_descriptor.node_routing_id))
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto progress_now =
          mesh::service_liveness_registry_t::clock_t::now ();
        (void) first.drain_monitor_events (progress_now);
        (void) second.drain_monitor_events (progress_now);
        (void) first.pump_one (progress_now);
        (void) second.pump_one (progress_now);
        std::this_thread::sleep_for (1ms);
    }
    assert (first.topology ().peer (second_descriptor.node_routing_id));
    assert (second.topology ().peer (first_descriptor.node_routing_id));

    bool submitted = false;
    while (!submitted && mesh::service_liveness_registry_t::clock_t::now ()
                           < deadline) {
        try {
            submitted = first.send_to_node (
              second_descriptor.node_routing_id,
              {"Probe", "application/json", bytes ("payload")});
        }
        catch (...) {
        }
        if (!submitted) {
            std::this_thread::sleep_for (5ms);
        }
    }
    assert (submitted);

    mesh::raw_mesh_pump_result_t pumped =
      mesh::raw_mesh_pump_result_t::no_data;
    while (pumped != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        pumped = second.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ());
        assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        if (pumped != mesh::raw_mesh_pump_result_t::application) {
            std::this_thread::sleep_for (2ms);
        }
    }
    assert (pumped == mesh::raw_mesh_pump_result_t::application);

    bool retained_submitted = false;
    while (!retained_submitted
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        retained_submitted = first.send_to_node (
          second_descriptor.node_routing_id,
          {"Probe", "application/json", bytes ("retained")});
        if (!retained_submitted)
            std::this_thread::sleep_for (1ms);
    }
    assert (retained_submitted);
    mesh::raw_mesh_pump_result_t retained_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (retained_pump == mesh::raw_mesh_pump_result_t::no_data
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        retained_pump = second.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ());
    }
    assert (retained_pump
            == mesh::raw_mesh_pump_result_t::backpressured);

    // A full application mailbox retains the second payload and pauses
    // application Recv. Liveness still crosses the existing Completion
    // connection and must be processed before that retained payload.
    const auto paused_liveness_base =
      mesh::service_liveness_registry_t::clock_t::now ();
    const auto paused_probe =
      first.tick_liveness (paused_liveness_base + 5s);
    assert (paused_probe.probes.size () == 1);
    mesh::raw_mesh_pump_result_t paused_probe_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (paused_probe_pump
             != mesh::raw_mesh_pump_result_t::infrastructure
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        paused_probe_pump = second.pump_one (
          mesh::service_liveness_registry_t::clock_t::now (),
          false);
        assert (paused_probe_pump
                != mesh::raw_mesh_pump_result_t::protocol_error);
        if (paused_probe_pump
            != mesh::raw_mesh_pump_result_t::infrastructure)
            std::this_thread::sleep_for (1ms);
    }
    assert (paused_probe_pump
            == mesh::raw_mesh_pump_result_t::infrastructure);
    mesh::raw_mesh_pump_result_t paused_ack_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (paused_ack_pump
             != mesh::raw_mesh_pump_result_t::infrastructure
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        paused_ack_pump = first.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ());
        assert (paused_ack_pump
                != mesh::raw_mesh_pump_result_t::protocol_error);
        if (paused_ack_pump
            != mesh::raw_mesh_pump_result_t::infrastructure)
            std::this_thread::sleep_for (1ms);
    }
    assert (paused_ack_pump
            == mesh::raw_mesh_pump_result_t::infrastructure);

    auto claim = second.mailbox ().try_claim (
      mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (claim && claim->records.size () == 1);
    assert (protocol::decode_header (
              claim->records.front ().parts.front ())
              .kind
            == protocol::command::nodeSend);
    const protocol::application_payload_t expected_payload{
      "Probe", "application/json", bytes ("payload")};
    assert (protocol::decode_application_payload (
              claim->records.front ().parts.at (1))
            == expected_payload);
    assert (second.mailbox ().release (*claim));

    assert (second.pump_one (
              mesh::service_liveness_registry_t::clock_t::now ())
            == mesh::raw_mesh_pump_result_t::application);
    auto retained_claim = second.mailbox ().try_claim (
      mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (retained_claim && retained_claim->records.size () == 1);
    const protocol::application_payload_t retained_payload{
      "Probe", "application/json", bytes ("retained")};
    assert (protocol::decode_application_payload (
              retained_claim->records.front ().parts.at (1))
            == retained_payload);
    assert (second.mailbox ().release (*retained_claim));

    bool channel_submitted = false;
    while (!channel_submitted
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        try {
            channel_submitted = first.send_to_channel (
              "alpha", {"ChannelProbe", "application/json", bytes ("channel")});
        }
        catch (...) {
        }
    }
    assert (channel_submitted);
    mesh::raw_mesh_pump_result_t channel_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (channel_pump != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        channel_pump = second.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ());
        assert (channel_pump != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (channel_pump == mesh::raw_mesh_pump_result_t::application);
    auto channel_claim = second.mailbox ().try_claim (
      mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (channel_claim && channel_claim->owner == "channel:alpha");
    assert (protocol::decode_channel_send_header (
              channel_claim->records.front ().parts.front ())
            == "alpha");
    assert (second.mailbox ().release (*channel_claim));

    using request_result_t =
      std::pair<foundation::operation_terminal_t,
                std::vector<std::uint8_t>>;
    std::promise<request_result_t> request_promise;
    auto request_future = request_promise.get_future ();
    assert (first.request_to_node (
      second_descriptor.node_routing_id,
      {"RequestProbe", "application/json", bytes ("request")},
      2s,
      [&request_promise] (
        foundation::operation_terminal_t terminal,
        std::vector<std::uint8_t> payload) mutable {
          request_promise.set_value (
            {terminal, std::move (payload)});
      }));
    mesh::raw_mesh_pump_result_t request_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (request_pump != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        request_pump = second.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ());
        assert (request_pump != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (request_pump == mesh::raw_mesh_pump_result_t::application);
    auto request_claim = second.mailbox ().try_claim (
      mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (request_claim && request_claim->records.size () == 1);
    const auto &request_record = request_claim->records.front ();
    assert (request_record.request_sequence);
    assert (request_record.correlation);
    assert (protocol::decode_node_request_header (
              request_record.parts.front ())
            == *request_record.correlation);
    assert (second.reply (
      request_record,
      {"RequestReply", "application/json", bytes ("reply")}));
    assert (second.mailbox ().release (*request_claim));
    const auto request_deadline =
      mesh::service_liveness_registry_t::clock_t::now () + 2s;
    while (request_future.wait_for (0ms) != std::future_status::ready
           && mesh::service_liveness_registry_t::clock_t::now ()
                < request_deadline) {
        const auto client_pump = first.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ());
        assert (client_pump != mesh::raw_mesh_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (request_future.wait_for (0ms) == std::future_status::ready);
    auto request_result = request_future.get ();
    assert (request_result.first
            == foundation::operation_terminal_t::completed);
    const protocol::application_payload_t expected_reply{
      "RequestReply", "application/json", bytes ("reply")};
    assert (protocol::decode_application_payload (request_result.second)
            == expected_reply);

    std::promise<request_result_t> actor_create_promise;
    auto actor_create_future = actor_create_promise.get_future ();
    protocol::actor_create_header_t actor_create{
      0,
      {first_descriptor.lifecycle_generation, 91},
      first_descriptor.node_routing_id,
      first_descriptor.lifecycle_generation,
      "actor-remote",
      "player",
      {"reservation-remote", "store-remote", 1, 1,
       second_descriptor.node_routing_id,
       second_descriptor.lifecycle_generation,
       "owner-remote", 1, 1},
      static_cast<std::uint64_t> (
        std::chrono::duration_cast<std::chrono::milliseconds> (
          std::chrono::system_clock::now ().time_since_epoch () + 2s)
          .count ())};
    assert (first.request_actor_create (
      second_descriptor.node_routing_id, actor_create, 2s,
      [&actor_create_promise] (
        foundation::operation_terminal_t terminal,
        std::vector<std::uint8_t> payload) mutable {
          actor_create_promise.set_value (
            {terminal, std::move (payload)});
      }));
    mesh::raw_mesh_pump_result_t actor_create_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (actor_create_pump
             != mesh::raw_mesh_pump_result_t::infrastructure
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        actor_create_pump = second.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ());
        assert (actor_create_pump
                != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (actor_create_pump
            == mesh::raw_mesh_pump_result_t::infrastructure);
    auto actor_create_claim = second.mailbox ().try_claim (
      mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
    assert (actor_create_claim
            && actor_create_claim->records.size () == 1);
    const auto &actor_create_record =
      actor_create_claim->records.front ();
    const auto decoded_actor_create =
      protocol::decode_actor_create_header (
        actor_create_record.parts.front ());
    assert (decoded_actor_create.operation == actor_create.operation);
    assert (decoded_actor_create.actor_id == "actor-remote");
    protocol::actor_create_reply_t actor_create_reply{
      {*actor_create_record.correlation, 0, 0},
      protocol::actor_create_result_t::created,
      second_descriptor.node_routing_id,
      "actor-remote",
      1};
    assert (second.reply_actor_create (
      actor_create_record, actor_create_reply));
    assert (second.mailbox ().release (*actor_create_claim));
    while (actor_create_future.wait_for (0ms)
             != std::future_status::ready
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        const auto pump = first.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ());
        assert (pump != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (actor_create_future.wait_for (0ms)
            == std::future_status::ready);
    const auto actor_create_result = actor_create_future.get ();
    assert (actor_create_result.first
            == foundation::operation_terminal_t::completed);
    assert (!actor_create_result.second.empty ());

    // The paused probe advanced the registry's logical next-probe time by
    // one interval. Continue from that same logical clock instead of mixing
    // the synthetic probe time with the wall clock used by the pump loop.
    const auto liveness_base = paused_liveness_base + 5s;
    const auto first_probe = first.tick_liveness (liveness_base + 5s);
    assert (first_probe.probes.size () == 1);
    mesh::raw_mesh_pump_result_t probe_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (probe_pump == mesh::raw_mesh_pump_result_t::no_data
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        probe_pump = second.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ());
    }
    assert (probe_pump == mesh::raw_mesh_pump_result_t::infrastructure);

    mesh::raw_mesh_pump_result_t ack_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (ack_pump == mesh::raw_mesh_pump_result_t::no_data
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        ack_pump = first.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ());
    }
    assert (ack_pump == mesh::raw_mesh_pump_result_t::infrastructure);
    const auto next_probe = first.tick_liveness (liveness_base + 10s);
    assert (next_probe.probes.size () == 1);
    assert (next_probe.probes.front ().probe_id
            != first_probe.probes.front ().probe_id);

    first.close ();
    second.close ();
}

} // namespace

int main ()
{
    verify_host_wide_application_byte_budget ();
    verify_host_wide_budget_waits_for_terminal_completion ();
    verify_host_wide_completion_send_permits ();
    verify_actor_create_command_49_roundtrip ();
    verify_topology_snapshot_and_connection_fence ();
    verify_duplicate_connection_survivor_is_symmetric ();
    verify_lifecycle_token_requires_current_discovery_expectation ();
    verify_physical_candidates_preserve_survivor ();
    verify_bilateral_raw_connection_without_public_pipe_id_keeps_survivor ();
    verify_raw_admission_rejects_lifecycle_mismatch ();
    verify_object_client_connection_requirement ();
    verify_manual_object_client_pair_ends_not_required ();
    verify_signed_weight_contract ();
    verify_independent_mailbox_domains_and_claim_fence ();
    verify_liveness_reuses_probe_and_fences_reconnect ();
    verify_location_descriptor_cas_snapshot_and_watch ();
    verify_manual_and_automatic_classic_fanout ();
    verify_client_server_independent_raw_path ();
    verify_client_server_admits_before_monitor_drain ();
    verify_client_server_weighted_selection ();
    verify_raw_owner_node_send_and_liveness ();
    return 0;
}
