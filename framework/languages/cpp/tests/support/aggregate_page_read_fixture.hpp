/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "scope_exit.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/stateful/public_store_adapters.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace zlink::framework::test
{
using namespace zlink::framework::runtime;
using namespace std::chrono_literals;

class paused_aggregate_commit_store_t final : public location_store_t
{
  public:
    explicit paused_aggregate_commit_store_t (location_store_t &store) : inner (store) {}
    task_t<store_read_result_t> read (store_key_t key) override
    {
        return inner.read (std::move (key));
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        const bool committing = std::ranges::any_of (request.mutations, [] (const auto &mutation) {
            const auto *put = std::get_if<store_put_t> (&mutation);
            if (!put || !put->key.value.starts_with ("zlink:v11:aggregate:"))
                return false;
            const auto record = nlohmann::json::parse (put->bytes);
            return record.at ("state") == "committed";
        });
        if (committing) {
            commit_waiting.complete (result_t<bool>::success (true));
            co_await release.task ();
        }
        co_return co_await inner.write (std::move (request));
    }
    location_store_t &inner;
    task_completion_source_t<bool> commit_waiting;
    task_completion_source_t<void> release;
};

inline task_t<aggregate_commit_result_t>
observe_aggregate_commit (task_t<aggregate_commit_result_t> commit,
                          task_completion_source_t<bool> &commit_waiting)
{
    scope_exit_t signal_terminal (
      [&] { commit_waiting.complete (result_t<bool>::success (false)); });
    co_return co_await commit;
}

template <typename TBytes>
void aggregate_read_keeps_source_version_until_commit (
  location_store_t &store,
  TBytes bytes,
  authority_aggregate_codec::json_t relocation_record = nullptr)
{
    namespace stateful = zlink::framework::runtime::stateful;
    paused_aggregate_commit_store_t provider (store);
    provider_location_repository_t repository (provider);
    repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto make_target = [&] (const std::string &id) {
        const auto claim = repository.claim_owner_lease (id, 30s).result ().value ();
        const auto token = std::get<owner_lease_claimed_t> (claim).token;
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "page-read";
        descriptor.rid = zlink::routing_id_t::from (id);
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = token.owner_id;
        descriptor.lease_generation = token.lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.object_capabilities.push_back ({placement_object_kind_t::user_spot, "room",
                                                   maintenance_policy_kind_t::snapshot, true, 10});
        descriptor.object_capabilities.push_back ({placement_object_kind_t::actor, "player",
                                                   maintenance_policy_kind_t::recreate, false, 0});
        descriptor.capacity.actors.limit = 10;
        descriptor.capacity.spots.limit = 10;
        descriptor.capacity.spot_types.push_back (
          {placement_object_kind_t::user_spot, "room", {0, 0, 10}});
        EXPECT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
        return object_creation_target_t{descriptor.mesh_name, node_rid_t::from_string (id), 1,
                                        token};
    };
    const auto source = make_target ("page-source");
    const auto target = make_target ("page-target");
    object_reserve_request_t request;
    request.key = {placement_object_kind_t::user_spot, "page-spot"};
    request.intent.stable_type = "room";
    request.target = source;
    request.creating_payload = bytes ("creating");
    request.capacity_bundle.spot_slots = 1;
    request.capacity_bundle.spot_type =
      spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
    const auto reserved = repository.reserve (request).result ().value ();
    const auto reservation = std::get<object_reserved_t> (reserved);
    const auto committed =
      repository.commit ({request.key, reservation.fence, bytes ("before")}).result ().value ();
    auto before = std::get<object_committed_t> (committed).ready;
    object_reserve_request_t actor_request;
    actor_request.key = {placement_object_kind_t::actor, "page-actor"};
    actor_request.intent.stable_type = "player";
    actor_request.target = source;
    actor_request.creating_payload = bytes ("creating");
    actor_request.capacity_bundle.actor_slots = 1;
    const auto actor_reserved = repository.reserve (actor_request).result ().value ();
    const auto actor_reservation = std::get<object_reserved_t> (actor_reserved);
    const auto actor_committed =
      repository.commit ({actor_request.key, actor_reservation.fence, bytes ("actor-before")})
        .result ()
        .value ();
    const auto actor_before = std::get<object_committed_t> (actor_committed).ready;
    const auto key = spot_authority_key (request.key.global_id);
    const auto original_version = before.store_version;
    protocol::relocation_id_t relocation{1, 1};
    std::uint64_t target_attempt = 0;
    if (!relocation_record.is_null ()) {
        auto &record = relocation_record["relocation"];
        relocation = {0x0f1e2d3c4b5a4968, 0x8778a1b2c3d4e5f6};
        auto &source_fence = record["source"];
        source_fence["nodeRid"] = zlink::routing_id_t::from ("page-source").to_hex ();
        source_fence["nodeGeneration"] = "1";
        source_fence["ownerId"] = source.owner.owner_id;
        source_fence["ownerLeaseGeneration"] = std::to_string (source.owner.lease_generation);
        record["coordinator"] = source_fence;
        record["coordinator"]["expectedStoreVersion"] = original_version;
        if (record.at ("phase") == "prepared") {
            target_attempt =
              authority_aggregate_codec::number (record.at ("target"), "attemptGeneration");
            auto &target_fence = record["target"];
            target_fence["nodeRid"] = zlink::routing_id_t::from ("page-target").to_hex ();
            target_fence["nodeGeneration"] = "1";
            target_fence["ownerId"] = target.owner.owner_id;
            target_fence["ownerLeaseGeneration"] = std::to_string (target.owner.lease_generation);
        }
        authority_put_t put{before.payload};
        put.replace_aggregate = authority_aggregate_codec::read (relocation_record);
        const auto result =
          repository.compare_exchange_authority (key, original_version, std::move (put))
            .result ()
            .value ();
        ASSERT_TRUE (std::holds_alternative<authority_stored_t> (result));
        before = std::get<authority_stored_t> (result).snapshot;
    }
    stateful::object_ref_t participant{stateful::object_kind_t::user_spot,
                                       request.key.global_id,
                                       before.object_generation,
                                       before.authority_owner_generation,
                                       source.mesh_name,
                                       std::string (source.node_rid.value ())};
    stateful::relocation_authority_fence_t source_fence{participant.kind,
                                                        participant.key,
                                                        original_version,
                                                        source.owner,
                                                        target.owner,
                                                        relocation,
                                                        {},
                                                        0,
                                                        target_attempt};
    stateful::public_authority_store_adapter_t adapter (repository);
    aggregate_prepare_request_t aggregate;
    aggregate.aggregate_id.value[15] = std::byte{0x63};
    aggregate.aggregate_generation = 1;
    aggregate.participants = {{actor_authority_key (actor_request.key.global_id),
                               actor_before.store_version,
                               authority_generation_transition_t::new_owner,
                               bytes ("actor-after"),
                               {}},
                              {key,
                               before.store_version,
                               authority_generation_transition_t::new_owner,
                               bytes ("after"),
                               {}}};
    aggregate.target_descriptor = {
      target.mesh_name, zlink::routing_id_t::from (std::string (target.node_rid.value ()))};
    aggregate.target_descriptor_lifecycle_generation = 1;
    aggregate.target_owner = target.owner;
    aggregate.capacity_bundle = request.capacity_bundle;
    aggregate.capacity_bundle.actor_slots = 1;
    const auto prepared = repository.prepare_aggregate (aggregate).result ().value ();
    ASSERT_TRUE (std::holds_alternative<aggregate_prepared_t> (prepared))
      << "prepare result variant " << prepared.index ();
    const auto fence = std::get<aggregate_prepared_t> (prepared).fence;
    auto pending =
      observe_aggregate_commit (repository.commit_aggregate (fence), provider.commit_waiting);
    scope_exit_t release_commit ([&] {
        provider.release.complete (result_t<void>::success ());
        (void) pending.result ();
    });
    ASSERT_TRUE (provider.commit_waiting.task ().result ().value ());
    EXPECT_FALSE (pending.await_ready ());
    // Release the commit even if an observation fails, so the fixture owns no pending work.
    const auto read = repository.read_authority (key).result ().value ();
    const auto during = std::get<authority_snapshot_t> (read);
    EXPECT_EQ (during.payload, before.payload);
    EXPECT_EQ (during.owner.owner_id, before.owner.owner_id);
    EXPECT_EQ (during.owner.lease_generation, before.owner.lease_generation);
    EXPECT_EQ (during.store_version, before.store_version);
    EXPECT_EQ (during.object_generation, before.object_generation);
    EXPECT_EQ (during.authority_owner_generation, before.authority_owner_generation);
    auto cursor = std::optional<authority_scan_cursor_t>{};
    bool scanned = false;
    do {
        const auto scan = repository.list_authorities ("zla1:s:", cursor, 10).result ().value ();
        const auto &page = std::get<authority_page_t> (scan);
        for (const auto &entry : page.items) {
            if (entry.key.value == key.value) {
                scanned = true;
                EXPECT_EQ (entry.snapshot.store_version, before.store_version);
                EXPECT_EQ (entry.snapshot.payload, before.payload);
            }
        }
        cursor = page.next_cursor;
    } while (cursor);
    EXPECT_TRUE (scanned);
    EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (
      repository
        .compare_exchange_authority (key, before.store_version, authority_put_t{bytes ("preserve")})
        .result ()
        .value ()));
    auto settlement = adapter.settle_source_relocation (source_fence, {participant}, false);
    EXPECT_EQ (settlement.outcome, stateful::relocation_authority_t::unsettled)
      << "A prepared authority must remain unsettled before commit";
    release_commit.run ();
    EXPECT_EQ (pending.result ().value (), aggregate_commit_result_t::committed);
    // Source settlement remains eligible to observe the terminal target owner.
    if (settlement.outcome == stateful::relocation_authority_t::unsettled)
        settlement = adapter.settle_source_relocation (source_fence, {participant}, false);
    EXPECT_EQ (settlement.outcome, stateful::relocation_authority_t::target_committed)
      << "Source settlement must remain open to the completed target commit";
    if (!relocation_record.is_null ()) {
        const auto target_read = repository.read_authority (key).result ().value ();
        const auto &target_authority = std::get<authority_snapshot_t> (target_read);
        EXPECT_FALSE (target_authority.aggregate && target_authority.aggregate->relocation);
    }
}

} // namespace zlink::framework::test
