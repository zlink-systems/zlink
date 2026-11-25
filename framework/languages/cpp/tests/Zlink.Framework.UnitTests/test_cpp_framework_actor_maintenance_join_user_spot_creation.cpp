/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "../support/owner_lease_time_store.hpp"
#include "../support/location_reclaim_clock.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace zlink::framework::test::actor_maintenance_join
{
TEST (ActorDirectRequest, CachedIdReachesRecreatedActorOnTheSameOwner)
{
    auto evidence = std::make_shared<observations_t> ();
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t owner, caller;
    configure (owner, evidence, "recreated-owner", store, relocations);
    owner.start ();
    ASSERT_TRUE (ready (owner));
    auto owner_services = owner.app.advanced ().services ().build_provider ();
    auto &actors = owner_services.get_required<fw::actor_manager_t> ();
    ASSERT_TRUE (actors.get_or_create (fw::actor_id_t (actor_id), actor_type).async ().result ());
    const auto previous = actors.find (fw::actor_id_t (actor_id)).result ();
    ASSERT_TRUE (previous);
    ASSERT_TRUE (previous.value ());
    auto &options = caller.app.add_zlink_framework ();
    options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
    options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
    options.add_location_store (store);
    options.add_relocation_store (relocations);
    caller.app.logging ().use_file ("recreated-client.flow");
    options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::detailed);
    options.add_route_mesh (mesh_name)
      .set_object_role (fw::object_role_t::client)
      .set_routing_id (zlink::routing_id_t::from ("recreated-client"))
      .listen ("tcp://127.0.0.1:0");
    caller.start ();
    ASSERT_TRUE (ready (caller));
    auto client_services = caller.app.advanced ().services ().build_provider ();
    auto &client = client_services.get_required<fw::actor_client_t> ();
    const auto first = client.request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                         .timeout (wait_budget)
                         .async<probe_t> ()
                         .result ();
    ASSERT_TRUE (first);
    ASSERT_TRUE (actors.destroy (*previous.value ()).result ().value ());
    ASSERT_TRUE (actors.get_or_create (fw::actor_id_t (actor_id), actor_type).async ().result ());
    const auto current = actors.find (fw::actor_id_t (actor_id)).result ();
    ASSERT_TRUE (current);
    ASSERT_TRUE (current.value ());
    ASSERT_NE (previous.value ()->object_generation (), current.value ()->object_generation ());
    ASSERT_EQ (previous.value ()->node_rid ().value (), current.value ()->node_rid ().value ());
    ASSERT_TRUE (
      client.send (fw::actor_id_t (actor_id), record_t{instance_probe}).async ().result ());
    {
        std::unique_lock lock (evidence->mutex);
        ASSERT_TRUE (evidence->changed.wait_for (lock, wait_budget,
                                                 [&] { return !evidence->recorded.empty (); }));
        EXPECT_NE (evidence->recorded.front (), first.value ().value);
    }
    const auto second = client.request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                          .timeout (wait_budget)
                          .async<probe_t> ()
                          .result ();
    ASSERT_TRUE (second);
    EXPECT_NE (first.value ().value, second.value ().value);
    {
        std::lock_guard lock (evidence->mutex);
        EXPECT_EQ (evidence->recorded.front (), second.value ().value);
    }
}

TEST (UserSpotCreation, ExistingReadyDoesNotRequireNewCapacity)
{
    auto evidence = std::make_shared<observations_t> ();
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t host;
    configure (host, evidence, first_rid, store, relocations, default_follow_duration, 1);
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    auto &spots = services.get_required<fw::spot_manager_t> ();
    const auto created = spots.get_or_create (fw::spot_id_t ("capacity-room"), room_type)
                           .timeout (wait_budget)
                           .async ()
                           .result ();
    ASSERT_TRUE (created);
    ASSERT_EQ (created.value ().state, fw::spot_create_state_t::created);
    auto &resolver = services.get_required<fw::runtime::store_location_resolvers_t> ();
    const auto descriptors = resolver.list_live_mesh_nodes (mesh_name).result ();
    ASSERT_TRUE (descriptors);
    ASSERT_EQ (descriptors.value ().size (), 1u);
    const auto &types = descriptors.value ().front ().capacity.spot_types;
    const auto full = std::find_if (types.begin (), types.end (), [] (const auto &type) {
        return type.object_kind == fw::placement_object_kind_t::user_spot
               && type.stable_type == room_type;
    });
    ASSERT_NE (full, types.end ());
    ASSERT_EQ (full->usage.limit, 1);
    ASSERT_EQ (full->usage.active, 1u);
    ASSERT_EQ (full->usage.reserved, 0u);
    const auto other = spots.get_or_create (fw::spot_id_t ("another-capacity-room"), room_type)
                         .timeout (wait_budget)
                         .async ()
                         .result ();
    ASSERT_FALSE (other);
    EXPECT_EQ (other.error_kind (), fw::framework_error_kind_t::unavailable);
    const auto existing = spots.get_or_create (fw::spot_id_t ("capacity-room"), room_type)
                            .timeout (wait_budget)
                            .async ()
                            .result ();
    ASSERT_TRUE (existing) << (existing.error () ? existing.error ()->what () : "no error");
    EXPECT_EQ (existing.value ().state, fw::spot_create_state_t::existing);
    EXPECT_EQ (existing.value ().spot.spot_id (), created.value ().spot.spot_id ());
    EXPECT_EQ (existing.value ().spot.object_generation (),
               created.value ().spot.object_generation ());
    EXPECT_EQ (existing.value ().spot.mesh_name (), created.value ().spot.mesh_name ());
    EXPECT_EQ (existing.value ().spot.node_rid ().value (),
               created.value ().spot.node_rid ().value ());
    std::lock_guard lock (evidence->mutex);
    EXPECT_EQ (evidence->room_factories, 1);
}


enum class user_spot_authority_case_t
{
    expired_ready,
    expired_retained,
    creating_target_ended
};

void user_spot_authority_qualification (user_spot_authority_case_t scenario)
{
    using namespace fw::runtime;
    const bool creating = scenario == user_spot_authority_case_t::creating_target_ended;
    const bool retained = scenario == user_spot_authority_case_t::expired_retained;
    auto inner = std::make_shared<in_memory_location_store_t> ();
    provider_location_repository_t original (*inner);
    original.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    constexpr auto owner_lease_ttl = 60s;
    const auto claimed =
      original.claim_owner_lease ("expired-ready-owner", owner_lease_ttl).result ();
    ASSERT_TRUE (claimed);
    const auto owner = std::get<fw::owner_lease_claimed_t> (claimed.value ()).token;
    fw::mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = mesh_name;
    descriptor.rid = zlink::routing_id_t::from ("expired-ready-target");
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = owner.owner_id;
    descriptor.lease_generation = owner.lease_generation;
    descriptor.object_role = fw::object_role_t::server;
    descriptor.state = fw::framework_runtime_state_t::serving;
    descriptor.security_identity = "expired-ready-target";
    descriptor.entry_spot_id = "expired-ready-entry";
    descriptor.object_capabilities.push_back ({fw::placement_object_kind_t::user_spot, room_type,
                                               fw::maintenance_policy_kind_t::disabled, false, 1});
    descriptor.capacity.spots.limit = 1;
    descriptor.capacity.spot_types.push_back (
      {fw::placement_object_kind_t::user_spot, room_type, {0, 0, 1}});
    ASSERT_EQ (original.update_mesh_node (descriptor, fw::location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               fw::location_write_status_t::stored);
    fw::object_reserve_request_t request;
    request.key = {fw::placement_object_kind_t::user_spot, "expired-ready-room"};
    request.intent.stable_type = room_type;
    request.target = {mesh_name, fw::node_rid_t::from_string (descriptor.rid.to_string ()), 1,
                      owner};
    request.capacity_bundle = {
      0, 1, fw::spot_type_capacity_delta_t{fw::placement_object_kind_t::user_spot, room_type, 1}};
    const auto reservation = original.reserve (request).result ();
    ASSERT_TRUE (reservation);
    const auto *reserved = std::get_if<fw::object_reserved_t> (&reservation.value ());
    ASSERT_NE (reserved, nullptr);
    if (creating) {
        ASSERT_EQ (original.remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, owner)
                     .result ()
                     .value (),
                   fw::location_write_status_t::stored);
    } else {
        const auto payload = encode_user_spot_authority_payload (user_spot_authority_payload_t{
          .state = user_spot_authority_state_t::ready,
          .stable_type = room_type,
          .spot_id = request.key.global_id,
          .owner_id = owner.owner_id,
          .owner_lease_generation = static_cast<std::uint64_t> (owner.lease_generation),
          .mesh_name = mesh_name,
          .node_rid = request.target.node_rid,
          .node_generation = 1});
        const auto committed = original.commit ({request.key, reserved->fence, payload}).result ();
        ASSERT_TRUE (committed);
        ASSERT_TRUE (std::holds_alternative<fw::object_committed_t> (committed.value ()));
    }
    std::shared_ptr<fw::location_store_t> view = inner;
    if (!creating) {
        auto clock = std::make_shared<fw::tests::owner_lease_time_store_t> (
          *inner, owner.owner_id,
          fw::tests::owner_lease_time_store_t::lease_view_t::clock_advanced);
        clock->advance_store_clock (owner_lease_ttl + 1s);
        const auto lease_key = std::string ("owner-lease") + '\0' + owner.owner_id;
        const auto lease_read = clock->read ({lease_key}).result ();
        ASSERT_TRUE (lease_read);
        const auto *lease = std::get_if<fw::store_found_t> (&lease_read.value ());
        ASSERT_NE (lease, nullptr);
        ASSERT_TRUE (lease->value.expires_at);
        EXPECT_LT (*lease->value.expires_at, lease->value.store_now);
        view = std::move (clock);
    }
    auto evidence = std::make_shared<observations_t> ();
    host_t host;
    // Only the relocation-enabled case has an eligible placement target.
    configure (host, evidence, first_rid, view, std::make_shared<in_memory_relocation_store_t> (),
               default_follow_duration, 1, retained, entry_kind_t::member, retained ? 1 : 0);
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    auto &source_repository = services.get_required<fw::location_repository_t> ();
    live_location_reader_t source_lease_view (source_repository);
    const auto owner_live = source_lease_view.owner_available (owner).result ();
    ASSERT_TRUE (owner_live);
    ASSERT_EQ (owner_live.value (), creating);
    const auto observed =
      source_repository.read_authority (spot_authority_key (request.key.global_id)).result ();
    ASSERT_TRUE (observed);
    ASSERT_TRUE (std::holds_alternative<fw::authority_snapshot_t> (observed.value ()));
    fw::location_reclaim_test_access_t reclaim_clock;
    reclaim_clock.bind (source_repository);
    if (!creating && !retained) {
        const auto waiting = services.get_required<fw::spot_manager_t> ()
                               .get_or_create (request.key.global_id, room_type)
                               .timeout (wait_budget)
                               .async ()
                               .result ();
        ASSERT_FALSE (waiting);
        EXPECT_EQ (waiting.error_kind (), fw::framework_error_kind_t::unavailable);
        const auto unchanged =
          source_repository.read_authority (spot_authority_key (request.key.global_id)).result ();
        ASSERT_TRUE (unchanged);
        ASSERT_TRUE (std::holds_alternative<fw::authority_snapshot_t> (unchanged.value ()));
        EXPECT_EQ (std::get<fw::authority_snapshot_t> (unchanged.value ()).store_version,
                   std::get<fw::authority_snapshot_t> (observed.value ()).store_version);
        reclaim_clock.advance (15s);
    }
    const auto result = services.get_required<fw::spot_manager_t> ()
                          .get_or_create (request.key.global_id, room_type)
                          .timeout (wait_budget)
                          .async ()
                          .result ();
    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::unavailable);
    const auto current =
      original.read_authority (fw::runtime::spot_authority_key (request.key.global_id)).result ();
    ASSERT_TRUE (current);
    if (!creating) {
        const auto *kept = std::get_if<fw::authority_snapshot_t> (&current.value ());
        ASSERT_NE (kept, nullptr);
        EXPECT_EQ (kept->store_version,
                   std::get<fw::authority_snapshot_t> (observed.value ()).store_version);
    } else
        EXPECT_TRUE (std::holds_alternative<fw::authority_missing_t> (current.value ()));
    if (creating) {
        // The live owner's old descriptor ended; the released pending slot is
        // available to a replacement lifecycle through the normal reservation.
        descriptor.lifecycle_generation = 2;
        descriptor.entry_spot_id = "replacement-entry";
        ASSERT_EQ (original.update_mesh_node (descriptor, fw::location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   fw::location_write_status_t::stored);
        request.target.node_lifecycle_generation = 2;
        const auto next = original.reserve (request).result ();
        ASSERT_TRUE (next);
        const auto *fresh = std::get_if<fw::object_reserved_t> (&next.value ());
        ASSERT_NE (fresh, nullptr);
        EXPECT_GT (fresh->fence.object_generation, reserved->fence.object_generation);
        const auto aborted = original.abort ({request.key, fresh->fence}).result ();
        ASSERT_TRUE (aborted);
        EXPECT_TRUE (std::holds_alternative<fw::object_aborted_t> (aborted.value ()));
    } else {
        const auto listed = original.list_mesh_nodes (mesh_name).result ();
        ASSERT_TRUE (listed);
        const auto old =
          std::find_if (listed.value ().items.begin (), listed.value ().items.end (),
                        [&] (const auto &item) { return item.rid == descriptor.rid; });
        ASSERT_NE (old, listed.value ().items.end ());
        EXPECT_EQ (old->capacity.spots.active, 1u);
        EXPECT_EQ (old->capacity.spots.reserved, 0u);
    }
    std::lock_guard lock (evidence->mutex);
    EXPECT_EQ (evidence->room_factories, 0);
}

TEST (UserSpotCreation, ExpiredReadyWithoutFactoryTargetKeepsAuthority)
{
    user_spot_authority_qualification (user_spot_authority_case_t::expired_ready);
}

TEST (UserSpotCreation, ExpiredReadyWithRelocationRemainsUnavailable)
{
    user_spot_authority_qualification (user_spot_authority_case_t::expired_retained);
}

TEST (UserSpotCreation, LiveOwnerCreatingReleasesEndedTargetReservation)
{
    user_spot_authority_qualification (user_spot_authority_case_t::creating_target_ended);
}

class user_spot_capacity_race_store_t final : public fw::location_store_t
{
  public:
    fw::task_t<fw::store_read_result_t> read (fw::store_key_t key) override
    {
        const auto &flow = fw::runtime::flow_context_t::current ();
        const bool watched = armed.load () && flow && flow->flow_id == loser_flow;
        if (watched && pause_after_missing && key.value == authority_key
            && armed.exchange (false)) {
            auto observed = co_await inner.read (std::move (key));
            if (!std::holds_alternative<fw::store_missing_t> (observed))
                throw std::runtime_error ("capacity race requires an initial Missing authority");
            paused.set_value ();
            co_await release.task ();
            co_return observed;
        }
        co_return co_await inner.read (std::move (key));
    }
    fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t request) override
    {
        const auto &flow = fw::runtime::flow_context_t::current ();
        if (!pause_after_missing && armed.load () && flow && flow->flow_id == loser_flow
            && std::any_of (request.conditions.begin (), request.conditions.end (),
                            [] (const auto &condition) {
                                return std::holds_alternative<fw::store_sum_at_most_condition_t> (
                                  condition);
                            })
            && armed.exchange (false)) {
            paused.set_value ();
            co_await release.task ();
        }
        co_return co_await inner.write (std::move (request));
    }
    fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    fw::runtime::in_memory_location_store_t inner;
    std::string loser_flow;
    std::string authority_key;
    bool pause_after_missing = false;
    std::atomic_bool armed{false};
    std::promise<void> paused;
    fw::task_completion_source_t<void> release;
};

void same_id_capacity_race (bool pause_after_missing)
{
    auto store = std::make_shared<user_spot_capacity_race_store_t> ();
    auto evidence = std::make_shared<observations_t> ();
    host_t host;
    configure (host, evidence, first_rid, store,
               std::make_shared<fw::runtime::in_memory_relocation_store_t> (),
               default_follow_duration, 1);
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    const auto listed = services.get_required<fw::runtime::store_location_resolvers_t> ()
                          .list_live_mesh_nodes (mesh_name)
                          .result ();
    ASSERT_TRUE (listed);
    ASSERT_EQ (listed.value ().size (), 1u);
    store->loser_flow = fw::runtime::flow_id_t::create ();
    store->pause_after_missing = pause_after_missing;
    store->authority_key = std::string ("authority") + '\0' + "spot" + '\0' + "capacity-race-room";
    auto blocked = store->paused.get_future ();
    const std::shared_ptr<void> release_on_exit (
      nullptr, [&] (void *) { store->release.complete (fw::result_t<void>::success ()); });
    store->armed.store (true);
    auto &spots = services.get_required<fw::spot_manager_t> ();
    auto pending = [&] {
        auto flow = fw::runtime::flow_context_t::enter (
          std::optional<std::string> (store->loser_flow), fw::flow_origin_t::application,
          fw::message_flow_log_mode_t::detailed, fw::flow_origin_t::application, std::nullopt);
        return spots.get_or_create ("capacity-race-room", room_type).timeout (wait_budget).async ();
    }();
    ASSERT_EQ (blocked.wait_for (wait_budget), std::future_status::ready);
    const auto winner = spots.get_or_create ("capacity-race-room", room_type)
                          .timeout (wait_budget)
                          .async ()
                          .result ();
    ASSERT_TRUE (winner);
    ASSERT_EQ (winner.value ().state, fw::spot_create_state_t::created);
    store->release.complete (fw::result_t<void>::success ());
    const auto loser = pending.result ();
    ASSERT_TRUE (loser) << (loser.error () ? loser.error ()->what () : "no error");
    EXPECT_EQ (loser.value ().state, fw::spot_create_state_t::existing);
    EXPECT_EQ (loser.value ().spot.spot_id (), winner.value ().spot.spot_id ());
    EXPECT_EQ (loser.value ().spot.object_generation (), winner.value ().spot.object_generation ());
    EXPECT_EQ (loser.value ().spot.mesh_name (), winner.value ().spot.mesh_name ());
    EXPECT_EQ (loser.value ().spot.node_rid ().value (), winner.value ().spot.node_rid ().value ());
    std::lock_guard lock (evidence->mutex);
    EXPECT_EQ (evidence->room_factories, 1);
}

TEST (UserSpotCreation, CapacityRaceObservesSameIdWinner)
{
    same_id_capacity_race (false);
}

TEST (UserSpotCreation, CandidateExhaustionRechecksSameIdWinner)
{
    same_id_capacity_race (true);
}
} // namespace zlink::framework::test::actor_maintenance_join
