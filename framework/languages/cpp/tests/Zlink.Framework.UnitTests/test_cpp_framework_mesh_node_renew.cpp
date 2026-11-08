/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "../support/location_reclaim_clock.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <span>
#include <string_view>

namespace zlink::framework::runtime
{
struct in_memory_location_repository_test_access_t
{
    static void expire (in_memory_location_repository_t &repository, const std::string &owner)
    {
        repository._lane.run ([&] { repository._leases.at (owner).lease_expires_at = {}; }).get ();
    }

    static void advance_descriptor_generation (in_memory_location_repository_t &repository)
    {
        repository._lane.run ([&] { ++repository._mesh_nodes.begin ()->second.lease_generation; })
          .get ();
    }
};

struct in_memory_store_continuation_test_access_t
{
    static void expire (in_memory_location_store_t &store, const std::string &owner)
    {
        store._lane
          .run ([&] {
              const auto key = std::string ("owner-lease") + '\0' + owner;
              store._values.at (key).expires_at = std::chrono::system_clock::time_point{};
          })
          .get ();
    }
};
} // namespace zlink::framework::runtime

namespace
{
using namespace zlink::framework;
using namespace zlink::framework::runtime;
using namespace std::chrono_literals;

class MeshNodeRenew : public ::testing::TestWithParam<bool>
{
  protected:
    in_memory_location_store_t opaque;
    provider_location_repository_t provider{opaque};
    in_memory_location_repository_t memory;
    location_reclaim_test_access_t monotonic;

    location_repository_t &store ()
    {
        return GetParam () ? static_cast<location_repository_t &> (provider) : memory;
    }

    void SetUp () override { monotonic.bind (store ()); }

    location_owner_token_t claim (const std::string &id = "owner")
    {
        return std::get<owner_lease_claimed_t> (
                 store ().claim_owner_lease (id, 1h).result ().value ())
          .token;
    }

    location_owner_token_t reclaim (const location_owner_token_t &owner)
    {
        if (GetParam ())
            in_memory_store_continuation_test_access_t::expire (opaque, owner.owner_id);
        else
            in_memory_location_repository_test_access_t::expire (memory, owner.owner_id);
        EXPECT_TRUE (std::holds_alternative<owner_lease_missing_t> (
          store ().read_owner_lease (owner.owner_id).result ().value ()));
        const auto next = claim (owner.owner_id);
        EXPECT_GT (next.lease_generation, owner.lease_generation);
        return next;
    }

    static mesh_node_descriptor_t descriptor (const location_owner_token_t &owner)
    {
        mesh_node_descriptor_t value;
        value.mesh_name = "recovery";
        value.rid = zlink::routing_id_t::from ("node");
        value.lifecycle_generation = 17;
        value.descriptor_revision = 1;
        value.endpoint = "tcp://127.0.0.1:7001";
        value.owner_id = owner.owner_id;
        value.lease_generation = owner.lease_generation;
        value.object_role = object_role_t::server;
        value.state = framework_runtime_state_t::serving;
        value.security_identity = "node";
        value.entry_spot_id = "entry";
        return value;
    }

    location_write_status_t write (const mesh_node_descriptor_t &value,
                                   location_write_intent_t intent = location_write_intent_t::renew)
    {
        return store ().update_mesh_node (value, intent).result ().value ().status;
    }

    mesh_node_descriptor_t read ()
    {
        return store ().list_mesh_nodes ("recovery").result ().value ().items.at (0);
    }
};

TEST_P (MeshNodeRenew, SameLifecycleNewLeaseRenewsAndReplaysWithoutChangingIntent)
{
    const auto owner = claim ();
    auto value = descriptor (owner);
    ASSERT_EQ (write (value, location_write_intent_t::new_claim), location_write_status_t::stored);
    ++value.descriptor_revision;
    ASSERT_EQ (write (value), location_write_status_t::stored);
    const auto next = reclaim (owner);
    ASSERT_EQ (write (value), location_write_status_t::ignored_stale);
    value.lease_generation = next.lease_generation;
    EXPECT_EQ (write (value), GetParam () ? location_write_status_t::ignored_stale
                                          : location_write_status_t::rejected_conflict);
    ++value.descriptor_revision;
    ASSERT_EQ (write (value), location_write_status_t::stored);
    EXPECT_EQ (read ().lease_generation, next.lease_generation);
    EXPECT_EQ (read ().lifecycle_generation, value.lifecycle_generation);
    EXPECT_EQ (read ().descriptor_revision, value.descriptor_revision);
    // §10 confirms the original request by content; timestamps are not identity.
    value.updated_at = std::chrono::system_clock::time_point{};
    EXPECT_EQ (write (value), location_write_status_t::stored);
    value.state = framework_runtime_state_t::draining;
    ++value.descriptor_revision;
    ASSERT_EQ (write (value), location_write_status_t::stored);
    EXPECT_EQ (read ().state, framework_runtime_state_t::draining);
    value.state = framework_runtime_state_t::serving;
    ++value.descriptor_revision;
    EXPECT_EQ (write (value), location_write_status_t::stored);
    EXPECT_EQ (store ().remove_mesh_node ({value.mesh_name, value.rid}, owner).result ().value (),
               location_write_status_t::ignored_stale);
    EXPECT_EQ (store ().remove_mesh_node ({value.mesh_name, value.rid}, next).result ().value (),
               location_write_status_t::stored);
}

TEST_P (MeshNodeRenew, NewLeaseCanPublishDrainingDirectly)
{
    const auto owner = claim ();
    auto value = descriptor (owner);
    ASSERT_EQ (write (value, location_write_intent_t::new_claim), location_write_status_t::stored);
    value.lease_generation = reclaim (owner).lease_generation;
    ++value.descriptor_revision;
    value.state = framework_runtime_state_t::draining;
    ASSERT_EQ (write (value), location_write_status_t::stored);
    EXPECT_EQ (read ().state, framework_runtime_state_t::draining);
    EXPECT_EQ (read ().lease_generation, value.lease_generation);
}

TEST_P (MeshNodeRenew, RecoveryKeepsOwnerLifecycleRevisionAndImmutableFences)
{
    const auto owner = claim ();
    auto value = descriptor (owner);
    ASSERT_EQ (write (value, location_write_intent_t::new_claim), location_write_status_t::stored);
    value.lease_generation = reclaim (owner).lease_generation;
    ++value.descriptor_revision;
    ASSERT_EQ (write (value), location_write_status_t::stored);
    const auto other = claim ("other");
    const auto conflict = GetParam () ? location_write_status_t::ignored_stale
                                      : location_write_status_t::rejected_conflict;
    const std::pair<const char *, std::function<void (mesh_node_descriptor_t &)>> changes[]{
      {"owner",
       [&] (auto &v) {
           v.owner_id = other.owner_id;
           v.lease_generation = other.lease_generation;
       }},
      {"lifecycle", [] (auto &v) { ++v.lifecycle_generation; }},
      {"endpoint", [] (auto &v) { v.endpoint += "/changed"; }},
      {"entry", [] (auto &v) { v.entry_spot_id = "changed"; }},
      {"security", [] (auto &v) { v.security_identity = "changed"; }}};
    for (const auto &[name, change] : changes) {
        SCOPED_TRACE (name);
        auto invalid = value;
        ++invalid.descriptor_revision;
        change (invalid);
        EXPECT_EQ (write (invalid), std::string_view (name) == "owner"
                                      ? location_write_status_t::ignored_stale
                                      : conflict);
        EXPECT_EQ (read ().descriptor_revision, value.descriptor_revision);
    }
    auto invalid = value;
    --invalid.descriptor_revision;
    EXPECT_EQ (write (invalid), location_write_status_t::ignored_stale);
    invalid = value;
    invalid.state = framework_runtime_state_t::draining;
    EXPECT_EQ (write (invalid), conflict);
    invalid = value;
    invalid.rid = zlink::routing_id_t::from ("missing");
    EXPECT_EQ (write (invalid), location_write_status_t::ignored_stale);
    ++value.descriptor_revision;
    EXPECT_EQ (write (value), location_write_status_t::stored);
}

TEST_P (MeshNodeRenew, LiveRequestCannotRegressStoredLeaseGeneration)
{
    const auto owner = claim ();
    auto value = descriptor (owner);
    ASSERT_EQ (write (value, location_write_intent_t::new_claim), location_write_status_t::stored);
    // Keep the request lease live while placing a newer generation in the row,
    // so the row's regression guard is tested independently of lease validation.
    if (GetParam ()) {
        const store_key_t key{std::string ("mesh-node") + '\0' + value.mesh_name + '\0'
                              + value.rid.to_hex ()};
        const auto row = std::get<store_found_t> (opaque.read (key).result ().value ()).value;
        auto record = nlohmann::json::parse (row.bytes.begin (), row.bytes.end ());
        record[location_record_fields::leaseGeneration] =
          std::to_string (owner.lease_generation + 1);
        record[location_record_fields::descriptor][location_record_fields::leaseGeneration] =
          std::to_string (owner.lease_generation + 1);
        const auto text = record.dump ();
        const auto data = std::as_bytes (std::span (text));
        const auto applied = opaque
                               .write ({{store_version_condition_t{key, row.version}},
                                        {store_put_t{key, {data.begin (), data.end ()}, {}}}})
                               .result ()
                               .value ();
        ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (applied));
    } else {
        in_memory_location_repository_test_access_t::advance_descriptor_generation (memory);
    }
    ++value.descriptor_revision;
    EXPECT_EQ (write (value), location_write_status_t::ignored_stale);
    EXPECT_EQ (read ().lease_generation, owner.lease_generation + 1);
    EXPECT_EQ (read ().descriptor_revision, 1u);
}

TEST_P (MeshNodeRenew, OtherDescriptorKindsStillRequireTheSameLease)
{
    const auto owner = claim ();
    client_server_server_descriptor_t client;
    client.channel_name = "clients";
    client.server_rid = zlink::routing_id_t::from ("server");
    client.lifecycle_generation = 17;
    client.descriptor_revision = 1;
    client.endpoint = "tcp://127.0.0.1:7002";
    client.state = framework_runtime_state_t::serving;
    client.security_identity = "server";
    client.owner_id = owner.owner_id;
    client.lease_generation = owner.lease_generation;
    fanout_publisher_descriptor_t fanout;
    fanout.channel_name = "events";
    fanout.publisher_rid = zlink::routing_id_t::from ("publisher");
    fanout.lifecycle_generation = 17;
    fanout.descriptor_revision = 1;
    fanout.endpoint = "tcp://127.0.0.1:7003";
    fanout.state = framework_runtime_state_t::serving;
    fanout.security_identity = "publisher";
    fanout.owner_id = owner.owner_id;
    fanout.lease_generation = owner.lease_generation;
    ASSERT_EQ (store ()
                 .update_client_server (client, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    ASSERT_EQ (store ()
                 .update_fanout_publisher (fanout, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    ++client.descriptor_revision;
    ++fanout.descriptor_revision;
    EXPECT_EQ (store ()
                 .update_client_server (client, location_write_intent_t::renew)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    EXPECT_EQ (store ()
                 .update_fanout_publisher (fanout, location_write_intent_t::renew)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    const auto next = reclaim (owner);
    client.lease_generation = fanout.lease_generation = next.lease_generation;
    ++client.descriptor_revision;
    ++fanout.descriptor_revision;
    EXPECT_EQ (store ()
                 .update_client_server (client, location_write_intent_t::renew)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::ignored_stale);
    EXPECT_EQ (store ()
                 .update_fanout_publisher (fanout, location_write_intent_t::renew)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::ignored_stale);
}

INSTANTIATE_TEST_SUITE_P (Backends, MeshNodeRenew, ::testing::Bool ());
} // namespace
