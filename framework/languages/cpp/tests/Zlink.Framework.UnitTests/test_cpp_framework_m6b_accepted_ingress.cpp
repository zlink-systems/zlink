/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/stateful_ingress_fixture.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "../support/m6b_cases.hpp"
#include "../support/m6b_counting_location_repository.hpp"
#include "../support/m6b_task.hpp"
#include "../support/m6b_mesh_fixture.hpp"
#include "../support/m6b_stateful_fixture.hpp"
#include "../support/stateful_object_sync.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"
#include <zlink/Contracts/Core/routing_id.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace mesh = zlink::framework::runtime::mesh;
namespace protocol = zlink::framework::runtime::protocol;
namespace stateful = zlink::framework::runtime::stateful;
namespace host = zlink::framework::runtime::host;
using namespace std::chrono_literals;
using zlink::framework::test::begin_reserved_object;

namespace zlink::framework::test::m6b
{
void verify_discovery_owner_metadata_preserved ()
{
    using namespace zlink::framework;
    const auto path = std::filesystem::path (__FILE__).parent_path ().parent_path ()
                      / "fixtures/source-request-owner.json";
    std::ifstream input (path);
    assert (input);
    const auto fixture = nlohmann::json::parse (input);
    for (const auto &example : fixture.at ("cases")) {
        counting_location_repository_t store;
        runtime::live_location_reader_t reader (store);
        const auto mesh_name = fixture.at ("meshName").get<std::string> ();
        const auto rid =
          zlink::routing_id_t::from (fixture.at ("peer").at ("nodeRid").get<std::string> ());
        const auto generation = fixture.at ("peer").at ("nodeGeneration").get<std::uint64_t> ();
        const auto owner_id = fixture.at ("descriptor").at ("ownerId").get<std::string> ();
        const auto lease_generation =
          fixture.at ("descriptor").at ("leaseGeneration").get<std::int64_t> ();
        location_owner_token_t owner;
        for (std::int64_t i = 0; i != lease_generation; ++i) {
            if (i)
                (void) store.release_owner_lease (owner).result ().value ();
            owner = std::get<owner_lease_claimed_t> (
                      store.claim_owner_lease (owner_id, 15s).result ().value ())
                      .token;
        }
        if (example.at ("stored").get<bool> ()) {
            mesh_node_descriptor_t descriptor{.mesh_name = mesh_name,
                                              .rid = rid,
                                              .lifecycle_generation = generation,
                                              .descriptor_revision = 1,
                                              .endpoint = "tcp://127.0.0.1:1",
                                              .state = framework_runtime_state_t::serving,
                                              .security_identity = "test",
                                              .owner_id = owner.owner_id,
                                              .lease_generation = owner.lease_generation};
            assert (store.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                      .result ()
                      .value ()
                      .status
                    == location_write_status_t::stored);
        }
        if (!example.at ("leaseLive").get<bool> ())
            (void) store.release_owner_lease (owner).result ().value ();
        if (example.at ("polled").get<bool> ())
            (void) reader.poll_mesh_sources (mesh_name).result ().value ();
        store.mesh_node_reads = store.owner_lease_reads = 0;
        const auto accepted =
          reader.request_source (mesh_name, rid.to_bytes (), generation).result ().value ();
        assert (store.mesh_node_reads == example.at ("storeDescriptorReads").get<int> ());
        assert (bool (accepted) == (example.at ("result") == "accepted"));
        if (accepted) {
            assert (accepted->owner_id == owner_id
                    && accepted->lease_generation == lease_generation);
            assert (accepted->rid == rid && accepted->lifecycle_generation == generation);
            const auto reads = store.mesh_node_reads.load ();
            const auto leases = store.owner_lease_reads.load ();
            assert (
              reader.request_source (mesh_name, rid.to_bytes (), generation).result ().value ()
              == accepted);
            assert (store.mesh_node_reads == reads && store.owner_lease_reads == leases);
        }
    }
}

void verify_memory_accepted_record_authority ()
{
    using namespace zlink::framework;
    {
        runtime::in_memory_location_repository_t lease_store;
        lease_store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
        runtime::location_runtime_t leases (lease_store, {}, "observed-owner");
        leases.start (zlink::routing_id_t::from ("observed-node"));
        const auto original_lease = leases.current_owner_observation ();
        assert (original_lease && original_lease->owner_id == leases.owner_id ());
        const auto generation = original_lease->lease_generation;
        leases.stop ();
        assert (!leases.current_owner_observation ());
        assert (original_lease->owner_id == "observed-owner"
                && original_lease->lease_generation == generation);
    }
    auto store = std::make_shared<counting_location_repository_t> ();
    runtime::live_location_reader_t reader (*store);
    const auto target = descriptor ("memory-target");
    auto source = descriptor ("memory-source");
    const auto original = std::make_shared<const mesh_node_descriptor_t> (
      mesh_node_descriptor_t{.mesh_name = source.mesh_name,
                             .rid = zlink::routing_id_t::from (source.node_routing_id),
                             .lifecycle_generation = source.lifecycle_generation,
                             .endpoint = source.advertised_endpoint,
                             .owner_id = "remote-memory-owner",
                             .lease_generation = 1});
    auto native = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = target,
               .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    native->configure_user_spot_operations (
      store,
      [] (const auto &, const auto &, const auto &)
        -> zlink::framework::task_t<
          zlink::framework::runtime::host::user_spot_materialize_result_t> {
          co_return host::user_spot_materialize_result_t{};
      });
    assert (native->transport ().topology ().admit (source, {1})
            == mesh::peer_admission_result_t::admitted);
    const location_owner_token_t local_owner{"local-memory-owner", 11};
    stateful::raw_stateful_dispatch_t dispatch (
      native->objects (), native->transport (),
      [native, local_owner, &reader] (stateful::accepted_record_authority_query_t query) {
          return native->accepted_record_authority (
            query, std::make_shared<const location_owner_token_t> (local_owner), &reader);
      });
    {
        const stateful::object_ref_t object{
          stateful::object_kind_t::user_spot,
          "late-discovery",
          1,
          1,
          target.mesh_name,
          zlink::routing_id_t::from (target.node_routing_id).to_string ()};
        const auto created = begin_reserved_object (native->objects (), object, "memory", {});
        assert (created.error == stateful::stateful_error_t::none);
        assert (native->objects ().commit_create (created.attempt)
                == stateful::stateful_error_t::none);
        const auto owner =
          mesh::service_mailbox_t::application_owner (host::owner_kind_t::spot, object.key);
        const protocol::spot_route_fence_t fence{
          object.key, 1, target.node_routing_id, target.lifecycle_generation, 1, 11};
        auto record = mesh::service_mailbox_record_t::from_parts (
          owner, mesh::service_mailbox_domain_t::application,
          detail::backend::materialize_binding_parts (
            {protocol::encode_spot_message_header (protocol::command::spotSend,
                                                   "memory-source-spot", fence, {1, 900}),
             protocol::encode_application_payload ({"Memory", "application/json", {'{', '}'}})}),
          source.node_routing_id, std::nullopt, source.lifecycle_generation);
        const auto slot =
          test::publish_stateful_claim (native->transport (), object, std::move (record));
        const auto result =
          dispatch
            .ingest_claimed (object, slot,
                             test::retained_ingress_snapshot (native->objects (), object))
            .result ()
            .value ();
        assert (result == stateful::stateful_error_t::unavailable);
        assert (slot.claim->outstanding () == 0);
        assert (native->objects ().pending (object, stateful::turn_domain_t::application) == 0);
        assert (!dispatch.try_claim (object).second);
        assert (store->authority_reads == 0 && store->mesh_node_reads == 1
                && store->owner_lease_reads == 0);
    }
    (void) store->claim_owner_lease (original->owner_id, 15s).result ().value ();
    auto source_descriptor = *original;
    source_descriptor.descriptor_revision = 1;
    source_descriptor.state = framework_runtime_state_t::serving;
    source_descriptor.security_identity = "test";
    (void) store->update_mesh_node (source_descriptor, location_write_intent_t::new_claim)
      .result ()
      .value ();
    (void) reader.poll_mesh_sources (source.mesh_name).result ().value ();
    store->mesh_node_reads = store->owner_lease_reads = 0;
    native->transport ().expect_peer (source);
    const auto local_observation = std::make_shared<const location_owner_token_t> (local_owner);
    const mesh::local_request_source_t local_source{
      local_observation, std::make_shared<const mesh::service_node_descriptor_t> (target)};
    std::uint64_t sequence = 0;
    for (const bool remote : {false, true})
        for (const bool actor : {false, true})
            for (const bool request : {false, true}) {
                ++sequence;
                const stateful::object_ref_t object{
                  actor ? stateful::object_kind_t::actor : stateful::object_kind_t::user_spot,
                  "memory-object-" + std::to_string (sequence),
                  1,
                  1,
                  target.mesh_name,
                  zlink::routing_id_t::from (target.node_routing_id).to_string ()};
                const auto created =
                  begin_reserved_object (native->objects (), object, "memory", {});
                assert (created.error == stateful::stateful_error_t::none);
                assert (native->objects ().commit_create (created.attempt)
                        == stateful::stateful_error_t::none);
                const protocol::spot_route_fence_t spot{
                  object.key, 1, target.node_routing_id, target.lifecycle_generation, 1, 11};
                const protocol::actor_route_fence_t actor_route{
                  object.key, 1, target.node_routing_id, target.lifecycle_generation, 1, 11};
                const auto correlation = request ? std::make_optional (sequence) : std::nullopt;
                const auto header =
                  actor
                    ? protocol::encode_actor_message_header (
                        request ? protocol::command::actorRequest : protocol::command::actorSend,
                        std::nullopt, actor_route, {1, sequence}, correlation)
                    : protocol::encode_spot_message_header (
                        request ? protocol::command::spotRequest : protocol::command::spotSend,
                        "memory-source-spot", spot, {1, sequence}, correlation);
                auto record = mesh::service_mailbox_record_t::from_parts (
                  {}, mesh::service_mailbox_domain_t::application,
                  detail::backend::materialize_binding_parts (
                    {header, protocol::encode_application_payload (
                               {"Memory", "application/json", {'{', '}'}})}),
                  remote ? source.node_routing_id : target.node_routing_id, std::nullopt,
                  remote ? source.lifecycle_generation : target.lifecycle_generation);
                // Local publication reads this node's lease observation before acceptance.
                if (!remote)
                    record.source_original = mesh::request_source_t{local_source};
                assert (dispatch
                          .ingest_claimed (object,
                                           test::publish_stateful_claim (
                                             native->transport (), object, std::move (record)),
                                           zlink::framework::test::retained_ingress_snapshot (
                                             native->objects (), object))
                          .result ()
                          .value ()
                        == stateful::stateful_error_t::none);
                const auto [error, delivery] = dispatch.try_claim (object);
                assert (error == stateful::stateful_error_t::none && delivery);
                assert (delivery->frozen.source.owner_id
                        == (remote ? original->owner_id : local_owner.owner_id));
                assert (delivery->frozen.source.lease_generation == (remote ? 1u : 11u));
                assert (store->authority_reads == 0);
                assert (store->mesh_node_reads == 0);
                assert (store->owner_lease_reads == 0);
            }
    const auto saved_remote =
      reader.request_source (source.mesh_name, source.node_routing_id, source.lifecycle_generation)
        .result ()
        .value ();
    assert (saved_remote && saved_remote->owner_id == original->owner_id);
    const auto saved_local = mesh::raw_mesh_node_owner_t::local_accepted_source (
      target.node_routing_id, target.lifecycle_generation, local_source);
    assert (saved_local
            && std::get<mesh::local_request_source_t> (*saved_local).lease == local_observation);
    (void) store
      ->remove_mesh_node ({source.mesh_name, source_descriptor.rid},
                          {source_descriptor.owner_id, source_descriptor.lease_generation})
      .result ()
      .value ();
    (void) reader.poll_mesh_sources (source.mesh_name).result ().value ();
    assert (
      !reader.request_source (source.mesh_name, source.node_routing_id, source.lifecycle_generation)
         .result ()
         .value ());
    assert (saved_remote->owner_id == original->owner_id && saved_remote->lease_generation == 1);
}

void verify_batched_accepted_record_sources ()
{
    using namespace zlink::framework;
    counting_location_repository_t store;
    const auto owner = std::get<owner_lease_claimed_t> (
                         store.claim_owner_lease ("batch-source-owner", 15s).result ().value ())
                         .token;
    std::vector<protocol::request_source_fence_t> sources;
    for (const auto *id : {"batch-source-one", "batch-source-two"}) {
        mesh_node_descriptor_t node{.mesh_name = "m6b-mesh",
                                    .rid = zlink::routing_id_t::from (id),
                                    .lifecycle_generation = 1,
                                    .descriptor_revision = 1,
                                    .endpoint = "tcp://127.0.0.1:1",
                                    .state = framework_runtime_state_t::serving,
                                    .security_identity = "test",
                                    .owner_id = owner.owner_id,
                                    .lease_generation = owner.lease_generation};
        assert (store.update_mesh_node (node, location_write_intent_t::new_claim)
                  .result ()
                  .value ()
                  .status
                == location_write_status_t::stored);
        sources.push_back ({owner.owner_id, static_cast<std::uint64_t> (owner.lease_generation),
                            node.rid.to_bytes (), 1});
    }
    runtime::live_location_reader_t reader (store);
    const auto read = [&] () -> std::optional<std::vector<protocol::request_source_fence_t>> {
        (void) reader.poll_mesh_sources ("m6b-mesh").result ().value ();
        std::vector<protocol::request_source_fence_t> resolved;
        for (const auto &source : sources) {
            const auto original =
              reader.request_source ("m6b-mesh", source.node_routing_id, source.node_generation)
                .result ()
                .value ();
            if (!original)
                return std::nullopt;
            resolved.push_back ({original->owner_id,
                                 static_cast<std::uint64_t> (original->lease_generation),
                                 source.node_routing_id, source.node_generation});
        }
        return resolved;
    };
    store.mesh_node_reads = store.owner_lease_reads = 0;
    const auto live = read ();
    assert (live && *live == sources);
    assert (store.mesh_node_reads == 1 && store.owner_lease_reads == 1);
    const auto now = std::chrono::system_clock::now ();
    store.owner_lease_override = owner_lease_found_t{owner, now, now};
    const auto mesh_reads_before_expiry = store.mesh_node_reads.load ();
    const auto lease_reads_before_expiry = store.owner_lease_reads.load ();
    assert (!read ());
    assert (store.mesh_node_reads == mesh_reads_before_expiry + 1);
    assert (store.owner_lease_reads == lease_reads_before_expiry + 1);
    auto impostor = owner;
    impostor.owner_id = "another-owner";
    store.owner_lease_override = owner_lease_found_t{impostor, now + 15s, now};
    assert (!read ());
    store.owner_lease_override.reset ();
    ++sources.front ().node_generation;
    assert (!read ());
    --sources.front ().node_generation;
    assert (read () == live);
}

void verify_location_store_accepted_record_authority ()
{
    using namespace zlink::framework;
    runtime::in_memory_location_repository_t store;
    store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto source_owner = std::get<owner_lease_claimed_t> (
                                store.claim_owner_lease ("source-owner", 15s).result ().value ())
                                .token;
    const auto target_owner = std::get<owner_lease_claimed_t> (
                                store.claim_owner_lease ("target-owner", 15s).result ().value ())
                                .token;
    const auto register_node = [&] (std::string rid, const location_owner_token_t &owner) {
        mesh_node_descriptor_t node{
          .mesh_name = "m6b-mesh",
          .rid = zlink::routing_id_t::from (rid),
          .lifecycle_generation = 1,
          .descriptor_revision = 1,
          .endpoint = "tcp://127.0.0.1:1",
          .application_version = 1,
          .object_capabilities =
            {{.object_kind = placement_object_kind_t::actor, .stable_type = "player"},
             {.object_kind = placement_object_kind_t::user_spot, .stable_type = "room"},
             {.object_kind = placement_object_kind_t::instance_spot, .stable_type = "room"}},
          .object_role = object_role_t::server,
          .capacity = {.actors = {.limit = 8}, .spots = {.limit = 8}},
          .state = framework_runtime_state_t::serving,
          .security_identity = "test",
          .owner_id = owner.owner_id,
          .lease_generation = owner.lease_generation};
        assert (store.update_mesh_node (std::move (node), location_write_intent_t::new_claim)
                  .result ()
                  .value ()
                  .status
                == location_write_status_t::stored);
    };
    register_node ("raw-source", source_owner);
    register_node ("raw-target", target_owner);
    const object_reserve_request_t request{
      .key = {placement_object_kind_t::actor, "actor-authority"},
      .intent = {.stable_type = "player"},
      .target = {.mesh_name = "m6b-mesh",
                 .node_rid = node_rid_t::from_string ("raw-target"),
                 .node_lifecycle_generation = 1,
                 .owner = target_owner},
      .capacity_bundle = {.actor_slots = 1}};
    const auto reserved = std::get<object_reserved_t> (store.reserve (request).result ().value ());
    const auto committed = std::get<object_committed_t> (
      store.commit ({request.key, reserved.fence, {std::byte{1}}}).result ().value ());

    const auto resolver = stateful::make_location_store_authority_resolver (store);
    const auto resolved =
      resolver ({.target = {stateful::object_kind_t::actor, "actor-authority",
                            committed.ready.object_generation,
                            committed.ready.authority_owner_generation, "m6b-mesh", "raw-target"},
                 .source_node_routing_id = bytes ("raw-source"),
                 .source_node_generation = 1,
                 .target_node_routing_id = bytes ("raw-target"),
                 .target_node_generation = 1,
                 .source_kind = protocol::frozen_source_kind_t::node,
                 .requested_authority_owner_generation = committed.ready.authority_owner_generation,
                 .requested_object_generation = committed.ready.object_generation})
        .result ()
        .value ();
    assert (resolved);
    assert (resolved->source.owner_id == source_owner.owner_id);
    assert (resolved->source.lease_generation
            == static_cast<std::uint64_t> (source_owner.lease_generation));
    assert (resolved->target_owner_lease_generation
            == static_cast<std::uint64_t> (target_owner.lease_generation));

    // A bound-session query carries the session triple next to the owning
    // Actor identity so resolvers can fence-check the binding.
    const auto resolved_bound =
      resolver ({.target = {stateful::object_kind_t::actor, "actor-authority",
                            committed.ready.object_generation,
                            committed.ready.authority_owner_generation, "m6b-mesh", "raw-target"},
                 .source_node_routing_id = bytes ("raw-source"),
                 .source_node_generation = 1,
                 .target_node_routing_id = bytes ("raw-target"),
                 .target_node_generation = 1,
                 .source_kind = protocol::frozen_source_kind_t::bound_session,
                 .source_actor = std::make_pair (std::string ("actor-authority"),
                                                 committed.ready.object_generation),
                 .source_session_routing_id = bytes ("sess-1"),
                 .source_binding_generation = 5,
                 .source_session_sequence = 9,
                 .requested_authority_owner_generation = committed.ready.authority_owner_generation,
                 .requested_object_generation = committed.ready.object_generation})
        .result ()
        .value ();
    assert (resolved_bound);
    assert (resolved_bound->source.owner_id == source_owner.owner_id);
    assert (resolved_bound->target_owner_lease_generation
            == static_cast<std::uint64_t> (target_owner.lease_generation));

    auto stale = stateful::accepted_record_authority_query_t{
      .target = {stateful::object_kind_t::actor, "actor-authority",
                 committed.ready.object_generation, committed.ready.authority_owner_generation,
                 "m6b-mesh", "raw-target"},
      .source_node_routing_id = bytes ("raw-source"),
      .source_node_generation = 2,
      .target_node_routing_id = bytes ("raw-target"),
      .target_node_generation = 1,
      .source_kind = protocol::frozen_source_kind_t::node,
      .requested_authority_owner_generation = committed.ready.authority_owner_generation,
      .requested_object_generation = committed.ready.object_generation};
    assert (!resolver (stale).result ().value ());

    auto previous_incarnation = stale;
    previous_incarnation.source_node_generation = 1;
    previous_incarnation.requested_object_generation = committed.ready.object_generation + 1;
    ++previous_incarnation.requested_authority_owner_generation;
    assert (resolver (previous_incarnation).result ().value ());
    auto same_incarnation_wrong_owner = previous_incarnation;
    same_incarnation_wrong_owner.requested_object_generation = committed.ready.object_generation;
    assert (!resolver (same_incarnation_wrong_owner).result ().value ());

    for (const auto kind :
         {placement_object_kind_t::user_spot, placement_object_kind_t::instance_spot}) {
        auto spot_request = request;
        spot_request.key = {kind, kind == placement_object_kind_t::user_spot
                                    ? "user-authority"
                                    : "instance-authority"};
        spot_request.intent.stable_type = "room";
        spot_request.capacity_bundle = {.spot_slots = 1,
                                        .spot_type = spot_type_capacity_delta_t{kind, "room", 1}};
        const auto spot_reserved =
          std::get<object_reserved_t> (store.reserve (spot_request).result ().value ());
        const auto spot_committed = std::get<object_committed_t> (
          store.commit ({spot_request.key, spot_reserved.fence, {std::byte{1}}})
            .result ()
            .value ());
        auto query = stale;
        query.target = {stateful::object_kind_t::user_spot,
                        spot_request.key.global_id,
                        spot_committed.ready.object_generation,
                        spot_committed.ready.authority_owner_generation,
                        "m6b-mesh",
                        "raw-target"};
        query.source_node_generation = 1;
        query.requested_authority_owner_generation = query.target.authority_owner_generation;
        assert (resolver (query).result ().value ());
        auto wrong_generation = query;
        ++wrong_generation.target.object_generation;
        assert (!resolver (wrong_generation).result ().value ());
        auto wrong_authority = query;
        ++wrong_authority.requested_authority_owner_generation;
        assert (!resolver (wrong_authority).result ().value ());
        auto wrong_node = query;
        ++wrong_node.target_node_generation;
        assert (!resolver (wrong_node).result ().value ());
        auto wrong_kind = query;
        wrong_kind.target.kind = stateful::object_kind_t::actor;
        assert (!resolver (wrong_kind).result ().value ());
    }
    auto retained_query = stale;
    retained_query.source_node_generation = 1;
    const object_creation_target_t relocated_target{
      "m6b-mesh", node_rid_t::from_string ("raw-source"), 1, source_owner};
    const auto moved =
      store
        .compare_exchange_authority (runtime::actor_authority_key ("actor-authority"),
                                     committed.ready.store_version,
                                     authority_retarget_t{{std::byte{1}}, relocated_target})
        .result ()
        .value ();
    assert (std::holds_alternative<authority_stored_t> (moved));
    assert (!resolver (retained_query).result ().value ());
    // Retained Follow is qualified by the native source seal (Location §7.3),
    // not this Ready-only Store resolver.
}
void verify_atomic_raw_stateful_ingress_commit ()
{
    []<typename Delivery> {
        static_assert (!requires (Delivery delivery) { delivery.payload; });
    }.template operator()<stateful::stateful_delivery_t> ();
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("atomic-ingress-target"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    target.start ();
    const auto target_descriptor = target.topology ().local_descriptor ();

    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates (
      {{"m6b-mesh", "atomic-ingress-target", {"player"}, 100, 16, 0, 8, 0}});
    const auto actor = create_ready (
      objects,
      {stateful::object_kind_t::actor, "atomic-actor", "player", std::nullopt, {}, false, false});

    const auto source_routing_id = bytes ("atomic-ingress-source");
    constexpr std::uint64_t source_generation = 7;
    constexpr std::uint64_t owner_lease_generation = 19;
    bool authority_live = false;
    std::size_t authority_queries = 0;
    stateful::raw_stateful_dispatch_t dispatch (
      objects, target,
      [&] (stateful::accepted_record_authority_query_t query)
        -> zlink::framework::task_t<std::optional<stateful::accepted_record_authority_t>> {
          ++authority_queries;
          assert (query.target == actor);
          assert (query.source_node_routing_id == source_routing_id);
          assert (query.source_node_generation == source_generation);
          if (!authority_live)
              co_return std::nullopt;
          co_return stateful::accepted_record_authority_t{
            {"atomic-source-owner", 23, source_routing_id, source_generation},
            owner_lease_generation};
      });
    const protocol::actor_route_fence_t fence{actor.key,
                                              actor.object_generation,
                                              target_descriptor.node_routing_id,
                                              target_descriptor.lifecycle_generation,
                                              actor.authority_owner_generation,
                                              owner_lease_generation};
    const auto enqueue = [&] (std::uint64_t operation_low, std::string payload) {
        const protocol::wire_operation_id_t operation{41, operation_low};
        assert (target.mailbox ().try_enqueue (mesh::service_mailbox_record_t::from_parts (
          "actor:atomic-actor", mesh::service_mailbox_domain_t::application,
          zlink::framework::detail::backend::materialize_binding_parts (
            {protocol::encode_actor_message_header (protocol::command::actorSend, std::nullopt,
                                                    fence, operation),
             protocol::encode_application_payload (
               {"ActorPacket", "application/json", bytes (std::move (payload))})}),
          source_routing_id, std::nullopt, source_generation,
          std::make_pair (operation.high, operation.low))));
    };
    const auto assert_no_queued_reservation = [&] {
        assert (objects.pending (actor, stateful::turn_domain_t::application) == 0);
        assert (objects.pending_bytes (actor, stateful::turn_domain_t::application) == 0);
    };

    // Authority rejection is outside the accepted-ingress transaction and
    // cannot allocate sequence 1 or reserve application work.
    enqueue (1, "authority-rejected");
    assert (
      zlink::framework::test::ingest_stateful_mailbox_record (objects, target, dispatch, actor)
        .result ()
        .value ()
      == stateful::stateful_error_t::unavailable);
    assert_no_queued_reservation ();
    authority_live = true;

    // Cross the former one-record object queue boundary through the production
    // object port while no transport claim is active.
    assert (objects.enqueue (actor, stateful::turn_domain_t::application,
                             stateful::turn_record_t{900, bytes ("capacity-filler"), 1})
            == stateful::stateful_error_t::none);
    const auto filled_count = objects.pending (actor, stateful::turn_domain_t::application);
    const auto filled_bytes = objects.pending_bytes (actor, stateful::turn_domain_t::application);
    enqueue (2, "accepted-beyond-former-capacity");
    assert (
      zlink::framework::test::ingest_stateful_mailbox_record (objects, target, dispatch, actor)
        .result ()
        .value ()
      == stateful::stateful_error_t::none);
    assert (objects.pending (actor, stateful::turn_domain_t::application) == filled_count + 1);
    assert (objects.pending_bytes (actor, stateful::turn_domain_t::application) > filled_bytes);
    assert (dispatch.discard_pending (actor, 1) == stateful::stateful_error_t::none);
    assert (objects.discard_application (actor, 900) == stateful::stateful_error_t::none);
    assert_no_queued_reservation ();

    enqueue (3, "accepted-one");
    assert (
      zlink::framework::test::ingest_stateful_mailbox_record (objects, target, dispatch, actor)
        .result ()
        .value ()
      == stateful::stateful_error_t::none);
    const auto authority_queries_after_admission = authority_queries;
    authority_live = false;
    const auto [first_error, first] = dispatch.try_claim (actor);
    assert (first_error == stateful::stateful_error_t::none && first);
    assert (first->turn.sequence == 2);
    assert (first->frozen.source.owner_id == "atomic-source-owner");
    assert (first->frozen.source.lease_generation == 23);
    assert (authority_queries == authority_queries_after_admission);
    assert_no_queued_reservation ();

    // A running item does not prevent another item from waiting in its FIFO.
    assert (objects.enqueue (actor, stateful::turn_domain_t::application,
                             stateful::turn_record_t{901, bytes ("while-running"), 1})
            == stateful::stateful_error_t::none);
    assert (objects.discard_application (actor, 901) == stateful::stateful_error_t::none);
    assert (dispatch.complete_async (*first).result ().value ()
            == stateful::stateful_error_t::none);
    assert (dispatch.complete_async (*first).result ().value ()
            == stateful::stateful_error_t::conflict);
    authority_live = true;

    enqueue (4, "accepted-two");
    assert (
      zlink::framework::test::ingest_stateful_mailbox_record (objects, target, dispatch, actor)
        .result ()
        .value ()
      == stateful::stateful_error_t::none);
    const auto [second_error, second] = dispatch.try_claim (actor);
    assert (second_error == stateful::stateful_error_t::none && second);
    assert (second->turn.sequence == 3);
    assert (dispatch.complete_async (*second).result ().value ()
            == stateful::stateful_error_t::none);

    enqueue (6, "accepted-three");
    assert (
      zlink::framework::test::ingest_stateful_mailbox_record (objects, target, dispatch, actor)
        .result ()
        .value ()
      == stateful::stateful_error_t::none);
    const auto [third_error, third] = dispatch.try_claim (actor);
    assert (third_error == stateful::stateful_error_t::none && third);
    assert (third->turn.sequence == 4);
    assert (dispatch.complete_async (*third).result ().value ()
            == stateful::stateful_error_t::none);
    assert (authority_queries == 5);

    // A flat accepted payload keeps its allocation through the local native
    // adapter and claim. The structural check above also rejects a second
    // delivery payload, which pointer identity of the canonical turn cannot detect.
    protocol::actor_message_header_t local_header;
    local_header.operation = {41, 7};
    local_header.target = fence;
    protocol::application_payload_t local_payload{"ActorPacket", "application/json",
                                                  bytes ("accepted-flat")};
    const auto *accepted_bytes = local_payload.payload_bytes ().data ();
    auto accepted = std::make_shared<zlink::framework::runtime::serial_application_record_t> ();
    accepted->payload ().emplace ();
    accepted->payload ()->application_record = stateful::accepted_actor_record (
      local_header, {"atomic-source-owner", 23, source_routing_id, source_generation},
      std::move (local_payload), false);
    std::optional<mesh::service_mailbox_record_t> taken_original;
    assert (
      dispatch.accept_local (actor, accepted, mesh::service_mailbox_record_t{}, taken_original, {})
        .result ()
        .value ()
      == stateful::stateful_error_t::none);
    assert (!accepted->payload ());
    const auto [flat_error, flat] = dispatch.try_claim (actor);
    assert (flat_error == stateful::stateful_error_t::none && flat);
    assert (flat->application_record == accepted);
    stateful::with_accepted_application (flat->turn, [&] (auto kind, const auto &application) {
        assert (kind == protocol::frozen_record_kind_t::actor_send);
        assert (application.payload_bytes () == bytes ("accepted-flat"));
        assert (application.payload_bytes ().data () == accepted_bytes);
    });
    assert (dispatch.complete_async (*flat).result ().value () == stateful::stateful_error_t::none);
    assert (dispatch.complete_async (*flat).result ().value ()
            == stateful::stateful_error_t::conflict);
    assert_no_queued_reservation ();
    assert (authority_queries == 5);
    target.close ();
}
void verify_relocation_data_duplicates_are_distinct_arrivals ()
{
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("duplicate-data-target"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    target.start ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    const auto source_routing_id = bytes ("duplicate-data-source");
    constexpr std::uint64_t source_generation = 7;
    const protocol::relocation_id_t relocation{0x91, 0x92};
    const protocol::relocation_coordinator_fence_t coordinator{
      "coordinator", 11, bytes ("coordinator-node"), 13, "store-v1"};
    const protocol::relocation_object_t wire_object{
      protocol::relocation_object_kind_t::actor, {}, "duplicate-data-actor", 3, 5};
    auto registered_object = wire_object;
    registered_object.stable_type = "player";

    protocol::frozen_application_record_t accepted;
    accepted.kind = protocol::frozen_record_kind_t::actor_send;
    accepted.source_kind = protocol::frozen_source_kind_t::node;
    accepted.source = {"source-owner", 17, source_routing_id, source_generation};
    accepted.operation = {19, 23};
    accepted.body = protocol::frozen_actor_application_body_t{
      {wire_object.object_id, wire_object.object_generation, target_descriptor.node_routing_id,
       target_descriptor.lifecycle_generation, wire_object.expected_authority_owner_generation, 29},
      {"ActorPacket", "application/json", bytes ("duplicate")}};
    const auto frozen = protocol::encode_frozen_application_record (accepted);
    const protocol::relocation_data_t data{
      relocation, 31, coordinator, protocol::relocation_role_t::source, wire_object, frozen};
    const auto encoded_data = protocol::encode_relocation_control (data);
    const auto wire_record = mesh::service_mailbox_record_t::from_parts (
      "duplicate-data-target", mesh::service_mailbox_domain_t::infrastructure,
      zlink::framework::detail::backend::materialize_binding_parts ({encoded_data}),
      source_routing_id, std::nullopt, source_generation);

    std::vector<protocol::wire_operation_id_t> target_queue;
    stateful::raw_relocation_replay_coordinator_t replay (target);
    assert (replay.register_target ({relocation, 31, coordinator, source_routing_id,
                                     source_generation, registered_object,
                                     [&] (const protocol::relocation_data_t &arrival) {
                                         target_queue.push_back (arrival.record.operation);
                                         return true;
                                     },
                                     [&] (const protocol::relocation_data_t &) {
                                         assert (false && "accepted duplicate arrival rolled back");
                                     }}));

    assert (await_task (replay.process (wire_record))
            == stateful::raw_relocation_replay_result_t::applied);
    assert (await_task (replay.process (wire_record))
            == stateful::raw_relocation_replay_result_t::applied);
    assert ((target_queue
             == std::vector<protocol::wire_operation_id_t>{frozen.operation, frozen.operation}));
    target.close ();
}

void verify_relocation_ingress_boundary_owns_saved_and_follow_only_work ()
{
    stateful::stateful_object_runtime_t runtime;
    runtime.replace_placement_candidates (
      {{"relocation-boundary", "source", {"player"}, 100, 16, 0, 16, 0}});
    const auto actor = create_ready (runtime, {stateful::object_kind_t::actor,
                                               "boundary-actor",
                                               "player",
                                               std::string ("relocation-boundary"),
                                               {},
                                               false,
                                               false});
    assert (runtime.enqueue (actor, stateful::turn_domain_t::application, {1, bytes ("saved")})
            == stateful::stateful_error_t::none);

    const auto [seal_error, seal] = await_task (runtime.try_seal_relocation_aggregate ({actor}));
    assert (seal_error == stateful::stateful_error_t::none);
    assert (seal.participants.size () == 1);
    assert (seal.participants.front ().pending_application.size () == 1);
    assert (runtime.enqueue (actor, stateful::turn_domain_t::application, {2, bytes ("boundary")})
            == stateful::stateful_error_t::none);
    const auto [boundary_error, boundary] = runtime.begin_relocation_boundary (seal.token);
    assert (boundary_error == stateful::stateful_error_t::none);
    assert (boundary.participants.size () == 1);
    assert (boundary.participants.front ().records.size () == 1);
    assert (boundary.participants.front ().records.front ().sequence == 2);
    assert (runtime.enqueue (actor, stateful::turn_domain_t::application, {3, bytes ("later")})
            == stateful::stateful_error_t::none);

    // Before Cutover, exact abort restores each source-owned segment once:
    // saved prefix, transferred boundary batch, then later held ingress.
    assert (runtime.abort_relocation_before_cutover (seal.token).result ().value ()
            == stateful::stateful_error_t::none);
    for (const auto expected : {1ull, 2ull, 3ull}) {
        const auto [claim_error, record] =
          runtime.try_claim (actor, stateful::turn_domain_t::application);
        assert (claim_error == stateful::stateful_error_t::none);
        assert (record && record->sequence == expected);
        assert (runtime.complete_claim (actor, stateful::turn_domain_t::application)
                == stateful::stateful_error_t::none);
    }

    const auto [second_error, second] =
      await_task (runtime.try_seal_relocation_aggregate ({actor}));
    assert (second_error == stateful::stateful_error_t::none);
    assert (
      runtime.enqueue (actor, stateful::turn_domain_t::application, {4, bytes ("post-capture")})
      == stateful::stateful_error_t::none);
    const auto [second_boundary_error, second_boundary] =
      runtime.begin_relocation_boundary (second.token);
    assert (second_boundary_error == stateful::stateful_error_t::none);
    assert (second_boundary.participants.front ().records.size () == 1);
    assert (
      runtime.enqueue (actor, stateful::turn_domain_t::application, {5, bytes ("follow-only")})
      == stateful::stateful_error_t::none);
    assert (runtime.finalize_relocation_cutover (second.token).result ().value ()
            == stateful::stateful_error_t::none);
    assert (runtime.abort_relocation_before_cutover (second.token).result ().value ()
            == stateful::stateful_error_t::conflict);
    const auto [claim_error, record] =
      runtime.try_claim (actor, stateful::turn_domain_t::application);
    assert (claim_error == stateful::stateful_error_t::moving);
    assert (!record);
}
}
