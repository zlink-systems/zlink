/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/store_location_resolvers.hpp"

#include <gtest/gtest.h>
#include <zlink/framework/contracts/spots/spot_identity.hpp>

#include <string_view>

#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
#include "../support/redis_location_fixture.hpp"
#include "../support/scope_exit.hpp"

#include <sw/redis++/redis++.h>

#include <iterator>
#include <vector>
#endif

namespace
{
using namespace zlink::framework;
using namespace zlink::framework::runtime;
using namespace std::chrono_literals;

constexpr auto entry_a = "mesh-entry-00000000-0000-4000-8000-000000000001";
constexpr auto entry_b = "mesh-entry-00000000-0000-4000-8000-000000000002";

location_owner_token_t
claim (location_repository_t &store, const std::string &owner_id, std::chrono::seconds ttl = 30s)
{
    return std::get<owner_lease_claimed_t> (
             store.claim_owner_lease (owner_id, ttl).result ().value ())
      .token;
}

mesh_node_descriptor_t
descriptor (const location_owner_token_t &owner, std::uint8_t node, const std::string &entry)
{
    return {.mesh_name = "mesh",
            .rid = zlink::routing_id_t::from (std::string (1, static_cast<char> (node))),
            .lifecycle_generation = node,
            .descriptor_revision = 1,
            .endpoint = "tcp://127.0.0.1:" + std::to_string (7000 + node),
            .entry_spot_id = entry,
            .channel_weights = {{"game", 100}},
            .application_version = 1,
            .object_role = object_role_t::server,
            .state = framework_runtime_state_t::serving,
            .security_identity = "security",
            .owner_id = owner.owner_id,
            .lease_generation = owner.lease_generation};
}

location_write_status_t publish (location_repository_t &store, const mesh_node_descriptor_t &value)
{
    return store.update_mesh_node (value, location_write_intent_t::new_claim)
      .result ()
      .value ()
      .status;
}

location_write_status_t publish_second (location_repository_t &store, const std::string &entry)
{
    const auto owner_a = claim (store, "entry-owner-a");
    const auto owner_b = claim (store, "entry-owner-b");
    EXPECT_EQ (publish (store, descriptor (owner_a, 1, entry_a)), location_write_status_t::stored);
    return publish (store, descriptor (owner_b, 2, entry));
}

void entry_identity_fences_reserve_and_release (location_repository_t &store)
{
    store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto first_owner = claim (store, "entry-owner-a", 15s);
    const auto second_owner = claim (store, "entry-owner-b", 15s);
    const auto entry_id = zlink::framework::detail::new_entry_spot_id ("entry-node");
    const auto descriptor = [&] (std::string rid, std::uint64_t lifecycle,
                                 const location_owner_token_t &owner, std::string id) {
        return mesh_node_descriptor_t{
          .mesh_name = "entry-mesh",
          .rid = zlink::routing_id_t::from (std::move (rid)),
          .lifecycle_generation = lifecycle,
          .descriptor_revision = 1,
          .endpoint = "tcp://127.0.0.1:1",
          .entry_spot_id = std::move (id),
          .application_version = 1,
          .object_capabilities = {{.object_kind = placement_object_kind_t::user_spot,
                                   .stable_type = "room",
                                   .spot_limit = 8}},
          .object_role = object_role_t::server,
          .capacity = {.spots = {.limit = 8},
                       .spot_types = {{.object_kind = placement_object_kind_t::user_spot,
                                       .stable_type = "room",
                                       .usage = {.limit = 8}}}},
          .state = framework_runtime_state_t::serving,
          .security_identity = "entry-test",
          .owner_id = owner.owner_id,
          .lease_generation = owner.lease_generation};
    };
    const auto first = descriptor ("entry-node-a", 41, first_owner, entry_id);
    const auto second = descriptor ("entry-node-b", 42, second_owner, entry_id);
    ASSERT_EQ (publish (store, first), location_write_status_t::stored);
    runtime::store_location_resolvers_t resolver (store);
    const auto first_route =
      resolver.resolve_spot_address (first.mesh_name, entry_id).result ().value ();
    ASSERT_TRUE (first_route);
    EXPECT_EQ (first_route->node_rid.to_string (), first.rid.to_string ());
    EXPECT_EQ (publish (store, second), location_write_status_t::rejected_conflict);

    object_reserve_request_t reserved_entry{
      .key = {placement_object_kind_t::user_spot, entry_id},
      .intent = {.stable_type = "room"},
      .target = {.mesh_name = first.mesh_name,
                 .node_rid = node_rid_t::from_string (first.rid.to_string ()),
                 .node_lifecycle_generation = first.lifecycle_generation,
                 .owner = first_owner},
      .capacity_bundle = {
        .spot_slots = 1,
        .spot_type = spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1}}};
    EXPECT_TRUE (std::holds_alternative<object_reserve_conflict_t> (
      store.reserve (reserved_entry).result ().value ()));

    ASSERT_EQ (
      store.remove_mesh_node ({first.mesh_name, first.rid}, first_owner).result ().value (),
      location_write_status_t::stored);
    ASSERT_EQ (publish (store, second), location_write_status_t::stored);
    resolver.invalidate_spot_address (entry_id);
    const auto replacement_route =
      resolver.resolve_spot_address (second.mesh_name, entry_id).result ().value ();
    ASSERT_TRUE (replacement_route);
    EXPECT_EQ (replacement_route->node_rid.to_string (), second.rid.to_string ());
    EXPECT_EQ (
      store.remove_mesh_node ({first.mesh_name, first.rid}, first_owner).result ().value (),
      location_write_status_t::ignored_stale);

    const auto occupied_id = zlink::framework::detail::new_user_spot_id ();
    auto occupied_target =
      descriptor ("occupied-target", 43, first_owner,
                  zlink::framework::detail::new_entry_spot_id ("occupied-target"));
    ASSERT_EQ (publish (store, occupied_target), location_write_status_t::stored);
    object_reserve_request_t occupied_request{
      .key = {placement_object_kind_t::user_spot, occupied_id},
      .intent = {.stable_type = "room"},
      .target = {.mesh_name = occupied_target.mesh_name,
                 .node_rid = node_rid_t::from_string (occupied_target.rid.to_string ()),
                 .node_lifecycle_generation = occupied_target.lifecycle_generation,
                 .owner = first_owner},
      .capacity_bundle = {
        .spot_slots = 1,
        .spot_type = spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1}}};
    ASSERT_TRUE (std::holds_alternative<object_reserved_t> (
      store.reserve (occupied_request).result ().value ()));
    auto conflicting_descriptor = descriptor ("occupied-entry", 44, second_owner, occupied_id);
    EXPECT_EQ (publish (store, conflicting_descriptor), location_write_status_t::rejected_conflict);
}

void owner_cleanup_releases_only_the_matching_claim (location_repository_t &store)
{
    const auto first = claim (store, "entry-owner-a");
    const auto second = claim (store, "entry-owner-b");
    const auto third = claim (store, "entry-owner-c");
    auto first_node = descriptor (first, 1, entry_a);
    const auto second_node = descriptor (second, 2, entry_a);
    ASSERT_EQ (publish (store, first_node), location_write_status_t::stored);
    EXPECT_EQ (store.remove_all_by_owner (first).result ().value (), 0);
    EXPECT_EQ (store.list_mesh_nodes ("mesh").result ().value ().items.size (), 1u);
    ++first_node.descriptor_revision;
    EXPECT_EQ (
      store.update_mesh_node (first_node, location_write_intent_t::renew).result ().value ().status,
      location_write_status_t::stored);
    ASSERT_EQ (publish (store, second_node), location_write_status_t::stored);
    EXPECT_EQ (store.remove_all_by_owner (first).result ().value (), 0);
    EXPECT_EQ (
      store.remove_mesh_node ({first_node.mesh_name, first_node.rid}, first).result ().value (),
      location_write_status_t::stored);
    EXPECT_EQ (publish (store, descriptor (third, 3, entry_a)),
               location_write_status_t::rejected_conflict);
}

class EntrySpotIdentityLifecycle : public testing::TestWithParam<bool>
{
  protected:
    in_memory_location_repository_t memory;
    in_memory_location_store_t opaque;
    provider_location_repository_t provider{opaque};

    location_repository_t &store ()
    {
        return GetParam () ? static_cast<location_repository_t &> (provider) : memory;
    }
};

TEST_P (EntrySpotIdentityLifecycle, ReserveAndDescriptorRemoveUseTheSameIdentity)
{
    entry_identity_fences_reserve_and_release (store ());
}

TEST_P (EntrySpotIdentityLifecycle, OwnerCleanupReleasesOnlyTheMatchingClaim)
{
    owner_cleanup_releases_only_the_matching_claim (store ());
}

TEST_P (EntrySpotIdentityLifecycle, RenewCannotTakeAnEntrySpotClaimedByAnotherNode)
{
    const auto first = claim (store (), "entry-owner-a");
    const auto second = claim (store (), "entry-owner-b");
    auto first_node = descriptor (first, 1, entry_a);
    ASSERT_EQ (publish (store (), first_node), location_write_status_t::stored);
    ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
      store ().release_owner_lease (first).result ().value ()));
    ASSERT_EQ (publish (store (), descriptor (second, 2, entry_a)),
               location_write_status_t::stored);
    const auto recovered = claim (store (), first.owner_id);
    first_node.lease_generation = recovered.lease_generation;
    ++first_node.descriptor_revision;
    EXPECT_EQ (store ()
                 .update_mesh_node (first_node, location_write_intent_t::renew)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::rejected_conflict);
}

INSTANTIATE_TEST_SUITE_P (Backends, EntrySpotIdentityLifecycle, testing::Bool ());

#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
class EntrySpotIdentity : public testing::TestWithParam<const char *>
{
};

TEST_P (EntrySpotIdentity, NewClaimReturnsSameStatusInMemoryAndRedis)
{
    const auto options = test::find_redis_options ();
    if (!options)
        GTEST_SKIP () << "Redis is not reachable at 127.0.0.1:6379; set ZLINK_REDIS_TEST_ENDPOINT";
    redis::redis_location_store_t redis (*options);
    sw::redis::Redis raw (redis::detail::normalize_connection_string (options->connection_string));
    test::scope_exit_t cleanup ([&] {
        EXPECT_NO_THROW ({
            std::vector<std::string> keys;
            raw.keys (options->key_prefix + ":*", std::back_inserter (keys));
            if (!keys.empty ())
                raw.del (keys.begin (), keys.end ());
        });
    });
    in_memory_location_repository_t memory;
    provider_location_repository_t provider (redis);
    const auto *entry = GetParam ();
    const auto expected = std::string_view (entry) == entry_a
                            ? location_write_status_t::rejected_conflict
                            : location_write_status_t::stored;
    const auto memory_status = publish_second (memory, entry);
    const auto provider_status = publish_second (provider, entry);
    EXPECT_EQ (memory_status, expected);
    EXPECT_EQ (provider_status, expected);
}

INSTANTIATE_TEST_SUITE_P (Backends, EntrySpotIdentity, testing::Values (entry_a, entry_b));
#endif
} // namespace
