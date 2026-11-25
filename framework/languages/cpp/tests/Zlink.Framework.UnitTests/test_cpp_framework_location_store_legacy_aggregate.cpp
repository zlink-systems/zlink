/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "../support/opaque_store_record_fixture.hpp"

#include <gtest/gtest.h>
#include <thread>

namespace
{
using namespace zlink::framework::test::opaque_store;
using json_t = nlohmann::json;

const store_key_t format_key{"zlink:v12:format"};
const store_key_t floor_key{"zlink:v12:generation-floor"};
const store_key_t legacy_capacity_key{"zlink:v11:capacity:mesh:target"};
const store_key_t lease_key{std::string ("owner-lease") + '\0' + "target-owner"};

class LegacyAggregateConversion : public testing::Test
{
  protected:
    void put (store_key_t key,
              std::string_view value,
              std::optional<std::chrono::milliseconds> retention = {})
    {
        ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
          store.write ({{}, {store_put_t{std::move (key), bytes (value), retention}}})
            .result ()
            .value ()));
    }

    store_read_result_t read (const store_key_t &key)
    {
        return store.read (key).result ().value ();
    }

    store_scan_page_t snapshot ()
    {
        return std::get<store_scan_page_t> (store.scan ({"", std::nullopt}).result ().value ());
    }

    void start ()
    {
        location_runtime_t runtime (repository, {}, "conversion-owner");
        runtime.start (zlink::routing_id_t::from ("startup"));
    }

    store_key_t seed_cpp (std::string id, std::string_view status, unsigned actor_slots)
    {
        const store_key_t key{"zlink:v11:aggregate:" + id};
        const json_t record{{"aggregateId", id},
                            {"aggregateGeneration", 1},
                            {"status", status},
                            {"targetMeshName", "mesh"},
                            {"targetNodeRid", "target"},
                            {"targetLifecycleGeneration", 1},
                            {"targetOwner", {{"ownerId", "target-owner"}, {"leaseGeneration", 1}}},
                            {"capacityBundle", {{"actors", actor_slots}, {"spots", 0}}}};
        put (key, record.dump ());
        return key;
    }

    in_memory_location_store_t store;
    provider_location_repository_t repository{store};
};

TEST_F (LegacyAggregateConversion, SettlesFreshStagingAndReturnsPreparedPending)
{
    using namespace std::chrono_literals;
    put (lease_key, R"({"recordVersion":1,"ownerId":"target-owner","leaseGeneration":"1"})", 1ms);
    std::this_thread::sleep_for (1ms);
    ASSERT_TRUE (std::holds_alternative<store_missing_t> (read (lease_key)));
    const auto staging = seed_cpp ("000000000000400080000000000000a1", "preparing", 1);
    const auto prepared = seed_cpp ("000000000000400080000000000000a2", "prepared", 2);
    const auto committed = seed_cpp ("000000000000400080000000000000a3", "committed", 1);
    const store_key_t inventory{"zlink:v11:aggregate-inventory:000000000000400080000000000000a3:0"};
    put (inventory, "opaque inventory");
    put (
      legacy_capacity_key,
      R"({"active":{"actors":4,"spots":0,"spotTypes":{}},"pending":{"actors":5,"spots":0,"spotTypes":{}}})");

    ASSERT_NO_THROW (start ());
    ASSERT_TRUE (std::holds_alternative<store_found_t> (read (format_key)));
    EXPECT_EQ (std::get<store_found_t> (read (format_key)).value.bytes, bytes ("converted"));
    for (const auto &key : {staging, prepared, committed, inventory, legacy_capacity_key})
        EXPECT_TRUE (std::holds_alternative<store_missing_t> (read (key))) << key.value;

    mesh_node_descriptor_t target;
    target.mesh_name = "mesh";
    target.rid = zlink::routing_id_t::from ("target");
    const auto active = read (capacity_key (target));
    const auto pending = read (capacity_key (target, "actors", "pending"));
    ASSERT_TRUE (std::holds_alternative<store_found_t> (active));
    ASSERT_TRUE (std::holds_alternative<store_found_t> (pending));
    EXPECT_EQ (std::get<store_found_t> (active).value.bytes, bytes ("4"));
    EXPECT_EQ (std::get<store_found_t> (pending).value.bytes, bytes ("3"));
}

TEST_F (LegacyAggregateConversion, LiveTerminalTargetLeavesEntireStoreUnchanged)
{
    const auto expired = seed_cpp ("000000000000400080000000000000a1", "aborted", 0);
    seed_cpp ("000000000000400080000000000000a2", "committed", 1);
    auto record = json_t::parse (std::get<store_found_t> (read (expired)).value.bytes);
    record["targetOwner"]["ownerId"] = "expired-owner";
    put (expired, record.dump ());
    put (lease_key, R"({"recordVersion":1,"ownerId":"target-owner","leaseGeneration":"1"})",
         std::chrono::hours (1));
    put (
      legacy_capacity_key,
      R"({"active":{"actors":1,"spots":0,"spotTypes":{}},"pending":{"actors":0,"spots":0,"spotTypes":{}}})");
    put ({"zlink:v11:aggregate-inventory:000000000000400080000000000000a2:0"},
         "live root inventory");
    const auto before = snapshot ();
    ASSERT_FALSE (before.next_cursor);

    try {
        start ();
        ADD_FAILURE () << "A live v11 target owner allowed startup";
    }
    catch (const framework_exception_t &error) {
        EXPECT_EQ (error.kind (), framework_error_kind_t::protocol_error);
    }

    EXPECT_TRUE (std::holds_alternative<store_missing_t> (read (format_key)));
    EXPECT_TRUE (std::holds_alternative<store_missing_t> (read (floor_key)));
    const auto after_snapshot = snapshot ();
    ASSERT_FALSE (after_snapshot.next_cursor);
    EXPECT_EQ (after_snapshot.items.size (), before.items.size ());
    for (const auto &item : before.items) {
        const auto row = read (item.key);
        ASSERT_TRUE (std::holds_alternative<store_found_t> (row)) << item.key.value;
        const auto &after = std::get<store_found_t> (row).value;
        EXPECT_EQ (after.bytes, item.value.bytes);
        EXPECT_EQ (after.version.value, item.value.version.value);
        EXPECT_EQ (after.expires_at, item.value.expires_at);
    }
}

TEST_F (LegacyAggregateConversion, ReturnsCppPreparedSpotTypeCapacity)
{
    const auto prepared = seed_cpp ("000000000000400080000000000000a1", "prepared", 0);
    auto record = json_t::parse (std::get<store_found_t> (read (prepared)).value.bytes);
    record["capacityBundle"] = {
      {"actors", 0},
      {"spots", 1},
      {"spotType", {{"objectKind", "userSpot"}, {"stableType", "room"}, {"count", 1}}}};
    put (prepared, record.dump ());
    put (
      legacy_capacity_key,
      R"({"actorsActive":0,"actorsPending":0,"spotsActive":2,"spotsPending":2,"spotTypes":{"2:room":{"active":1,"pending":2}}})");

    ASSERT_NO_THROW (start ());
    EXPECT_TRUE (std::holds_alternative<store_missing_t> (read (prepared)));
    mesh_node_descriptor_t target;
    target.mesh_name = "mesh";
    target.rid = zlink::routing_id_t::from ("target");
    const auto dimension = std::string ("spot-type") + '\0' + "userSpot" + '\0' + "room";
    EXPECT_EQ (capacity_count (store, capacity_key (target, "spots", "pending")), 1u);
    EXPECT_EQ (capacity_count (store, capacity_key (target, dimension, "pending")), 1u);
}

json_t legacy_authority ()
{
    return {{"recordVersion", 1},
            {"payload", "b2xk"},
            {"objectGeneration", "1"},
            {"authorityOwnerGeneration", "3"},
            {"ownerId", "source-owner"},
            {"ownerLeaseGeneration", "1"},
            {"allocation",
             {{"state", "active"},
              {"objectKind", "actor"},
              {"stableType", "sample"},
              {"descriptor", {{"meshName", "mesh"}, {"routingIdHex", "736f75726365"}}},
              {"descriptorLifecycleGeneration", "1"},
              {"capacity", {{"actors", 1}, {"spots", 0}}}}},
            {"pendingCreation", nullptr}};
}

TEST_F (LegacyAggregateConversion, SettlesJavaBinaryAndNodeJsonWithSharedAuthorityEnvelope)
{
    // Java DataOutputStream writes the state, UUID, generation and the target
    // fence before the inventory digest and capacity vector.
    constexpr std::string_view uuid = "00000000-0000-4000-8000-0000000000a1";
    std::string java;
    const auto number = [&] (std::uint64_t value, unsigned width) {
        while (width != 0)
            java += static_cast<char> (value >> (8 * --width));
    };
    const auto text = [&] (std::string_view value) {
        number (value.size (), 2);
        java += value;
    };
    number (2, 1);
    number (0x4000, 8);
    number (0x80000000000000a1ULL, 8);
    number (1, 8);
    text ("mesh");
    text ("746172676574");
    number (1, 8);
    text ("target-owner");
    number (1, 8);
    number (32, 4);
    java.append (32, 'd');
    number (0, 4);
    number (0, 4);
    number (0, 1);
    number (1, 4);
    number (32, 4);
    java.append (32, 'f');
    const store_key_t java_root{"zlink:v11:aggregate:" + std::string (uuid) + ":1"};
    const store_key_t java_payload{"zlink:v11:aggregate-inventory:" + std::string (uuid)
                                   + ":1:participant:0:payload"};
    const store_key_t authority{std::string ("authority") + '\0' + "actor" + '\0' + "java"};
    put (java_root, java);
    put (java_payload, "hello");
    auto java_authority = legacy_authority ();
    java_authority["aggregate"] = {
      {"aggregateId", uuid},
      {"aggregateGeneration", "1"},
      {"index", 0},
      {"expectedStoreVersion", "1"},
      {"ownerTransition", "preserve"},
      {"targetAuthorityOwnerGeneration", "3"},
      {"authorityPayloadSha256",
       "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"},
      {"membershipMutationSha256",
       "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}};
    put (authority, java_authority.dump ());

    const store_key_t node_root{"zlink:v11:aggregate:node%3Aid:5"};
    const store_key_t node_inventory{"zlink:v11:aggregate-inventory:node%3Aid:5:root"};
    put (
      node_root,
      R"({"state":"prepared","targetDescriptor":{"meshName":"mesh","rid":"target"},"targetDescriptorLifecycleGeneration":{"$bigint":"1"},"targetOwner":{"ownerId":"target-owner","leaseGeneration":{"$bigint":"1"}},"capacity":{"actors":0,"spots":1,"spotType":{"objectKind":"user_spot","stableType":"room","count":1}}})");
    put (node_inventory, "inventory");
    put (
      legacy_capacity_key,
      R"({"active":{"actors":0,"spots":2,"spotTypes":{"user_spot\u0000room":1}},"pending":{"actors":0,"spots":2,"spotTypes":{"user_spot\u0000room":2}}})");

    ASSERT_NO_THROW (start ());
    for (const auto &key : {java_root, java_payload, node_root, node_inventory})
        EXPECT_TRUE (std::holds_alternative<store_missing_t> (read (key))) << key.value;
    const auto normalized = json_t::parse (std::get<store_found_t> (read (authority)).value.bytes);
    EXPECT_FALSE (normalized.contains ("aggregate"));
    const auto current =
      repository.read_authority (actor_authority_key ("java")).result ().value ();
    ASSERT_TRUE (std::holds_alternative<authority_snapshot_t> (current));
    EXPECT_EQ (std::get<authority_snapshot_t> (current).payload, bytes ("hello"));
    mesh_node_descriptor_t target;
    target.mesh_name = "mesh";
    target.rid = zlink::routing_id_t::from ("target");
    EXPECT_EQ (capacity_count (store, capacity_key (target, "spots", "pending")), 1u);
    EXPECT_EQ (
      capacity_count (
        store, capacity_key (target, std::string ("spot-type") + '\0' + "userSpot" + '\0' + "room",
                             "pending")),
      1u);
}

TEST_F (LegacyAggregateConversion, NormalizesCommittedDotnetParticipantBeforeDeletingChildren)
{
    constexpr std::string_view uuid = "00000000-0000-4000-8000-0000000000a1";
    constexpr std::uint64_t lifecycle = 0x8000000000000001ULL;
    const store_key_t root{"zlink:v11:aggregate:000000000000400080000000000000a1:1"};
    const store_key_t metadata{root.value + ":participant:0:meta"};
    const store_key_t payload{root.value + ":participant:0:payload"};
    const store_key_t inventory{root.value + ":inventory:0:0"};
    const store_key_t authority{std::string ("authority") + '\0' + "actor" + '\0' + "dotnet"};
    put (root,
         json_t{{"status", 2},
                {"header",
                 {{"aggregateId", uuid},
                  {"aggregateGeneration", 1},
                  {"targetDescriptor", {{"meshName", "mesh"}, {"routingIdHex", "746172676574"}}},
                  {"targetDescriptorLifecycleGeneration", lifecycle},
                  {"targetOwner", {{"ownerId", "target-owner"}, {"leaseGeneration", 1}}},
                  {"capacity", {{"actors", 1}, {"spots", 0}}}}}}
           .dump ());
    put (metadata,
         json_t{{"index", 0},
                {"key", actor_authority_key ("dotnet").value},
                {"ownerTransition", 2},
                {"targetAuthorityOwnerGeneration", 7},
                {"authorityPayloadSha256", "LPJNul+wow4m6DsqxbninhsWHlwfp0JecwQzYpOLmCQ="}}
           .dump ());
    put (payload, "hello");
    put (inventory, "inventory");
    auto dotnet_authority = legacy_authority ();
    dotnet_authority["aggregateFence"] = {{"aggregateId", uuid}, {"aggregateGeneration", 1}};
    dotnet_authority["aggregateParticipantIndex"] = 0;
    dotnet_authority["aggregateExpectedStoreVersion"] = "1";
    put (authority, dotnet_authority.dump ());
    ASSERT_NO_THROW (start ());
    for (const auto &key : {root, metadata, payload, inventory})
        EXPECT_TRUE (std::holds_alternative<store_missing_t> (read (key))) << key.value;
    const auto normalized = json_t::parse (std::get<store_found_t> (read (authority)).value.bytes);
    EXPECT_FALSE (normalized.contains ("aggregateFence"));
    const auto current =
      repository.read_authority (actor_authority_key ("dotnet")).result ().value ();
    ASSERT_TRUE (std::holds_alternative<authority_snapshot_t> (current));
    const auto &moved = std::get<authority_snapshot_t> (current);
    EXPECT_EQ (moved.payload, bytes ("hello"));
    EXPECT_EQ (moved.authority_owner_generation, 7u);
    EXPECT_EQ (moved.owner.owner_id, "target-owner");
    EXPECT_EQ (moved.owner.lease_generation, 1);
    EXPECT_EQ (moved.allocation.target.node_lifecycle_generation, lifecycle);
}
}
