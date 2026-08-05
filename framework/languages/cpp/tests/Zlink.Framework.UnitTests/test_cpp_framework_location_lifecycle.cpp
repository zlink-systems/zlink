/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/actors/actor_ref_access.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/location_lifecycle.hpp"
#include "runtime/locations/location_runtime.hpp"

#include <gtest/gtest.h>

namespace
{

using zlink::framework::actor_location_key_t;
using zlink::framework::actor_location_t;
using zlink::framework::location_write_status_t;
using zlink::framework::spot_location_key_t;
using zlink::framework::spot_location_t;
using zlink::framework::runtime::in_memory_location_repository_t;
using zlink::framework::runtime::location_lifecycle_t;
using zlink::framework::runtime::location_runtime_t;

actor_location_t make_actor (std::string actor_id, std::int64_t generation = 0)
{
    (void) generation;
    const auto actor_id_copy = actor_id;
    return actor_location_t{.mesh_name = "node-a",
                            .actor_id = std::move (actor_id),
                            .actor_type = "player",
                            .actor_ref = zlink::framework::detail::actor_ref_access_t::make (
                              zlink::framework::node_rid_t::from_string ("node-a"),
                              "player", actor_id_copy, 1),
                            .owner_node_rid = zlink::routing_id_t::from ("node-a"),
                            .owner_node_generation = 1,
                            .spot_id = "entry-spot",
                            .spot_generation = 1,
                            .spot_kind = zlink::spot_kind::entry,
                            .membership_epoch = 1};
}

spot_location_t make_spot (std::string spot_id)
{
    return spot_location_t{.mesh_name = "node-a",
                           .spot_id = std::move (spot_id),
                           .spot_type = "play",
                           .node_rid = zlink::routing_id_t::from ("node-a"),
                           .spot_kind = zlink::spot_kind::user,
                           .generation = 1};
}

TEST (ZLinkFrameworkLocationLifecycle, TracksClaimedActor)
{
    in_memory_location_repository_t store;
    location_runtime_t runtime (store, {}, "owner-a");
    runtime.start (zlink::routing_id_t::from ("node-a"));
    location_lifecycle_t lifecycle (runtime);

    const auto claim = lifecycle.claim_actor (make_actor ("actor-1"));
    ASSERT_EQ (location_write_status_t::stored, claim.status);
    EXPECT_EQ (1u, claim.store_generation);
    EXPECT_EQ (1u, lifecycle.tracked_actor_count ());

    runtime.stop ();
}

TEST (ZLinkFrameworkLocationLifecycle, TracksMaterializationPerProcess)
{
    in_memory_location_repository_t store;
    location_runtime_t owner_a (store, {}, "owner-a");
    location_runtime_t owner_b (store, {}, "owner-b");
    owner_a.start (zlink::routing_id_t::from ("node-a"));
    owner_b.start (zlink::routing_id_t::from ("node-b"));

    location_lifecycle_t lifecycle_a (owner_a);
    location_lifecycle_t lifecycle_b (owner_b);

    ASSERT_EQ (location_write_status_t::stored,
               lifecycle_a.claim_actor (make_actor ("actor-1")).status);
    EXPECT_EQ (location_write_status_t::stored,
               lifecycle_b.claim_actor (make_actor ("actor-1")).status);
    EXPECT_EQ (1u, lifecycle_a.tracked_actor_count ());
    EXPECT_EQ (1u, lifecycle_b.tracked_actor_count ());

    owner_b.stop ();
    owner_a.stop ();
}

TEST (ZLinkFrameworkLocationLifecycle, ReleasesTrackedActor)
{
    in_memory_location_repository_t store;
    location_runtime_t runtime (store, {}, "owner-a");
    runtime.start (zlink::routing_id_t::from ("node-a"));
    location_lifecycle_t lifecycle (runtime);

    ASSERT_EQ (location_write_status_t::stored,
               lifecycle.claim_actor (make_actor ("actor-1")).status);
    const auto released =
      lifecycle.release_actor (
        actor_location_key_t{.mesh_name = "node-a", .actor_id = "actor-1"});
    EXPECT_EQ (location_write_status_t::stored, released.status);
    EXPECT_EQ (0u, lifecycle.tracked_actor_count ());
    runtime.stop ();
}

TEST (ZLinkFrameworkLocationLifecycle, IgnoresActorOperationsWhenClaimIsNotTracked)
{
    in_memory_location_repository_t store;
    location_runtime_t runtime (store, {}, "owner-a");
    runtime.start (zlink::routing_id_t::from ("node-a"));
    location_lifecycle_t lifecycle (runtime);

    auto moved = make_actor ("actor-missing");
    const auto updated = lifecycle.update_actor_location (std::move (moved));
    EXPECT_EQ (location_write_status_t::ignored_stale, updated.status);

    const auto renewed =
      lifecycle.renew_actor (
        actor_location_key_t{.mesh_name = "node-a", .actor_id = "actor-missing"});
    EXPECT_EQ (location_write_status_t::ignored_stale, renewed.status);

    const auto released =
      lifecycle.release_actor (
        actor_location_key_t{.mesh_name = "node-a", .actor_id = "actor-missing"});
    EXPECT_EQ (location_write_status_t::ignored_stale, released.status);
    EXPECT_EQ (0u, lifecycle.tracked_actor_count ());

    runtime.stop ();
}

TEST (ZLinkFrameworkLocationLifecycle, UpdatesTrackedActorLocationWithoutChangingOwnerToken)
{
    in_memory_location_repository_t store;
    location_runtime_t runtime (store, {}, "owner-a");
    runtime.start (zlink::routing_id_t::from ("node-a"));
    location_lifecycle_t lifecycle (runtime);

    const auto claim = lifecycle.claim_actor (make_actor ("actor-1"));
    ASSERT_EQ (location_write_status_t::stored, claim.status);
    EXPECT_TRUE (
      lifecycle.owns_actor (
        actor_location_key_t{.mesh_name = "node-a", .actor_id = "actor-1"}));

    auto moved = make_actor ("actor-1");
    moved.spot_kind = zlink::spot_kind::user;
    moved.spot_id = "play-spot";
    const auto updated = lifecycle.update_actor_location (std::move (moved));
    ASSERT_EQ (location_write_status_t::stored, updated.status);
    EXPECT_EQ (claim.store_generation, static_cast<std::uint64_t> (updated.generation));

    EXPECT_EQ (claim.store_generation, static_cast<std::uint64_t> (updated.generation));

    runtime.stop ();
}

TEST (ZLinkFrameworkLocationLifecycle, TracksSpotMaterializationPerProcess)
{
    in_memory_location_repository_t store;
    location_runtime_t owner_a (store, {}, "owner-a");
    location_runtime_t owner_b (store, {}, "owner-b");
    owner_a.start (zlink::routing_id_t::from ("node-a"));
    owner_b.start (zlink::routing_id_t::from ("node-b"));

    location_lifecycle_t lifecycle_a (owner_a);
    location_lifecycle_t lifecycle_b (owner_b);
    const auto key = spot_location_key_t{.spot_id = "spot-1"};

    ASSERT_EQ (location_write_status_t::stored, lifecycle_a.claim_spot (make_spot ("spot-1")).status);
    EXPECT_EQ (location_write_status_t::stored,
               lifecycle_b.claim_spot (make_spot ("spot-1")).status);

    const auto release_b = lifecycle_b.release_spot (key);
    EXPECT_EQ (location_write_status_t::stored, release_b.status);
    const auto release_b_again = lifecycle_b.release_spot (key);
    EXPECT_EQ (location_write_status_t::ignored_stale, release_b_again.status);
    EXPECT_EQ (location_write_status_t::stored,
               lifecycle_a.release_spot (key).status);

    owner_b.stop ();
    owner_a.stop ();
}

TEST (ZLinkFrameworkLocationLifecycle, ClaimsAndReleasesTrackedSpot)
{
    in_memory_location_repository_t store;
    location_runtime_t runtime (store, {}, "owner-a");
    runtime.start (zlink::routing_id_t::from ("node-a"));
    location_lifecycle_t lifecycle (runtime);

    const auto claim = lifecycle.claim_spot (make_spot ("spot-1"));
    ASSERT_EQ (location_write_status_t::stored, claim.status);
    EXPECT_EQ (1, claim.spot.generation);
    const auto key = spot_location_key_t{.spot_id = "spot-1"};
    const auto released = lifecycle.release_spot (key);
    EXPECT_EQ (location_write_status_t::stored, released.status);
    EXPECT_EQ (location_write_status_t::ignored_stale,
               lifecycle.release_spot (key).status);

    runtime.stop ();
}

TEST (ZLinkFrameworkLocationLifecycle, RenewKeepsTrackedActor)
{
    in_memory_location_repository_t store;
    location_runtime_t owner_a (store, {}, "owner-a");
    owner_a.start (zlink::routing_id_t::from ("node-a"));

    location_lifecycle_t lifecycle_a (owner_a);
    std::vector<std::string> deactivated;
    const auto claim = lifecycle_a.claim_actor (make_actor ("actor-1"), [&] (const auto &actor) {
        deactivated.push_back (actor.actor_id);
    });
    ASSERT_EQ (location_write_status_t::stored, claim.status);

    const auto renewed =
      lifecycle_a.renew_actor (
        actor_location_key_t{.mesh_name = "node-a", .actor_id = "actor-1"});
    EXPECT_EQ (location_write_status_t::stored, renewed.status);
    EXPECT_TRUE (deactivated.empty ());
    EXPECT_EQ (1u, lifecycle_a.tracked_actor_count ());

    owner_a.stop ();
}

} // namespace
