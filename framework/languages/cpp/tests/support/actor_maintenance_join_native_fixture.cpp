/* SPDX-License-Identifier: Elastic-2.0 */

#include "actor_maintenance_join_native_fixture.hpp"
#include "actor_maintenance_join_packets.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/host/runtime_failure_collector.hpp"
#include <gtest/gtest.h>
#include <thread>

namespace zlink::framework::test::actor_maintenance_join
{
relay_peer_pair_t::relay_peer_pair_t (
  fw::runtime::mesh::service_object_role_t source_role,
  fw::runtime::mesh::service_object_role_t target_role,
  fw::runtime::stateful::accepted_record_authority_resolver_t authority)
{
    source_store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto make = [] (const char *rid, std::uint64_t generation,
                          fw::runtime::mesh::service_object_role_t role,
                          std::future<void> &destroyed) {
        fw::runtime::host::host_options_t options;
        options.mesh.descriptor.mesh_name = mesh_name;
        options.mesh.descriptor.node_routing_id = zlink::routing_id_t::from (rid).to_bytes ();
        options.mesh.descriptor.lifecycle_generation = generation;
        options.mesh.descriptor.descriptor_revision = 1;
        options.mesh.descriptor.advertised_endpoint = "tcp://127.0.0.1:0";
        options.mesh.descriptor.object_role = role;
        options.mesh.runtime_failures =
          std::make_shared<fw::runtime::runtime_failure_collector_t> ();
        auto completion = std::make_shared<std::promise<void>> ();
        destroyed = completion->get_future ();
        auto host = std::shared_ptr<host_type> (new host_type (std::move (options)),
                                                [completion] (host_type *value) {
                                                    delete value;
                                                    completion->set_value ();
                                                });
        // An object node runs under an auto-connect owner (MeshNode 7.1).
        if (role != fw::runtime::mesh::service_object_role_t::none)
            host->configure_peer_reconcile ([] (const std::vector<std::uint8_t> &) {});
        return host;
    };
    source = make ("relay-source", 7, source_role, hosts_destroyed[0]);
    target = make ("relay-target", 11, target_role, hosts_destroyed[1]);
    if (authority)
        target->configure_stateful_dispatch (std::move (authority), &source_reader);
    else
        target->configure_stateful_dispatch ([token = lease] { return token; }, &source_reader);
    source->start ();
    target->start ();
    source_wire = source->transport ().topology ().local_descriptor ();
    target_wire = target->transport ().topology ().local_descriptor ();
    original = discovery (source_wire, "source-owner");
    (void) source_store.claim_owner_lease (original->owner_id, 15s).result ().value ();
    (void) source_store.update_mesh_node (*original, fw::location_write_intent_t::new_claim)
      .result ()
      .value ();
    (void) source_reader.poll_mesh_sources (mesh_name).result ().value ();
    source->transport ().connect_peer (target_wire.advertised_endpoint, target_wire);
}

relay_peer_pair_t::~relay_peer_pair_t ()
{
    target->close ();
    source->close ();
    target.reset ();
    source.reset ();
    // A task publishes its result before final_suspend destroys the frame.
    // Its last host reference and the Core context can still belong to that frame.
    for (auto &destroyed : hosts_destroyed)
        destroyed.get ();
}

std::shared_ptr<const fw::mesh_node_descriptor_t>
relay_peer_pair_t::discovery (const descriptor_type &wire, const char *owner)
{
    return std::make_shared<const fw::mesh_node_descriptor_t> (
      fw::mesh_node_descriptor_t{.mesh_name = wire.mesh_name,
                                 .rid = zlink::routing_id_t::from (wire.node_routing_id),
                                 .lifecycle_generation = wire.lifecycle_generation,
                                 .descriptor_revision = 1,
                                 .endpoint = wire.advertised_endpoint,
                                 .state = fw::framework_runtime_state_t::serving,
                                 .security_identity = "test",
                                 .owner_id = owner,
                                 .lease_generation = 1});
}

std::optional<fw::runtime::mesh::request_source_t>
relay_peer_pair_t::accepted_source (std::uint64_t generation)
{
    auto descriptor =
      source_reader.request_source (mesh_name, source_wire.node_routing_id, generation)
        .result ()
        .value ();
    return descriptor ? std::optional<fw::runtime::mesh::request_source_t>{std::move (descriptor)}
                      : std::nullopt;
}

void relay_peer_pair_t::pump ()
{
    const auto discard = [] (const auto &, const auto &, auto, auto) {};
    source->dispatch_ready (discard).result ().value ();
    target->dispatch_ready (discard).result ().value ();
}

bool relay_peer_pair_t::ready ()
{
    const auto peer = target->transport ().topology ().peer (source_wire.node_routing_id);
    const auto other = source->transport ().topology ().peer (target_wire.node_routing_id);
    return peer && peer->ready && other && other->ready;
}

bool relay_peer_pair_t::await_ready ()
{
    const auto deadline = std::chrono::steady_clock::now () + wait_budget;
    while (!ready () && std::chrono::steady_clock::now () < deadline) {
        pump ();
        std::this_thread::sleep_for (1ms);
    }
    return ready ();
}

native_intake_fixture_t::native_intake_fixture_t (
  fw::dispatch_options_t dispatch,
  fw::runtime::stateful::accepted_record_authority_resolver_t authority)
{
    source_store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    fw::runtime::host::host_options_t options;
    options.core_context = core_context;
    options.mesh.descriptor = {mesh_name,
                               rid,
                               1,
                               1,
                               "tcp://127.0.0.1:0",
                               {},
                               fw::runtime::mesh::service_node_state_t::preparing};
    options.mesh.runtime_failures = std::make_shared<fw::runtime::runtime_failure_collector_t> ();
    options.mesh.dispatch = std::move (dispatch);
    options.object_stable_types = {actor_type, "framework.spot"};
    host = std::make_shared<fw::runtime::host::public_host_runtime_t> (std::move (options));
    const auto lease =
      std::make_shared<const fw::location_owner_token_t> (fw::location_owner_token_t{source_id, 1});
    resolver = [host = host, lease, sources = &source_reader] (
                 fw::runtime::stateful::accepted_record_authority_query_t query) {
        return host->accepted_record_authority (std::move (query), lease, sources);
    };
    if (authority)
        host->configure_stateful_dispatch (std::move (authority), &source_reader);
    else
        host->configure_stateful_dispatch ([lease] { return lease; }, &source_reader);
    auto peer = host->transport ().topology ().local_descriptor ();
    peer.node_routing_id = zlink::routing_id_t::from (source_id).to_bytes ();
    peer.advertised_endpoint = "tcp://127.0.0.1:1";
    auto original = std::make_shared<const fw::mesh_node_descriptor_t> (
      fw::mesh_node_descriptor_t{.mesh_name = peer.mesh_name,
                                 .rid = zlink::routing_id_t::from (peer.node_routing_id),
                                 .lifecycle_generation = peer.lifecycle_generation,
                                 .descriptor_revision = 1,
                                 .endpoint = peer.advertised_endpoint,
                                 .state = fw::framework_runtime_state_t::serving,
                                 .security_identity = "test",
                                 .owner_id = source_id,
                                 .lease_generation = 1});
    (void) source_store.claim_owner_lease (source_id, 15s).result ().value ();
    (void) source_store.update_mesh_node (*original, fw::location_write_intent_t::new_claim)
      .result ()
      .value ();
    (void) source_reader.poll_mesh_sources (mesh_name).result ().value ();
    host->transport ().expect_peer (peer);
    EXPECT_EQ (host->transport ().topology ().admit (peer, {1}, std::chrono::steady_clock::now (),
                                                     fw::runtime::protocol::command::admit),
               fw::runtime::mesh::peer_admission_result_t::admitted);
    host->start ();
}

native_intake_fixture_t::~native_intake_fixture_t ()
{
    host->close ();
    // Result publication can precede release of the dispatch coroutine's
    // Host reference. The fixture owns Core teardown through that release.
    core_context->term ();
}

fw::runtime::mesh::service_mailbox_record_t
native_intake_fixture_t::actor_record (std::uint64_t sequence,
                                       std::uint64_t authority_generation) const
{
    namespace wire = fw::runtime::protocol;
    namespace mesh = fw::runtime::mesh;
    auto source = zlink::routing_id_t::from (source_id).to_bytes ();
    const wire::actor_route_fence_t fence{actor_id, 1, rid, 1, authority_generation, 1};
    fw::runtime::messaging::envelope_header_t envelope;
    envelope.kind = fw::runtime::messaging::message_kind_t::command;
    envelope.channel_name = mesh_name;
    envelope.message_name = probe_t::packet_name;
    auto parts = fw::detail::encode_actor_packet (envelope, zlink::message_t{});
    auto record_parts = fw::detail::backend::materialize_binding_parts (
      {wire::encode_actor_message_header (wire::command::actorSend, std::nullopt, fence,
                                          {1, sequence}),
       wire::encode_application_payload (
         wire::application_payload_t::from_parts (std::move (parts).take_items ()))});
    return mesh::service_mailbox_record_t::from_parts (
      mesh::service_mailbox_t::application_owner (fw::runtime::host::owner_kind_t::actor, actor_id),
      mesh::service_mailbox_domain_t::application, std::move (record_parts), std::move (source), {},
      1);
}
} // namespace zlink::framework::test::actor_maintenance_join
