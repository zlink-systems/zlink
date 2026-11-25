/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/actor_maintenance_join_location_store_fixtures.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace zlink::framework::test::actor_maintenance_join
{
TEST (ActorMaintenanceJoin, ReadyAuthorityAcceptsLocalSendBeforeStoreWriteReturns)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto store = std::make_shared<observed_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    auto observed = std::make_shared<std::atomic<bool>> (false);
    auto sent = std::make_shared<fw::task_completion_source_t<void>> ();
    host_t source, target;
    configure (source, evidence_owner, first_rid, store, relocations, source_follow_duration);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    configure (target, evidence_owner, second_rid, store, relocations, source_follow_duration);
    target.start ();
    ASSERT_TRUE (ready (target));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> ()));
    auto target_services = target.app.advanced ().services ().build_provider ();
    auto &client = target_services.get_required<fw::actor_client_t> ();
    // Callback copies own services/results; evidence outlives both hosts.
    store->observe ([target_services, observed, sent,
                     evidence_owner] () mutable -> fw::task_t<void> {
        const auto location = co_await target_services.get_required<fw::location_runtime_query_t> ()
                                .find_actor_location (fw::actor_id_t (actor_id));
        if (!location || location->state != fw::location_object_state_t::ready
            || location->node_rid.to_string () != second_rid)
            co_return;
        if (!observed->exchange (true)) {
            std::fprintf (
              stderr, "Store write boundary: Ready ActorId=%s NodeRid=%s ObjectGeneration=%llu\n",
              location->global_id.c_str (), location->node_rid.to_string ().c_str (),
              static_cast<unsigned long long> (location->object_generation));
            const auto admitted = co_await fw::runtime::await_result (
              target_services.get_required<fw::actor_client_t> ()
                .send (fw::actor_id_t (actor_id), record_t{1})
                .async ());
            {
                std::lock_guard lock (evidence_owner->mutex);
                EXPECT_TRUE (evidence_owner->recorded.empty ());
            }
            sent->complete (admitted);
        }
        co_await sent->task ();
    });
    const auto relocated =
      source.app
        .relocate ({.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget})
        .result ();
    store->observe ({});
    ASSERT_TRUE (observed->load ());
    ASSERT_TRUE (sent->task ().await_ready ());
    const auto admitted = sent->task ().result ();
    ASSERT_TRUE (admitted) << (admitted.error () ? admitted.error ()->what () : "");
    ASSERT_TRUE (relocated);
    EXPECT_EQ (relocated.value ().outcome, fw::relocation_outcome_t::relocated);
    const auto reply = client.request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                         .timeout (wait_budget)
                         .async<probe_t> ()
                         .result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (reply.value ().value, 2);
    std::unique_lock lock (evidence.mutex);
    EXPECT_TRUE (evidence.changed.wait_for (
      lock, wait_budget, [&] { return evidence.recorded == std::vector<int>{1}; }));
}

TEST (ActorMaintenanceJoin, TargetRejectsChangedCoordinatorFence)
{
    namespace stateful = fw::runtime::stateful;
    // A join commit has no relocation: only the source StoreVersion fences it.
    for (const auto changed :
         {"none", "owner", "lease", "baseline", "empty-baseline", "join", "join-baseline"}) {
        SCOPED_TRACE (changed);
        auto evidence = std::make_shared<observations_t> ();
        auto store = std::make_shared<observed_location_store_t> ();
        auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
        host_t source, target;
        configure (source, evidence, first_rid, store, relocations, source_follow_duration);
        source.start ();
        ASSERT_TRUE (ready (source));
        auto services = source.app.advanced ().services ().build_provider ();
        ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                       .get_or_create (fw::actor_id_t (actor_id), actor_type)
                       .timeout (wait_budget)
                       .async ()
                       .result ());
        configure (target, evidence, second_rid, store, relocations, source_follow_duration);
        target.start ();
        ASSERT_TRUE (ready (target));
        auto &repository = services.get_required<fw::location_repository_t> ();
        const auto key = fw::runtime::actor_authority_key (actor_id);
        const auto original =
          std::get<fw::authority_snapshot_t> (repository.read_authority (key).result ().value ());
        const auto nodes =
          repository.list_mesh_nodes (original.allocation.target.mesh_name).result ().value ();
        const auto destination =
          std::find_if (nodes.items.begin (), nodes.items.end (),
                        [] (const auto &node) { return node.rid.to_string () == second_rid; });
        ASSERT_NE (destination, nodes.items.end ());
        const fw::location_owner_token_t target_owner{destination->owner_id,
                                                      destination->lease_generation};
        const fw::object_creation_target_t placement{
          destination->mesh_name, fw::node_rid_t::from_string (second_rid),
          destination->lifecycle_generation, target_owner};
        const stateful::object_ref_t source_ref{
          stateful::object_kind_t::actor,       actor_id,
          original.object_generation,           original.authority_owner_generation,
          original.allocation.target.mesh_name, first_rid};
        auto target_ref = source_ref;
        target_ref.node_id = second_rid;
        auto payload = fw::runtime::decode_direct_actor_authority_payload (original.payload);
        ASSERT_TRUE (payload);
        payload->node_rid = placement.node_rid;
        payload->node_generation = placement.node_lifecycle_generation;
        payload->owner_id = target_owner.owner_id;
        payload->owner_lease_generation = target_owner.lease_generation;
        auto coordinator = original.owner;
        auto baseline = original.store_version;
        if (std::string_view (changed) == "owner")
            coordinator.owner_id += "-other";
        if (std::string_view (changed) == "lease")
            ++coordinator.lease_generation;
        const bool join = std::string_view (changed).starts_with ("join");
        if (std::string_view (changed) == "baseline"
            || std::string_view (changed) == "join-baseline")
            baseline += "-other";
        if (std::string_view (changed) == "empty-baseline")
            baseline.clear ();
        stateful::public_authority_store_adapter_t adapter (repository);
        const stateful::relocation_authority_fence_t fence{
          source_ref.kind, source_ref.key, baseline, coordinator, target_owner, {4718, 2}};
        const bool accepted =
          std::string_view (changed) == "none" || std::string_view (changed) == "join";
        if (!join)
            EXPECT_EQ (adapter.observe_relocation (fence),
                       accepted ? stateful::relocation_authority_t::unsettled
                                : stateful::relocation_authority_t::source_preserved);
        const auto published =
          adapter.publish (source_ref, target_ref, target_owner, placement, "target-fence", 1, {},
                           fw::runtime::encode_actor_authority_payload (*payload), baseline,
                           join ? decltype (fence.relocation){} : fence.relocation,
                           join ? fw::location_owner_token_t{} : coordinator);
        EXPECT_EQ (published.status, accepted ? stateful::authority_publish_status_t::published
                                              : stateful::authority_publish_status_t::conflict);
        const auto after =
          std::get<fw::authority_snapshot_t> (repository.read_authority (key).result ().value ());
        if (!accepted) {
            EXPECT_EQ (after.store_version, original.store_version);
            EXPECT_EQ (after.owner.owner_id, original.owner.owner_id);
            EXPECT_EQ (after.owner.lease_generation, original.owner.lease_generation);
            EXPECT_EQ (after.authority_owner_generation, original.authority_owner_generation);
        }
        std::lock_guard lock (evidence->mutex);
        EXPECT_TRUE (evidence->recorded.empty ());
    }
}

TEST (ActorMaintenanceJoin, SourcePreserveDiscardsRawIdentityPreparedBeforeTargetCas)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto store = std::make_shared<observed_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source, target;
    configure (source, evidence_owner, first_rid, store, relocations, source_follow_duration);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    auto &repository = services.get_required<fw::location_repository_t> ();
    const auto key = fw::runtime::actor_authority_key (actor_id);
    const auto original =
      std::get<fw::authority_snapshot_t> (repository.read_authority (key).result ().value ());
    configure (target, evidence_owner, second_rid, store, relocations, source_follow_duration);
    target.start ();
    ASSERT_TRUE (ready (target));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> ()));
    auto target_services = target.app.advanced ().services ().build_provider ();
    auto &query = target_services.get_required<fw::location_runtime_query_t> ();
    const auto state =
      fw::detail::mesh_node_runtime_t::registrations (target.app.advanced ().zlink ())
        .front ()
        ->spot_state;
    const auto native = fw::detail::spot_node_runtime_t (state).native_node ();
    const auto candidate = fw::runtime::host::public_host_runtime_t::remote_actor_ref (
      zlink::routing_id_t::from (second_rid), actor_id, original.object_generation);
    auto preserved = std::make_shared<std::promise<bool>> ();
    auto preservation = preserved->get_future ();
    store->before_next_write_when (
      [native, candidate, target_services, services,
       key] (const fw::store_write_request_t &request) mutable -> fw::task_t<bool> {
          // Read-only observation of the raw index; the failure is injected
          // through the Store provider, without a relocation runtime hook.
          if (!native->resolve_actor_async (candidate).result ().value ())
              co_return false;
          const auto location =
            co_await target_services.get_required<fw::location_runtime_query_t> ()
              .find_actor_location (fw::actor_id_t (actor_id));
          if (!location || location->node_rid.to_string () != first_rid)
              co_return false;
          const auto current = std::get<fw::authority_snapshot_t> (
            co_await services.get_required<fw::location_repository_t> ().read_authority (key));
          // This in-memory Store issues globally distinct versions. Match
          // the Actor row condition without decoding provider keys/payloads.
          co_return std::ranges::any_of (request.conditions, [&] (const auto &condition) {
              const auto *version = std::get_if<fw::store_version_condition_t> (&condition);
              return version && version->expected.value == current.store_version;
          });
      },
      [services, key, payload = original.payload, preserved] () mutable -> fw::task_t<void> {
          const auto current = std::get<fw::authority_snapshot_t> (
            co_await services.get_required<fw::location_repository_t> ().read_authority (key));
          // Source Preserve clears the captured attempt in the same CAS
          // that restores the source payload (Location §10).
          fw::authority_put_t preserve{payload};
          preserve.replace_aggregate =
            fw::runtime::authority_aggregate_codec::without_capture (current.aggregate);
          preserved->set_value (std::holds_alternative<fw::authority_stored_t> (
            co_await services.get_required<fw::location_repository_t> ()
              .compare_exchange_authority (key, current.store_version, std::move (preserve))));
      });
    const auto relocated =
      source.app
        .relocate ({.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget})
        .result ();
    store->before_next_write_when ({}, {});
    ASSERT_EQ (preservation.wait_for (wait_budget), std::future_status::ready);
    ASSERT_TRUE (preservation.get ());
    ASSERT_TRUE (relocated);
    EXPECT_NE (relocated.value ().outcome, fw::relocation_outcome_t::relocated);
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget, [&] {
            return std::ranges::find (evidence.destroyed, 2) != evidence.destroyed.end ();
        }));
    }
    EXPECT_TRUE (store->intercepted_write_conflicted ());
    EXPECT_FALSE (native->resolve_actor_async (candidate).result ().value ());
    native->seal_relocation_targets ().result ().value ();
    EXPECT_FALSE (native->objects ().find (fw::runtime::stateful::object_kind_t::actor, actor_id));
    const auto location = query.find_actor_location (fw::actor_id_t (actor_id)).result ().value ();
    ASSERT_TRUE (location);
    EXPECT_EQ (location->node_rid.to_string (), first_rid);
}

TEST (ActorMaintenanceJoin, ClosingCasPreventsInFlightReadyRouteRefill)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto store = std::make_shared<observed_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t host;
    release_callbacks_t release{*evidence_owner};
    configure (host, evidence_owner, first_rid, store, relocations);
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    auto &spots = services.get_required<fw::spot_manager_t> ();
    const auto created = spots.get_or_create (fw::spot_id_t (local_spot), room_type)
                           .timeout (wait_budget)
                           .async ()
                           .result ();
    ASSERT_TRUE (created);
    struct holds_t
    {
        fw::task_completion_source_t<void> read_entered, release_read, closing_entered,
          release_close;
    };
    const auto holds = std::make_shared<holds_t> ();
    const std::shared_ptr<void> cleanup (nullptr, [holds] (void *) {
        holds->release_read.complete (fw::result_t<void>::success ());
        holds->release_close.complete (fw::result_t<void>::success ());
    });
    {
        std::lock_guard lock (evidence_owner->mutex);
        evidence_owner->closing_hook = [holds] () -> fw::task_t<void> {
            holds->closing_entered.complete (fw::result_t<void>::success ());
            co_await holds->release_close.task ();
        };
    }
    auto &resolver = services.get_required<fw::runtime::store_location_resolvers_t> ();
    resolver.invalidate_spot_address (local_spot);
    const auto raw_key = std::string ("authority\0spot\0", 15) + local_spot;
    store->after_next_read (raw_key, [holds] () -> fw::task_t<void> {
        holds->read_entered.complete (fw::result_t<void>::success ());
        co_await holds->release_read.task ();
    });
    auto reading = resolver.resolve_spot_address ({}, local_spot);
    ASSERT_TRUE (holds->read_entered.task ().result_for (wait_budget));
    auto closing = spots.close (created.value ().spot);
    ASSERT_TRUE (holds->closing_entered.task ().result_for (wait_budget));
    ASSERT_FALSE (closing.await_ready ());
    holds->release_read.complete (fw::result_t<void>::success ());
    const auto old = reading.result ();
    ASSERT_TRUE (old);
    ASSERT_TRUE (old.value ());
    EXPECT_EQ (old.value ()->authority_state, fw::runtime::user_spot_authority_state_t::ready);
    store->observe_authority_reads (raw_key);
    const auto current = resolver.resolve_spot_address ({}, local_spot).result ();
    ASSERT_TRUE (current);
    ASSERT_TRUE (current.value ());
    EXPECT_EQ (current.value ()->authority_state,
               fw::runtime::user_spot_authority_state_t::closing);
    EXPECT_NE (current.value ()->store_version, old.value ()->store_version);
    EXPECT_GT (store->accepted_store_calls ().first, 0u);
    holds->release_close.complete (fw::result_t<void>::success ());
    ASSERT_TRUE (closing.result ());
}

TEST (ActorMaintenanceJoin, RemoteClosedSpotJoinInvalidatesWarmRoute)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto store = std::make_shared<observed_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source, target;
    release_callbacks_t release{evidence};
    configure (source, evidence_owner, first_rid, store, relocations);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    configure (target, evidence_owner, second_rid, store, relocations);
    target.start ();
    ASSERT_TRUE (ready (target));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), second_rid));
    auto target_services = target.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (set_published_placement_weight (services, 0, &target_services));
    auto &spots = target_services.get_required<fw::spot_manager_t> ();
    const auto created = spots.get_or_create (fw::spot_id_t (local_spot), room_type)
                           .timeout (wait_budget)
                           .async ()
                           .result ();
    ASSERT_TRUE (created);
    ASSERT_EQ (created.value ().spot.node_rid ().value (), second_rid);
    auto &resolver = services.get_required<fw::runtime::store_location_resolvers_t> ();
    const auto warm = resolver.resolve_spot_address ({}, local_spot).result ();
    ASSERT_TRUE (warm);
    ASSERT_TRUE (warm.value ());
    ASSERT_EQ (warm.value ()->node_rid.to_string (), second_rid);
    const auto closed = spots.close (created.value ().spot).result ();
    ASSERT_TRUE (closed);
    ASSERT_TRUE (closed.value ());
    auto &client = services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (move_to (client, evidence, local_spot, 1));
    {
        std::lock_guard lock (evidence.mutex);
        const auto *failed = std::get_if<fw::actor_join_failed_t> (&evidence.completions.back ());
        ASSERT_NE (failed, nullptr);
        EXPECT_EQ (failed->error_kind, fw::framework_error_kind_t::unavailable);
    }
    store->observe_authority_reads (std::string ("authority\0spot\0", 15) + local_spot);
    ASSERT_TRUE (move_to (client, evidence, local_spot, 2));
    {
        std::lock_guard lock (evidence.mutex);
        const auto *failed = std::get_if<fw::actor_join_failed_t> (&evidence.completions.back ());
        ASSERT_NE (failed, nullptr);
        EXPECT_EQ (failed->error_kind, fw::framework_error_kind_t::not_found);
    }
    EXPECT_GT (store->accepted_store_calls ().first, 0u);
}
} // namespace zlink::framework::test::actor_maintenance_join
