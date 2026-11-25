/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_stream_fixture.hpp"
#include "../support/actor_maintenance_join_native_fixture.hpp"
#include "../support/stateful_object_sync.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace zlink::framework::test::actor_maintenance_join
{
using zlink::framework::test::begin_reserved_object;

TEST (RelayAdmission, ManualWirePeersDoNotRequireDiscovery)
{
    using role_t = fw::runtime::mesh::service_object_role_t;
    // A manual or route-only member (object role None) publishes no descriptor and has no
    // object runtime, so its wire admission never waits for a discovery original.
    for (const auto target_role : {role_t::none, role_t::server}) {
        SCOPED_TRACE (static_cast<int> (target_role));
        relay_peer_pair_t pair (role_t::none, target_role);
        ASSERT_TRUE (pair.await_ready ());
        EXPECT_TRUE (pair.accepted_source (pair.source_wire.lifecycle_generation));
    }
}

// Location runtime §9.3: discovery is not an admission input.
TEST (RelayAdmission, HandshakePublishesReadyBeforeDiscovery)
{
    relay_peer_pair_t pair;
    ASSERT_TRUE (pair.await_ready ());
}

TEST (RelayAdmission, AcceptedSourceRejectsWrongFences)
{
    using namespace fw::runtime;
    relay_peer_pair_t pair;
    ASSERT_TRUE (pair.await_ready ());
    stateful::object_ref_t actor{
      stateful::object_kind_t::actor, actor_id, 1, 1, mesh_name, "relay-target"};
    const auto created = begin_reserved_object (pair.target->objects (), actor, actor_type, {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (pair.target->objects ().commit_create (created.attempt),
               stateful::stateful_error_t::none);
    stateful::raw_stateful_dispatch_t dispatch (
      pair.target->objects (), pair.target->transport (),
      [&] (stateful::accepted_record_authority_query_t q) {
          return pair.target->accepted_record_authority (std::move (q), pair.lease,
                                                         &pair.source_reader);
      });
    protocol::actor_message_header_t header;
    header.target = {
      actor_id, 1, pair.target_wire.node_routing_id, pair.target_wire.lifecycle_generation, 1, 1};
    header.operation = {1, 3};
    header.bound_session_source =
      protocol::actor_message_header_t::bound_session_source_t{{0, 0, 0, 1}, 1, 1};
    const auto authority = [&] (auto h, std::uint64_t generation) {
        return dispatch
          .accepted_actor_authority (actor, h, pair.source_wire.node_routing_id, generation,
                                     pair.accepted_source (generation), true, std::nullopt)
          .result ()
          .value ();
    };
    ASSERT_EQ (authority (header, pair.source_wire.lifecycle_generation).first,
               stateful::stateful_error_t::none);
    const auto denied = [&] (auto h, std::uint64_t generation, auto expected) {
        const auto result = authority (h, generation);
        EXPECT_EQ (result.first, expected);
        EXPECT_FALSE (result.second);
    };
    auto bad = header;
    ++bad.target.owner_lease_generation;
    denied (bad, pair.source_wire.lifecycle_generation, stateful::stateful_error_t::conflict);
    bad = header;
    ++bad.target.authority_owner_generation;
    denied (bad, pair.source_wire.lifecycle_generation, stateful::stateful_error_t::conflict);
    bad = header;
    ++bad.target.target_node_generation;
    denied (bad, pair.source_wire.lifecycle_generation, stateful::stateful_error_t::conflict);
    denied (header, pair.source_wire.lifecycle_generation + 1,
            stateful::stateful_error_t::unavailable);
}

TEST (ActorDirectRequest, AdmissionDistinguishesIncarnationFromOwner)
{
    using namespace fw::runtime;
    relay_peer_pair_t pair;
    ASSERT_TRUE (pair.await_ready ());
    stateful::object_ref_t actor{
      stateful::object_kind_t::actor, actor_id, 2, 2, mesh_name, "relay-target"};
    const auto created = begin_reserved_object (pair.target->objects (), actor, actor_type, {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (pair.target->objects ().commit_create (created.attempt),
               stateful::stateful_error_t::none);
    stateful::accepted_record_authority_query_t query{
      .target = actor,
      .source_node_routing_id = pair.source_wire.node_routing_id,
      .source_node_generation = pair.source_wire.lifecycle_generation,
      .source_original = pair.accepted_source (pair.source_wire.lifecycle_generation),
      .target_node_routing_id = pair.target_wire.node_routing_id,
      .target_node_generation = pair.target_wire.lifecycle_generation,
      .requested_authority_owner_generation = 1,
      .requested_object_generation = 1};
    const auto recreated =
      pair.target->accepted_record_authority (query, pair.lease, &pair.source_reader).result ();
    ASSERT_TRUE (recreated);
    ASSERT_TRUE (recreated.value ());
    query.requested_object_generation = actor.object_generation;
    EXPECT_FALSE (
      pair.target->accepted_record_authority (query, pair.lease, &pair.source_reader).result ());
}

// Location runtime §9.3: a record keeps the source original that was valid when it was
// accepted. A discovery UPDATE or delete before its claim reads the source does not change
// the four source values.
TEST (RelayAdmission, AcceptedRecordKeepsSourceOriginalAcrossDiscoveryChange)
{
    using namespace fw::runtime;
    for (const bool removed : {false, true}) {
        SCOPED_TRACE (removed ? "discovery delete" : "discovery update");
        std::optional<stateful::accepted_record_authority_query_t> query;
        fw::task_completion_source_t<std::optional<stateful::accepted_record_authority_t>> held;
        relay_peer_pair_t pair (mesh::service_object_role_t::server,
                                mesh::service_object_role_t::server,
                                [&] (stateful::accepted_record_authority_query_t value) {
                                    query = std::move (value);
                                    return held.task ();
                                });
        pair.target->transport ().expect_peer (pair.source_wire);
        ASSERT_TRUE (pair.await_ready ());
        stateful::object_ref_t actor{
          stateful::object_kind_t::actor, actor_id, 1, 1, mesh_name, "relay-target"};
        const auto created = begin_reserved_object (pair.target->objects (), actor, actor_type, {});
        ASSERT_EQ (created.error, stateful::stateful_error_t::none);
        ASSERT_EQ (pair.target->objects ().commit_create (created.attempt),
                   stateful::stateful_error_t::none);
        const protocol::actor_route_fence_t fence{
          actor_id, 1, pair.target_wire.node_routing_id, pair.target_wire.lifecycle_generation,
          1,        1};
        auto sent = pair.source->transport ().send_to_actor (
          pair.target_wire.node_routing_id, std::nullopt, fence,
          {"Probe", "application/json", {'{', '}'}}, std::nullopt,
          protocol::wire_operation_id_t{pair.source_wire.lifecycle_generation, 41});
        // A held dispatch turn keeps referring to its callbacks, so they outlive it.
        const std::function<void (const host::ready_record_t &, const host::receive_record_t &,
                                  std::vector<zlink::message_t>,
                                  zlink::framework::runtime::application_job_queue_t::permit_t)>
          discard = [] (const auto &, const auto &, auto, auto) {};
        const std::function<bool ()> no_next_receive;
        std::optional<fw::task_t<std::size_t>> turn;
        // Settles the held claim and its dispatch turn before the hosts close.
        struct release_t
        {
            fw::task_completion_source_t<std::optional<stateful::accepted_record_authority_t>>
              &source;
            std::optional<fw::task_t<std::size_t>> &turn;
            ~release_t ()
            {
                source.complete (
                  fw::result_t<std::optional<stateful::accepted_record_authority_t>>::success (
                    std::nullopt));
                if (turn)
                    (void) turn->result ();
            }
        } release{held, turn};
        const auto deadline = std::chrono::steady_clock::now () + wait_budget;
        while (!query && std::chrono::steady_clock::now () < deadline) {
            pair.source->dispatch_ready (discard).result ().value ();
            auto next = pair.target->dispatch_ready (discard, true, no_next_receive);
            if (!next.await_ready ()) {
                turn.emplace (std::move (next));
                break;
            }
            next.result ().value ();
            std::this_thread::sleep_for (1ms);
        }
        ASSERT_TRUE (sent.result ().value ());
        ASSERT_TRUE (query);
        // The record is in the target mailbox and its claim waits for authority.
        (void) pair.source_store
          .remove_mesh_node ({mesh_name, pair.original->rid},
                             {pair.original->owner_id, pair.original->lease_generation})
          .result ()
          .value ();
        if (removed) {
            pair.target->transport ().forget_peer (pair.source_wire.node_routing_id,
                                                   pair.source_wire.advertised_endpoint);
        } else {
            auto replacement = *pair.original;
            replacement.owner_id = "source-replacement";
            (void) pair.source_store.claim_owner_lease (replacement.owner_id, 15s)
              .result ()
              .value ();
            (void) pair.source_store
              .update_mesh_node (replacement, fw::location_write_intent_t::new_claim)
              .result ()
              .value ();
            pair.target->transport ().expect_peer (pair.source_wire);
        }
        (void) pair.source_reader.poll_mesh_sources (mesh_name).result ().value ();
        const auto authority =
          pair.target->accepted_record_authority (*query, pair.lease, &pair.source_reader)
            .result ()
            .value ();
        ASSERT_TRUE (authority);
        EXPECT_EQ (authority->source.owner_id, pair.original->owner_id);
        EXPECT_EQ (authority->source.lease_generation, 1u);
        EXPECT_EQ (authority->source.node_routing_id, pair.source_wire.node_routing_id);
        EXPECT_EQ (authority->source.node_generation, pair.source_wire.lifecycle_generation);
        held.complete (
          fw::result_t<std::optional<stateful::accepted_record_authority_t>>::success (authority));
    }
}

TEST (RelayAdmission, FirstBoundSessionRequest)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t entry, session;
    release_callbacks_t release{evidence};
    constexpr auto target_rid = "tableA-relay-admission";
    configure (entry, evidence_owner, target_rid, store, relocations);
    entry.start ();
    ASSERT_TRUE (ready (entry));
    auto services = entry.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    configure_session (session, evidence_owner, store, relocations);
    session.start ();
    ASSERT_TRUE (ready (session));
    auto session_services = session.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (
      peer_ready (session_services.get_required<fw::route_mesh_runtime_t> (), target_rid));
    view_client_t client (session_services.get_required<fw::framework_runtime_t> ()
                            .listener_status (fw::listener_kind_t::stream, stream_name)
                            .endpoint);
    ASSERT_TRUE (client.connect ());
    EXPECT_EQ (client.view (), std::optional<int>{1});
    std::unique_lock lock (evidence.mutex);
    EXPECT_TRUE (
      evidence.changed.wait_for (lock, wait_budget, [&] { return evidence.relay_completed; }));
}
} // namespace zlink::framework::test::actor_maintenance_join
