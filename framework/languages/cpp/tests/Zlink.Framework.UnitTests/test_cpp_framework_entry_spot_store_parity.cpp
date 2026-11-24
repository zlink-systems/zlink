/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/redis_location_options.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/provider_location_repository.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::runtime;

constexpr auto entry_id = "play-entry-00000000-0000-4000-8000-000000000001";
constexpr auto user_spot = placement_object_kind_t::user_spot;
constexpr auto instance_spot = placement_object_kind_t::instance_spot;

struct write_results_t
{
    location_write_status_t first;
    location_write_status_t second;
    bool reserved;
    location_write_status_t third;
    bool competing_type_mismatch;
};

write_results_t write_sequence (location_repository_t &store, placement_object_kind_t kind)
{
    const auto claim = [&] (const std::string &rid) {
        return std::get<owner_lease_claimed_t> (
                 store.claim_owner_lease (rid, 60s).result ().value ())
          .token;
    };
    const auto publish = [&] (const std::string &rid, const location_owner_token_t &owner) {
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "play";
        descriptor.rid = zlink::routing_id_t::from (rid);
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7000";
        descriptor.application_version = 1;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.security_identity = rid;
        descriptor.owner_id = owner.owner_id;
        descriptor.lease_generation = owner.lease_generation;
        descriptor.entry_spot_id = entry_id;
        descriptor.capacity.spots.limit = 8;
        for (const auto spot_kind : {user_spot, instance_spot}) {
            descriptor.object_capabilities.push_back (
              {spot_kind, "room", maintenance_policy_kind_t::disabled, false, 8});
            descriptor.capacity.spot_types.push_back ({spot_kind, "room", {0, 0, 8}});
        }
        return store.update_mesh_node (descriptor, location_write_intent_t::new_claim)
          .result ()
          .value ()
          .status;
    };
    const auto first_owner = claim ("node-a");
    const auto first = publish ("node-a", first_owner);
    const auto second = publish ("node-b", claim ("node-b"));
    const auto reserve = [&] (placement_object_kind_t spot_kind) {
        object_reserve_request_t request;
        request.key = {spot_kind, entry_id};
        request.intent.stable_type = "room";
        request.target = {"play", node_rid_t::from_string ("node-a"), 1, first_owner};
        request.creating_payload = {std::byte{1}};
        request.capacity_bundle = {0, 1, spot_type_capacity_delta_t{spot_kind, "room", 1}};
        return store.reserve (request).result ().value ();
    };
    const auto reserved = reserve (kind);
    const auto third = publish ("node-c", claim ("node-c"));
    const auto competing = reserve (kind == user_spot ? instance_spot : user_spot);
    return {first, second, std::holds_alternative<object_reserved_t> (reserved), third,
            std::holds_alternative<object_type_mismatch_t> (competing)};
}

class EntrySpotStoreParity : public ::testing::TestWithParam<placement_object_kind_t>
{
};

TEST_P (EntrySpotStoreParity, DescriptorWritesAndSpotReserve)
{
    const auto options = zlink::framework::test::find_redis_options ();
    if (!options)
        GTEST_SKIP () << "Redis is not reachable; set ZLINK_REDIS_TEST_ENDPOINT";
    redis::redis_location_store_t provider (*options);
    provider_location_repository_t redis_repository (provider);
    in_memory_location_repository_t memory_repository;

    const auto redis = write_sequence (redis_repository, GetParam ());
    const auto memory = write_sequence (memory_repository, GetParam ());
    EXPECT_EQ (redis.first, location_write_status_t::stored);
    EXPECT_EQ (redis.second, location_write_status_t::stored);
    EXPECT_TRUE (redis.reserved);
    EXPECT_EQ (redis.third, location_write_status_t::stored);
    EXPECT_TRUE (redis.competing_type_mismatch);
    EXPECT_EQ (memory.first, redis.first);
    EXPECT_EQ (memory.second, redis.second);
    EXPECT_EQ (memory.reserved, redis.reserved);
    EXPECT_EQ (memory.third, redis.third);
    EXPECT_EQ (memory.competing_type_mismatch, redis.competing_type_mismatch);
}

INSTANTIATE_TEST_SUITE_P (UserAndInstance,
                          EntrySpotStoreParity,
                          ::testing::Values (user_spot, instance_spot));
}
